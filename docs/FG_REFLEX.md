# Frame generation and Reflex

This Windows build adds DLSS frame generation, FSR frame generation and NVIDIA Reflex to the existing temporal upscalers. It also includes the aspect-ratio and custom-resolution changes from PR #12.

Choose the generation backend and multiplier in the launcher's Graphics section. The same choices appear in the Insert menu. Generation and Reflex changes are saved for the next game start; reconstruction presets retain their existing behavior. Frame generation defaults to Off.

The six additions are:

1. **DLSS 2x generation:** one interpolated frame between source frames, when supported by the NVIDIA runtime.
2. **NVIDIA Reflex:** Off, On and On + Boost, with input/simulation/render/present markers and driver pacing when the required Vulkan extensions are available.
3. **FSR 2x generation:** an independent backend, usable with the existing reconstruction choices when its Vulkan requirements are met.
4. **DLSS 3x/4x choices:** always selectable, with the effective count bounded by the runtime capability. Unsupported generation falls back to ordinary presentation.
5. **Conservative object motion:** keeps the existing object-motion option, but rejects ambiguous instanced batches rather than matching their transform history by draw order. Camera motion remains available for these batches.
6. **Diagnostics and CPU regressions:** source/presented counters, timing logs, matching guide/output captures and regression coverage for cadence, history, settings, Reflex input-cycle gating and diagnostic parsers.

Generation uses completed source frames and skips intermediate presentation slots whose deadlines have expired. It seeds SDK history after scene changes or serial gaps. Depth, motion and camera data are captured with the matching display buffer; leased frame resources remain owned until their GPU work and presentation complete.

## Settings

| INI key | Environment variable | Values | Default |
| --- | --- | --- | --- |
| `frame_generation` | `BB_FRAME_GENERATION` | `off`, `2x`, `3x`, `4x` | `off` |
| `fg_backend` | `BB_FG_BACKEND` | `dlss`, `fsr` | `dlss` |
| `reflex` | `BB_REFLEX` | `off`, `on`, `boost` | `on` |

FSR supports Off/2x; selecting FSR clamps a higher multiplier to 2x. Unsupported Reflex remains inactive. Generation currently bypasses HDR output and scenes without valid depth/camera guides.

## Diagnostics

- `BB_FG_TIMING=1`: logs observed source and presentation rates, generated presentations and skipped slots. Presentation calls are not proof of displayed scanout cadence; compare with PresentMon when GPU testing is permitted.
- `BB_REFLEX_TIMING=1`: logs available driver latency reports. These are not input-to-photon measurements.
- `BB_FG_CAPTURE_DIR=<folder>`: captures one generated frame once its rendered serial reaches `BB_FG_CAPTURE_FRAME` (default 120). Files are written in `frame_<serial>` after the draw timeline completes. Readback can affect performance; keep it off during pacing measurements.

Captures contain `depth.bin` (R32 hardware depth in 0..1), `motion.bin` (RG16 current-to-previous motion in render pixels), `color_rgba8.raw`, `full_rgba8.raw`, `generated_N_rgba8.raw` and `guides_manifest.json`. The manifest identifies render/output sizes, frame IDs, jitter and camera parameters.

Analyze a capture with `python tools/windows/analyze_fg_guides.py <capture-folder>/frame_<serial>`. Other scripts analyze PresentMon CSVs, Reflex logs and process-memory CSVs; use their `--help` for accepted formats.

## Validation and package

Compilation, CPU regression tests, shader validation and isolated launcher-widget checks are performed without running the game or creating a GPU device. GPU frame pacing, image quality, effective multipliers, latency and long-run memory behavior still require runtime validation.

The package includes the paired ABI-2 `bbport_dlss.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, existing FSR assets, diagnostic tools and license notices. NVIDIA SDK terms, original MIT bridge notices and GPL-3.0 notices for adapted bbhost code are retained.
