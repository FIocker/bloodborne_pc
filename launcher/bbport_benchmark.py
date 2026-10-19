"""Benchmark: the same route on the same save for every player, with their own settings.

The launcher (Advanced > Run benchmark, or Bloodborne.exe --benchmark) puts the benchmark save
(benchmark/savedata) in place of the player's, starts the game offline and uncapped, and drives
it through BB_PAD_FILE: the title menu, then a fixed route in Cathedral Ward (standing still,
turning the camera, walking). The player's save comes back afterwards, also after a crash (the
next start restores it). The results go to <port>/benchmarks/BB-Benchmark-<time>.zip: a summary
(FPS, 1% lows, slowest frames, stalls, texture evictions and write-backs, VRAM), the log,
screenshots taken along the route (stretched polygons and black screens show there) and the
allocator's VRAM statistics.

A recorded route (benchmark/route.txt, made with BB_PAD_RECORD and F9 in the game) replaces the
built-in one when present.
"""
import ctypes
import os
import platform
import re
import shutil
import statistics
import struct
import subprocess
import threading
import time
import zipfile
import zlib
from pathlib import Path

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
BACKUP_NAME = 'savedata.benchmark-backup'

# The built-in route: (phase, seconds, pad tokens, seconds between screenshots).
ROUTE = [
    ('idle', 20, '', 4),
    ('pan', 30, 'rx=255', 5),
    ('walk', 8, 'ly=0', 3),
    ('walk back', 8, 'ly=255', 3),
]
SUMMARY_PHASES = ['idle', 'pan', 'walk']  # 'walk back' counts with 'walk'
SCENE_DRAWS = 500  # draws a frame from which the start area is on screen


class Cancelled(Exception):
    pass


def restore_save(user_dir):
    """Puts the player's save back if a benchmark left its own in place (crash, closed launcher)."""
    user_dir = Path(user_dir)
    backup = user_dir / BACKUP_NAME
    if not backup.is_dir():
        return False
    save = user_dir / 'savedata'
    if save.exists():
        shutil.rmtree(save)
    backup.rename(save)
    return True


class Pad:
    """BB_PAD_FILE writer. The game re-reads the file when its whole-second time or its size
    changes, so each write differs in size from the one before."""

    def __init__(self, path):
        self.path = Path(path)
        self.count = 0
        self.set('')

    def set(self, tokens):
        self.count += 1
        filler = 'x' * (1 + self.count % 2)  # unknown tokens are ignored
        self.path.write_text(f'{tokens} {filler}\n', encoding='ascii')

    def press(self, button, hold=0.25, after=1.0, wait=time.sleep):
        self.set(button)
        wait(hold)
        self.set('')
        wait(after)


