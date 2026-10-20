#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Controller buttons for the launcher's Controls page, read through the SDL3 library the game
ships (bin/SDL3.dll), so every controller the game knows (Xbox, PlayStation, Switch) reports the
same button names the game's input.ini uses (bbport_input_config.CONTROLLER_NAMES).

Pads.pressed() polls; the Controls page asks it every few milliseconds while a slot waits for a
button. Without the library or a controller it reports nothing and says why (Pads.problem)."""
import ctypes
import os
import sys
from pathlib import Path

SDL_INIT_GAMEPAD = 0x2000
# SDL_GamepadButton values -> the game's names (src/runtime_input_config.c kButtonNames).
BUTTONS = {0: 'cross', 1: 'circle', 2: 'square', 3: 'triangle', 4: 'back', 6: 'options',
           7: 'l3', 8: 'r3', 9: 'l1', 10: 'r1', 11: 'pad_up', 12: 'pad_down', 13: 'pad_left',
           14: 'pad_right', 16: 'r4', 17: 'l4', 18: 'r5', 19: 'l5'}
# SDL_GamepadAxis trigger values -> names; a trigger counts as pressed past half way.
TRIGGERS = {4: 'l2', 5: 'r2'}
TRIGGER_PRESSED = 16000


def _library_paths(port_dir):
    port_dir = Path(port_dir)
    yield port_dir / 'bin' / 'SDL3.dll'
    yield port_dir / 'out' / 'SDL3.dll'
    yield Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin' / 'SDL3.dll'
    yield Path('libSDL3.so.0')


class Pads:
    def __init__(self, port_dir):
        self.sdl, self.pads, self.problem = None, {}, None
        for path in _library_paths(port_dir):
            if path.name.endswith('.dll') and not path.is_file():
                continue
            try:
                self.sdl = ctypes.CDLL(str(path))
                break
            except OSError:
                continue
        if not self.sdl:
            self.problem = 'SDL3 library not found'
            return
        sdl = self.sdl
        sdl.SDL_SetHint.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        sdl.SDL_Init.argtypes, sdl.SDL_Init.restype = [ctypes.c_uint32], ctypes.c_bool
        sdl.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
        sdl.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_uint32)
        sdl.SDL_OpenGamepad.argtypes, sdl.SDL_OpenGamepad.restype = [ctypes.c_uint32], ctypes.c_void_p
        sdl.SDL_GamepadConnected.argtypes, sdl.SDL_GamepadConnected.restype = [ctypes.c_void_p], ctypes.c_bool
        sdl.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
        sdl.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
        sdl.SDL_GetGamepadButton.restype = ctypes.c_bool
        sdl.SDL_GetGamepadAxis.argtypes, sdl.SDL_GetGamepadAxis.restype = [ctypes.c_void_p, ctypes.c_int], ctypes.c_int16
        sdl.SDL_free.argtypes = [ctypes.c_void_p]
        sdl.SDL_GetError.restype = ctypes.c_char_p
        # The launcher's window has the focus, not an SDL one: read the pads anyway.
        sdl.SDL_SetHint(b'SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS', b'1')
        if not sdl.SDL_Init(SDL_INIT_GAMEPAD):
            self.problem = f'SDL: {sdl.SDL_GetError().decode(errors="replace")}'
            self.sdl = None

    def _refresh(self):
        """Opens newly connected controllers, forgets unplugged ones."""
        sdl = self.sdl
        sdl.SDL_PumpEvents()
        count = ctypes.c_int(0)
        ids = sdl.SDL_GetGamepads(ctypes.byref(count))
        current = {ids[i] for i in range(count.value)} if ids else set()
        if ids:
            sdl.SDL_free(ids)
        for pad_id in list(self.pads):
            if pad_id not in current or not sdl.SDL_GamepadConnected(self.pads[pad_id]):
                sdl.SDL_CloseGamepad(self.pads.pop(pad_id))
        for pad_id in current - set(self.pads):
            handle = sdl.SDL_OpenGamepad(pad_id)
            if handle:
                self.pads[pad_id] = handle

    def pressed(self):
        """The set of button names held on any controller right now."""
        if not self.sdl:
            return set()
        self._refresh()
        self.sdl.SDL_UpdateGamepads()
        held = set()
        for handle in self.pads.values():
            held |= {name for button, name in BUTTONS.items() if self.sdl.SDL_GetGamepadButton(handle, button)}
            held |= {name for axis, name in TRIGGERS.items()
                     if self.sdl.SDL_GetGamepadAxis(handle, axis) > TRIGGER_PRESSED}
        return held

    def connected(self):
        if not self.sdl:
            return 0
        self._refresh()
        return len(self.pads)


if __name__ == '__main__':  # python bbport_gamepad.py <port dir>: prints buttons as they change
    import time
    pads = Pads(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
    print('problem:', pads.problem, 'controllers:', pads.connected())
    last = set()
    for _ in range(600):
        now = pads.pressed()
        if now != last:
            print(sorted(now))
            last = now
        time.sleep(0.02)
