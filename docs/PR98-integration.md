# PR #98 integration

Merged [upstream PR #98](https://github.com/Supermedo/bloodborne_pc/pull/98)
at `c6454fbb99947ed6698550a177f06e42476b513b`, based on Windows 1.9.2.
This is a fork integration build, not an official 1.9.3 release.

The update includes the PR's rollback to the 1.8.2 GPU layer with its retained
allocation retry and diagnostics, VRAM/staging improvements, low-VRAM profile,
integrated-GPU handling, audio thread priorities, partial patch compatibility for
modified executables, and Quit to Desktop/Language and debug-font corrections.
Controller rebinding and matching prompts, the launcher benchmark and bundled
benchmark save, optional startup save backups, launcher respec and Rebirth at the
Doll are included.

## Integration choices

- Keep the fork's paced DLSS/FSR FG and Reflex implementation, submission frame IDs,
  simulation boundaries, synchronization and UI composition.
- Keep the bbhost camera, lock-on flicks and menu pointer. The PR's separate camera
  hook and pad-level `mouse_lock_switch` implementation are superseded by these
  existing features and are not enabled alongside them.
- Preserve native neural rendering and its controls.
- Retain the wider semaphore wait-stage array needed by the fork's FG submissions.
  The old renderer's two-element array does not cover all fork submissions.
- Keep the implemented flip APIs and their declarations. Remove the duplicate old
  flip-patching helper restored by the GPU rollback, so flips retain their source IDs.
- Retain physical-output-aware menu coordinates and HUD centering while adopting
  the PR's checks for safe patch groups in modified executables.
- Keep controller actions separate from the fork's single-input PC modifiers.
  Save preserves pending mouse edits and the dictionary used by live UI callbacks
  when keyboard and controller bindings are changed together.
- Keep the fork update channel. Existing configuration, mods, patches and saves
  are preserved during deployment. RTAO remains excluded.

## Validation

The native Windows build and ten native suites pass: direct camera, camera hooks,
menu pointer, input configuration, pad, prompt icons, FG timing/settings, Reflex
and neural settings. The hidden launcher test covers Graphics/Controls, combined
keyboard/controller/mouse saves, presets and repeated Save callbacks.

The Python run executes 277 checks: 213 pass, 57 skip, and seven fail. All seven
also fail on unmodified PR #98 in the same Windows environment; they concern
symlink privileges and Linux-oriented driver/shell fixtures. With local game data,
the Rebirth code-linking and talk-script round-trip/menu checks also pass (three
passes, one executable-fixture check skipped).

Short, isolated gameplay regressions cover DLSS 2x FG, FSR 2x FG, neural rendering,
direct mouse input and menu/overlay presentation. Test captures and exact build
hashes are kept with the local deployment artifacts. These checks do not certify
all hardware, long-session stability, latency improvements, or the complete
Rebirth/benchmark gameplay flow.
