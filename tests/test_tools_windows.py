"""CPU unit tests for Windows diagnostic tools in tools/windows/."""
import csv
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# Resolve ROOT repo-relative from this file: tests/ -> root
ROOT = Path(__file__).resolve().parent.parent
TOOLS_WIN = ROOT / "tools" / "windows"


class AnalyzeFgGuidesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def create_fixture(self, rw=16, rh=8, ow=32, oh=16, depth_vals=None, motion_vals=None, out_rgba_sizes=None):
        manifest = {
            "width": rw,
            "height": rh,
            "output_width": ow,
            "output_height": oh,
            "depth_format": 100,
            "motion_format": 83,
            "render_frame_index": 42,
            "frame_index": 42,
            "reflex_frame_id": 100,
            "frame_ms": 16.6667,
            "reset": False,
            "generated_count": 1,
            "jitter": [0.0, 0.0],
            "camera": {"near_plane": 0.1, "far_plane": 1000.0, "vertical_fov": 0.785},
        }
        (self.dir / "guides_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

        # R32 depth
        if depth_vals is None:
            depth_vals = [0.5] * (rw * rh)
        depth_bytes = struct.pack(f"<{len(depth_vals)}f", *depth_vals)
        (self.dir / "depth.bin").write_bytes(depth_bytes)

        # RG16 motion (half float 'e')
        if motion_vals is None:
            motion_vals = [0.0] * (rw * rh * 2)
        motion_bytes = struct.pack(f"<{len(motion_vals)}e", *motion_vals)
        (self.dir / "motion.bin").write_bytes(motion_bytes)

        # Output RGBA8 files
        if out_rgba_sizes is None:
            out_rgba_sizes = {
                "color_rgba8.raw": ow * oh * 4,
                "full_rgba8.raw": ow * oh * 4,
                "generated_1_rgba8.raw": ow * oh * 4,
            }
        for name, size in out_rgba_sizes.items():
            (self.dir / name).write_bytes(b"\x80" * size)

    def test_valid_guides_and_outputs(self):
        self.create_fixture()
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"STDOUT: {res.stdout}\nSTDERR: {res.stderr}")
        self.assertIn("Render dimensions: 16x8, Output dimensions: 32x16", res.stdout)
        self.assertIn("Depth (R32): min=0.500000, max=0.500000", res.stdout)
        self.assertIn("color_rgba8.raw): 2048 bytes verified", res.stdout)

    def test_reject_nonfinite_depth(self):
        self.create_fixture(depth_vals=[math.nan] + [0.5] * (16 * 8 - 1))
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("non-finite count=1", res.stdout)

    def test_reject_out_of_range_depth(self):
        self.create_fixture(depth_vals=[1.5] + [0.5] * (16 * 8 - 1))
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("Depth values outside 0..1: 1", res.stdout)

    def test_reject_nonfinite_motion(self):
        self.create_fixture(motion_vals=[math.inf] + [0.0] * (16 * 8 * 2 - 1))
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("Motion non-finite component count: 1", res.stdout)

    def test_reject_rgba_output_size_mismatch(self):
        # Expected is 32*16*4 = 2048, give wrong size
        self.create_fixture(out_rgba_sizes={"color_rgba8.raw": 1000})
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("size mismatch", res.stdout)

    def test_reject_missing_manifest(self):
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("manifest not found", res.stdout)


class AnalyzePresentmonTests(unittest.TestCase):
    def test_valid_presentmon_csv_parsing(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            writer = csv.writer(f)
            writer.writerow(["Application", "ProcessID", "SwapChainAddress", "TimeInMs", "MsBetweenDisplayChange", "MsBetweenPresents", "PresentMode"])
            for t in range(0, 1000, 16):
                writer.writerow(["bbport.exe", "1234", "0xABCD", str(t), "16.666", "16.666", "Composed: Independent Flip"])
            f_path = Path(f.name)

        try:
            res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_presentmon.py"), str(f_path), "--after-ms", "100", "--before-ms", "800"],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            self.assertIn("bbport.exe", res.stdout)
            self.assertIn("actual display cadence", res.stdout)
            self.assertIn("display interval ms", res.stdout)
        finally:
            f_path.unlink(missing_ok=True)


class AnalyzeReflexTests(unittest.TestCase):
    def test_valid_reflex_timing_log_parsing(self):
        lines = []
        for i in range(1, 20):
            # Timestamps in microseconds (16 ms spacing)
            sim0 = i * 16000
            inp = sim0 + 500
            sim1 = sim0 + 2000
            ren0 = sim1 + 1000
            ren1 = ren0 + 4000
            pre0 = ren1 + 500
            pre1 = pre0 + 100
            gpu0 = ren0 + 2000
            gpu1 = pre0 + 3000
            lines.append(f"reflex timing: id={i} input={inp} sim={sim0}..{sim1} render={ren0}..{ren1} present={pre0}..{pre1} gpu={gpu0}..{gpu1}")

        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write("\n".join(lines))
            f_path = Path(f.name)

        try:
            res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_reflex.py"), str(f_path), "--tail-seconds", "60"],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            data = json.loads(res.stdout.split("Driver timestamps")[0])
            self.assertEqual(data["frames"], 19)
            self.assertEqual(data["invalid_reports"], 0)
            self.assertIn("simulation_to_present_ms", data)
        finally:
            f_path.unlink(missing_ok=True)


class AnalyzeStabilityTests(unittest.TestCase):
    def test_valid_stability_csv_parsing(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            writer = csv.writer(f)
            writer.writerow(["seconds", "private_bytes", "working_set", "handles", "threads", "gpu_global_mib", "gpu_util_percent"])
            for s in range(300, 600, 30):
                writer.writerow([str(s), str(100 * 1024 * 1024), str(150 * 1024 * 1024), "500", "32", "2048", "75.0"])
            f_path = Path(f.name)

        try:
            res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_stability.py"), str(f_path), "--after", "300", "--before", "600"],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            self.assertIn("private_bytes:", res.stdout)
            self.assertIn("working_set:", res.stdout)
            self.assertIn("slope", res.stdout)
        finally:
            f_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