class Run:
    def __init__(self, port_dir, data_dir, user_dir, env, command, progress=print):
        self.port_dir = Path(port_dir)
        self.user_dir = Path(user_dir)
        self.data_dir = Path(data_dir)
        stamp = time.strftime('%Y%m%d-%H%M%S')
        self.out = Path(data_dir) / 'benchmarks' / f'BB-Benchmark-{stamp}'
        self.env = dict(env)
        self.command = command
        self.progress = progress
        self.lines = []
        self.marks = {}
        self.cancelled = threading.Event()
        self.process = None
        self.title_seen = threading.Event()
        self.world_seen = threading.Event()
        self.scene_seen = threading.Event()  # frame stats with the area's draws, not a loading screen
        self.exited = threading.Event()
        self.mounts = 0  # read-only save mounts: the title, then the main menu reading the saves

    # -- control ------------------------------------------------------------------------------

    def cancel(self):
        self.cancelled.set()

    def wait(self, seconds):
        if self.cancelled.wait(seconds):
            raise Cancelled()
        if self.exited.is_set():
            raise RuntimeError('the game closed during the benchmark (see log.txt)')

    def wait_for(self, event, timeout, what):
        end = time.monotonic() + timeout
        while not event.is_set():
            self.wait(0.25)
            if time.monotonic() > end:
                raise RuntimeError(f'{what} did not come within {timeout} s (see log.txt)')

    def mark(self, name):
        self.marks.setdefault(name, len(self.lines))
        self.lines.append(f'== Benchmark: {name}\n')

    # -- running ------------------------------------------------------------------------------

    def run(self):
        """Runs the route; returns the zip with the results. Raises on failure (the save is
        restored either way)."""
        save_source = self.port_dir / 'benchmark' / 'savedata'
        if not save_source.is_dir():
            raise RuntimeError(f'the benchmark save is missing ({save_source})')
        self.out.mkdir(parents=True, exist_ok=True)
        restore_save(self.user_dir)
        save = self.user_dir / 'savedata'
        backup = self.user_dir / BACKUP_NAME
        self.user_dir.mkdir(parents=True, exist_ok=True)
        if save.exists():
            save.rename(backup)
        else:
            backup.mkdir()  # nothing to restore but the marker: an empty save folder comes back
        try:
            shutil.copytree(save_source, save)
            self.drive()
        finally:
            self.stop()
            try:
                restore_save(self.user_dir)
            except OSError as error:
                self.progress(f'Could not restore your save yet ({error}); it comes back the '
                              f'next time the launcher starts.')
            (self.out / 'log.txt').write_text(''.join(self.lines), encoding='utf-8')
        return self.finish()

    def drive(self):
        pad = Pad(self.out / 'pad.txt')
        env = self.env
        for key in [k for k in env if k.startswith(('BB_SHADNET', 'BB_THD_', 'BB_COMMUNITY'))]:
            env.pop(key)
        env.pop('BB_NET_BACKEND', None)
        env.pop('BB_SAVE_BACKUPS', None)  # the benchmark's own save: nothing to keep
        env['BB_ONLINE'] = '0'
        env.update(BB_PAD_FILE=str(pad.path), BB_FPS_LOG='1', BB_FRAME_STATS='1',
                   BB_FRAME_DUMP_TRIGGER=str(self.out / 'shot'), BB_DUMP_DIR=str(self.out),
                   BB_VMA_DUMP=str(self.out / 'vram.json'), BB_FPS='uncap', BB_FPS_LIMIT='0',
                   BB_PRESENT_MODE='Immediate')
        route = self.port_dir / 'benchmark' / 'route.txt'
        if route.is_file():
            env['BB_PAD_REPLAY'] = str(route)
        self.progress('Starting the game…')
        self.process = subprocess.Popen(self.command, cwd=self.port_dir, env=env,
                                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        threading.Thread(target=self.read_output, daemon=True).start()

        self.wait_for(self.title_seen, 300, 'The title screen')
        self.progress('Loading the benchmark save…')
        # The title takes input only after its logos (half a minute or more on a slow PC). Down and
        # Cross pick Play Offline; the game then reads the saves (another read-only mount), which
        # says the main menu is up. Until then the pair is tried again (Cross also skips logos).
        self.wait(10)
        end = time.monotonic() + 180
        while self.mounts < 2:
            if time.monotonic() > end:
                raise RuntimeError('the title menu did not answer (see log.txt)')
            pad.press('down', after=0.8, wait=self.wait)
            pad.press('cross', after=0.2, wait=self.wait)
            deadline = time.monotonic() + 5
            while self.mounts < 2 and time.monotonic() < deadline:
                self.wait(0.25)
        # Continue (the first entry with a save) loads the benchmark character.
        self.wait(2)
        for _attempt in range(4):
            pad.press('cross', after=1, wait=self.wait)
            deadline = time.monotonic() + 30
            while not self.world_seen.is_set() and time.monotonic() < deadline:
                self.wait(0.25)
            if self.world_seen.is_set():
                break
        self.wait_for(self.world_seen, 1, 'The game world')
        # A fixed 30 s from the load's start was still the loading screen on a Xeon E3-1230 V2
        # with a cold shader cache, and its idle phase measured that. Wait for the area itself
        # (Cathedral Ward draws ~1400 a frame, loading screens 5-150), then let it settle.
        self.progress('Waiting for the area to load…')
        deadline = time.monotonic() + 240
        while not self.scene_seen.is_set() and time.monotonic() < deadline:
            self.wait(0.5)
        self.wait(15 if self.scene_seen.is_set() else 0)

        if route.is_file():
            self.mark('route')
            pad.set('replay')
            self.shoot_for(self.recorded_length(route), 3, 'Recorded route')
            self.mark('end')
            return
        for phase, seconds, tokens, every in ROUTE:
            self.mark(phase)
            pad.set(tokens)
            self.shoot_for(seconds, every, phase.capitalize())
        pad.set('')
        self.mark('end')

    def recorded_length(self, route):
        last = 0
        for line in route.read_text(encoding='ascii', errors='replace').splitlines():
            parts = line.split()
            if parts and parts[0].isdigit():
                last = int(parts[0])
        return last / 1000 + 2

    def shoot_for(self, seconds, every, label):
        end = time.monotonic() + seconds
        next_shot = time.monotonic() + 1
        while time.monotonic() < end:
            self.progress(f'{label}: {max(0, int(end - time.monotonic()))} s left')
            if time.monotonic() >= next_shot:
                (self.out / 'shot').touch()
                next_shot += every
            self.wait(0.5)

    def read_output(self):
        for raw in iter(self.process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            self.lines.append(text)
            if "mounted at /savedata0 (existing, read-only)" in text:
                self.mounts += 1
                self.title_seen.set()
            if "guest thread 'CSChrThread0' created" in text:
                self.world_seen.set()
            draws = re.search(r'(\d+) draws/frame', text) if text.startswith('Frame stats:') else None
            if draws and self.world_seen.is_set() and int(draws.group(1)) >= SCENE_DRAWS:
                self.scene_seen.set()
        self.exited.set()

    def stop(self):
        if self.process and self.process.poll() is None:
            # run.py and the game under it.
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(self.process.pid)],
                           capture_output=True, creationflags=NO_WINDOW)
            try:
                self.process.wait(15)
            except subprocess.TimeoutExpired:
                pass
        self.exited.wait(10)

    # -- results ------------------------------------------------------------------------------

    def finish(self):
        self.progress('Writing the results…')
        summary = summarize(self.lines, self.marks)
        (self.out / 'summary.txt').write_text(system_info() + summary, encoding='utf-8')
        config = Path(self.env.get('BB_CONFIG') or self.data_dir / 'bbport.ini')
        if config.is_file():
            shutil.copy(config, self.out / 'bbport.ini')
        for raw in sorted(self.out.glob('*.raw')):
            try:
                raw_to_png(raw)
                raw.unlink()
            except (OSError, ValueError):
                pass
        (self.out / 'pad.txt').unlink(missing_ok=True)
        archive = self.out.with_suffix('.zip')
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(self.out.iterdir()):
                zf.write(path, f'{self.out.name}/{path.name}')
        return archive


