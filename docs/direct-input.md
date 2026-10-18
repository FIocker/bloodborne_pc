# Direct mouse input: the camera and the game menus

Two things make the mouse behave like a PC mouse in bbport, and both are ported from
[droogie/bbhost](https://github.com/droogie/bbhost) at `566184a`:

| Piece | bbport | bbhost |
|---|---|---|
| The direct camera's step (how a mouse count becomes a camera angle) | `src/mouse_camera_step.h`, in `src/runtime_mouse_camera.c` | `src/engine/mouse_camera_step.h` |
| The camera hooks, the pad-facing state, the lock-on flick | `src/runtime_mouse_camera.c/.h` | `src/engine/mouse_camera.cpp`, `src/engine/mouse_camera_stubs.cpp` |
| The game-menu pointer (hit testing, press, scroll) | `src/menu_pointer.cpp/.h` | bbhost's game-menu pointer |

`src/runtime_mouse_camera.c` is the C half, and the two callers of its API are the pad read
(`src/runtime_pad.c`) and the window; `src/menu_pointer.cpp` is C++ because it drives the game's
own UI objects. `src/probe.c` calls
`mouse_camera_bind_image()` and `menu_pointer_bind_image()` once, after the image is loaded and
patched and before the guest runs.

## What the direct camera does

Dark Souls III's mouse camera is an **angle per count** of the mouse's own movement, added to the
follow camera's pitch and yaw once a frame, after the stick and only when the stick has not turned
the camera: no frame time, no dead zone, no cap and no acceleration. Bloodborne's
`ChrExFollowCam::Update` is the same function without the mouse, so the step goes where the
stick's own step stores the free camera's angles, and the update's prologue takes the counts.
That is why the sensitivity is in **degrees a mouse count** and not in degrees a second: the same
movement turns the camera the same amount at 30 FPS and at 120 FPS.

```ini
mouse_to_joystick = right          # required: this is how mouse look is turned on at all (F7)
mouse_camera = direct              # direct | stick | off
mouse_direct_sensitivity = 5       # 0..10, Dark Souls III's own scale: 5 is 0.25 degrees a count
mouse_camera_sensitivity = 1.0     # 0.05..20, a multiplier on top of it
mouse_camera_scale = 1.0           # 0.05..20, a second multiplier, for finer control
mouse_camera_invert_x = off        # 0|1|on|off|true|false|yes|no
mouse_camera_invert_y = off
mouse_hold_auto_rotation = on      # hold the camera's own turns as the character moves
```

| Setting | Meaning |
|---|---|
| `mouse_camera` | `direct` writes the game's camera angles from mouse motion. `stick` keeps the mouse-to-joystick emulation (`legacy` is accepted as another name for it). `off` leaves the camera to the pad entirely. **Absent, `direct` is the mode** (an explicit line always wins). |
| `mouse_direct_sensitivity` | `0`..`10`, rounded. The angle a count turns is `0.2 * (s * 0.15 + 0.5)` degrees: 0.1 at 0, **0.25 at 5** (the default), 0.4 at 10. Out of range is a warning, and the default is kept. |
| `mouse_camera_sensitivity`, `mouse_camera_scale` | Multipliers applied on top, `0.05`..`20` each, `1.0` by default. Files written before the DS3 scale existed set `mouse_camera_sensitivity`; both are applied. |
| `mouse_camera_invert_x`, `mouse_camera_invert_y` | Flip one axis of the mouse's camera motion. They apply to the stick path too, not only to `direct`. |
| `mouse_hold_auto_rotation` | On (the default), the camera's own turns as the character moves are held while the mouse turns the camera. Off, the game keeps them. |
| `mouse_look_feel` | `linear` (default) or `legacy`: how mouse motion becomes the right stick **when `direct` is not the route** (not installed, `mouse_camera = stick`, or a different game version). `linear` adds the counts to the stick with no dead zone, no acceleration and no smoothing; `legacy` is the shadPS4 `EmulateJoystick` formula this port used before. |
| `mouse_look_gain` | `0.01`..`20` (default `1.1`): stick units added per mouse count in the `linear` feel. |
| `mouse_movement_params` | `deadzone_offset, speed, speed_offset` (`0.5, 1, 0.125`): the `legacy` formula only. `deadzone_offset` is still parsed for compatibility but no longer affects anything. |
| `hotkey_toggle_mouse_to_joystick` | Key that turns mouse look on and off (`f7` by default). While it is off the mouse is inert and the right stick/arrow keys drive the camera. |
| `hotkey_reload_inputs` | Key that reloads `input.ini` without restarting (`f8` by default). |

The effective angle is `mouse_camera_degrees_per_count(mouse_direct_sensitivity)` times
`mouse_camera_sensitivity` times `mouse_camera_scale`; a multiplier outside `(0, 100]` is not a
scale and falls back to the plain DS3 value.

**Lock-on.** While a target is locked, the game drives the camera, so the mouse does not write
angles. A fast movement (a flick) switches the target instead: the counts of the updates the free
camera did not run in are read at the pad sample, and a movement above 3000 counts a second —
Dark Souls III's 50 counts a frame at 60 FPS, kept as a rate so Bloodborne's 30 FPS asks for the
same movement — is delivered as a right-stick flick of 80 ms, in the direction of the movement.
Below 1500 counts a second the switch rearms, and at most one switch happens every 0.5 s. Slow
movements never switch, and never accumulate into one.

**Fallback.** If the image is not the verified 1.09 build, or a hook site has been changed by
another patch, nothing is written: one line is logged
(`Mouse camera: refused, ...; the mouse turns the camera through the stick`) and the port stays on
the stick path, with `mouse_look_feel`/`mouse_look_gain` deciding how that feels.

## The launcher

The Controls page writes all of the above. Its `Mouse look` section starts with `Enable mouse
look`, the on/off switch (`mouse_to_joystick`, and the same thing F7 toggles in game);
`Mouse controls` picks the stick the emulation uses, and `Camera input` picks `Direct camera
(bbhost)` or `Stick emulation`. `Direct camera sensitivity` is the 0..10 DS3 scale, `Direct
camera multiplier` is `mouse_camera_sensitivity`, and the two inversion checkboxes and `Hold
automatic camera rotation while using the mouse` map to their `input.ini` lines. The
stick-emulation settings (`Stick response`, `Linear stick gain`, `Legacy sensitivity`, `Legacy
minimum turn speed`) sit in their own section below, because they only apply when the direct
camera is not the route.

`Apply bbhost bindings`, on the `Keyboard & mouse layout` row, is an **explicit** preset: it
stages bbhost's PC layout (WASD, Space to dodge, E to interact, LShift + attack for a strong
attack, mouse buttons for the attacks, Escape for Options, IJKL camera controls, arrow-key d-pad, Enter/Backspace for the
menu's decide/return) and replaces the keyboard and mouse lines when the page is saved. It is the
only path that replaces a layout wholesale — an update never rewrites `input.ini`, and the page's
own rows only change the binding the player sets there — and controller bindings are kept either
way. The menu's two keys are used by the runtime in game menus only, so `Enter` still reaches the
game as Options elsewhere.

## How the hooks are placed

Every hook is placed only on the verified Bloodborne 1.09 image, behind the same segment
fingerprint the aspect-ratio patches use:

```
BB_INPUT_EBOOT_SEGMENTS=cbe97cb9e7b830195c93e7ed139cbaaeea66302da82f1b39ca31f90a3ceccc52
```

`run.py` publishes it for every launch, taken from the eboot as `prepare.py` decoded it and before
any patch is applied, so it names the game image itself. `BB_MOUSE_CAMERA_EBOOT_SEGMENTS` and
`BB_ASPECT_EBOOT_SEGMENTS` are older names and are still read as fallbacks.

The camera's sites, at the addresses bbhost's own hooks use (the linked image keeps the eboot's
layout, so a site is at `image + (address - 0x400000)`):

| Site | Address | What is displaced |
|---|---|---|
| `ChrExFollowCam::Update` prologue | `0x183ac60` | 20 bytes (`push rbp; mov rbp, rsp; push r15..rbx; sub rsp, 0x3d8`) |
| The free camera's store of pitch and yaw | `0x183ce54` | 19 bytes, resumed at `0x183ce67` |
| The fast turn's two exits | `0x183f9bc`, `0x183fabf` | 14 bytes each |
| The camera's own turns as the character moves | `0x183c6e8`, `0x183c870`, `0x183c984`, `0x183dde6` | 9 bytes each (a `vmovss [r13+N], xmm`) |

Every instruction is read back and matched **before any of them is changed**, and if a write
fails the originals are restored before the guest is allowed to run. The four "own turns" stores
are the ones the community patch *Disable Camera Auto Rotation via Movement* makes no-ops: three
of them are held while the mouse turns the camera (all but the auto turn's step, which only R3's
recentre reaches), and if a patch already nopped them the port leaves them as they are and says
so once.

The callbacks run from register-preserving stubs (`bb_mouse_store_stub`, `bb_mouse_ramp_stub`,
`bb_mouse_update_stub`), written as inline assembly in `src/runtime_mouse_camera.c` so the
assembler and linker own every relocation. All guest volatile GPRs and all 16 XMM registers
survive the host callback; the prologue and the store have different continuations.

The window thread is where the counts come from: `gpu/shim/window.cpp`'s event pump sends SDL's
relative motion to `mouse_camera_motion()` while the direct camera is active, and to the shared
`mouse.dx/dy` the stick path reads when it is not. `mouse_camera_set_enabled()` is called on every
poll with `direct camera wanted && mouse captured`, and turning it off drops whatever was in
flight, so the game's own camera is left alone the moment the mouse is not the route. Mouse
capture itself needs mouse look on, the window focused, the settings menu and text entry closed,
and no game menu open.

## The game-menu pointer

The same fingerprint guards the menu pointer, and it validates more before it writes: the list
update's vtable slot (`kListVTable + 3*8` must still point at `0x1ecff30`), the five non-vtable
callers (four `call`s and the inventory item list's tail `jmp`), and 14 bytes of padding before
`sub_1ec1e10` that the jump to the host is written into. A mismatch logs one line
(`Pointer: refused ...`) and leaves the game's UI untouched.

With the hooks in place the port drives the game's own list cursor:

- **Coordinates.** `menu_pointer_stage()` fits the whole output image into the window (fullscreen
  and windowed resize share the mapping) and then unprojects the centered 1920x1080 Scaleform
  stage, so a 21:9 or 32:9 output keeps the HUD 16:9 centered and the pointer still lands on the
  right row. Points outside the image, or in the letterbox bars, select nothing.
- **When it is live.** `menu_pointer_in_menu()` is true in the front end as a whole, and in the
  world while the flow has a menu open or a list had focus within the last 400 ms. A position is
  only valid while the window is focused, the mouse is not in relative mode, and no overlay owns
  input.
- **Buttons.** Left click is a press, right click is the menu's back, and the wheel scrolls the
  list (d-pad up/down/left/right, the bits the game's list update already reads).
- **Ownership.** The device that was used last owns the pointer (`menu_pointer_device()`, fed by
  the window's events; analog drift never takes it). While a controller owns it the pointer's own
  press is suppressed, and `mouse_hold_auto_rotation` is not applied either.
- **Decide and return.** The menu's confirm and back inputs (`menu_confirm`, `menu_back` in
  `input.ini`, and the launcher's `Enter`/`Backspace` in the bbhost preset) press the game's own
  decide and return buttons while a menu is open, and the port reads the guest's region value to
  pick the layout: region 5 decides with Circle and returns with Cross, otherwise Cross decides
  and Circle returns.

`strong` (`LShift` in the bbhost preset) is the light attack's heavy modifier: while it is held,
`r1` is delivered as `r2`. It is scoped to gameplay — inside a game menu the light attack stays a
light attack.

## Diagnostics

- `BB_MOUSE_TRACE=1` prints one line a second with the samples, the counts, whether they went to
  the camera or to the stick, and the hook's state.
- At exit the port prints one report line: camera updates, how many the mouse turned, how many the
  stick had, the lock-on flicks, and how often auto-rotation was held.
- `mouse_camera_set_enabled()` logs on and off transitions (the first few, and every one with
  `BB_MOUSE_TRACE=1`), with the reason the hooks are or are not placed.

## Tests

The step is a host-side function with no guest and no GPU, so it is tested directly:

```
ninja -C out/gpu direct-mouse-test && out/gpu/direct-mouse-test
ninja -C out/gpu menu-pointer-test && out/gpu/menu-pointer-test
```

`tests/test_direct_mouse.c` links `src/runtime_mouse_camera.c` and pins the angle a count turns
(one sample and the same counts split over many updates are the same turn), the axis signs, the
pitch limits and the yaw wrap, the stick's priority, the drop when the camera is off, the lock-on
count routing and the flick's switch/hold/rearm/cooldown, the camera object's fields
(`+0x140`/`+0x144`/`+0x1ec`/`+0x1f0` and the input byte), and the published flags and counters.
`tests/test_menu_pointer.cpp` covers the pointer's stage mapping, and `tests/test_mouse_hooks.c`
(Windows, `mouse-hooks-test`) runs the actual trampolines on a synthetic image: the fingerprint
and byte guards refusing to touch the guest code, the two different continuations, the store's
flags surviving the host callback, and an update→store round trip. On the pad side,
`tests/test_pad.c` checks the direct and linear routes, what the runtime publishes to the step,
and the menu's gating of the strong modifier and its own confirm/back inputs, while
`tests/test_input_config.c` covers every setting above, including the invalid values that must
keep the defaults.

## Notes

- `docs/INPUT.md` has the rest of the input configuration (bindings, deadzones, stick shaping).
- The direct camera needs the 1.09 executable; on any other build the port logs one line and uses
  the stick path, so a config written for this feature still loads everywhere.
- The port's older camera path (`gpu/shim/bloodborne_cam.cpp` with its sampler thread,
  `gpu/shim/bbport_mouse.cpp`) is no longer started by anything; the window's SDL events are the
  only source of mouse motion.