# -- the summary --------------------------------------------------------------------------------

def summarize(lines, marks):
    order = sorted(marks.items(), key=lambda item: item[1])
    phases = {}
    for (name, start), (_next, end) in zip(order, order[1:]):
        phases.setdefault('walk' if name == 'walk back' else name, []).append((start, end))
    measured = [r for name, ranges in phases.items() for r in ranges]
    first = min((s for s, _e in measured), default=len(lines))
    last = max((e for _s, e in measured), default=len(lines))

    def in_ranges(ranges):
        for start, end in ranges:
            yield from lines[start:end]

    out = ['', 'Results', '-------']
    rows = []
    for name in [p for p in SUMMARY_PHASES if p in phases] + [p for p in phases if p not in SUMMARY_PHASES]:
        rate = [m for m in (re.match(r'Frame rate: ([\d.]+) FPS, slowest frame ([\d.]+) ms', l)
                            for l in in_ranges(phases[name])) if m]
        pacing = [m for m in (re.match(r'Frame pacing: median ([\d.]+) ms.*?p99 ([\d.]+) ms', l)
                              for l in in_ranges(phases[name])) if m]
        rate = rate[1:] if len(rate) > 2 else rate  # the first window began before the phase
        if not rate:
            out.append(f'{name:10} no samples')
            continue
        fps = statistics.mean(float(m[1]) for m in rate)
        worst = max(float(m[2]) for m in rate)
        p99 = statistics.median(float(m[2]) for m in pacing) if pacing else 0
        low = 1000 / p99 if p99 else 0
        rows.append((name, fps, low, worst))
        out.append(f'{name:10} {fps:7.1f} FPS average   {low:6.1f} FPS 1% low   slowest frame '
                   f'{worst:6.1f} ms')
    if rows:
        overall = statistics.mean(r[1] for r in rows)
        out.append(f'{"overall":10} {overall:7.1f} FPS average')

    window = lines[first:last]
    stalls = sum(1 for l in window if l.startswith('== Stall'))
    compiles = sum(int(m[1]) for m in (re.search(r'(\d+) shader/pipeline compiles', l) for l in window) if m)
    evicted = written = dropped = 0
    for l in lines:
        m = re.search(r'(\d+) images evicted, (\d+) written back.*?(\d+) dropped', l)
        if m:
            evicted += int(m[1]); written += int(m[2]); dropped += int(m[3])
    vram = [int(m[1]) for m in (re.match(r'Memory: VRAM (\d+) MiB', l) for l in lines) if m]
    full = sum(1 for l in lines if 'video memory full' in l)
    out += ['',
            f'Stalls during the route: {stalls}',
            f'Shaders compiled during the route: {compiles}'
            + ('  (many: run the benchmark again for steady numbers)' if compiles > 20 else ''),
            f'Texture cache, whole run: {evicted} evicted, {written} written back, {dropped} dropped',
            f'VRAM: peak {max(vram)} MiB (memory lines every minute)' if vram else 'VRAM: no memory line',
            f'"Video memory full" messages: {full}']
    problems = []
    for pattern, label in (('Failed to create graphics pipeline', 'pipelines that failed to build'),
                           ('Fault:', 'crashes'), ('device lost', 'GPU device lost'),
                           ('stale pipelines', 'shader cache entries from another version')):
        count = sum(1 for l in lines if pattern in l)
        if count:
            problems.append(f'  {label}: {count}')
    out += ['', 'Problems in the log:'] + (problems or ['  none'])
    keep = ('GPU:', 'DLSS:', 'Upscaler:', 'Object motion:', 'Output resolution', 'Output ',
            'Patches: FPS', 'Frame generation', 'VideoOut:')
    out += ['', 'Setup (from the log):'] + [
        '  ' + l.rstrip() for l in lines[:first]
        if l.startswith(keep) and not re.match(r'(Frame generation pacing|Object motion: \d)', l)][:20]
    return '\n'.join(out) + '\n'


def system_info():
    info = ['Bloodborne PC port benchmark', '============================',
            f'Date: {time.strftime("%Y-%m-%d %H:%M")}',
            f'OS: {platform.platform()}']
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            info.append(f'CPU: {winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()} '
                        f'({os.cpu_count()} threads)')
    except (ImportError, OSError):
        info.append(f'CPU: {platform.processor()} ({os.cpu_count()} threads)')
    try:
        class MemoryStatus(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong),
                        ('total', ctypes.c_ulonglong), ('available', ctypes.c_ulonglong),
                        ('total_page', ctypes.c_ulonglong), ('available_page', ctypes.c_ulonglong),
                        ('total_virtual', ctypes.c_ulonglong), ('available_virtual', ctypes.c_ulonglong),
                        ('extended', ctypes.c_ulonglong)]
        status = MemoryStatus(length=ctypes.sizeof(MemoryStatus))
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        info.append(f'RAM: {status.total / 2**30:.0f} GB')
    except (AttributeError, OSError):
        pass
    return '\n'.join(info) + '\n'


# -- screenshots ---------------------------------------------------------------------------------

def raw_to_png(raw, max_width=800):
    """The presenter's RGBA dump (output_<n>_<w>x<h>.raw) as a smaller RGB PNG."""
    m = re.search(r'_(\d+)x(\d+)\.raw$', raw.name)
    if not m:
        raise ValueError(raw.name)
    width, height = int(m[1]), int(m[2])
    data = raw.read_bytes()
    if len(data) < width * height * 4:
        raise ValueError(f'{raw.name}: short')
    step = max(1, -(-width // max_width))
    out_w, out_h = len(range(0, width, step)), len(range(0, height, step))
    pixels = memoryview(data).cast('I')
    rows = []
    for y in range(0, height, step):
        row = pixels[y * width:(y + 1) * width:step].tobytes()
        rgb = bytearray(out_w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[0::4], row[1::4], row[2::4]
        rows.append(b'\x00' + bytes(rgb))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', out_w, out_h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(b''.join(rows), 9))
           + chunk(b'IEND', b''))
    raw.with_suffix('.png').write_bytes(png)
