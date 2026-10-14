# Windows GPU validation

Tests used an RTX 5080, driver 617.14, FIFO presentation and object motion enabled, with copied saves in offline play. The current desktop was 1920x1080 at approximately 120 Hz; reconstructed 4K and custom ultrawide targets were fitted to that desktop. These runs exercise the higher-resolution workload, but do not establish native 4K scanout performance. Camera movement was scripted; the reported intervals exclude loading and initialization. Final comparisons ran for 105 seconds and soaks for five minutes.

## Frame pacing

| Mode | Source cap | Observed display FPS | Display median / p99 |
| --- | ---: | ---: | --- |
| FSR 2x, previous pacing | 60 | 113.40 | 8.34 / 18.92 ms |
| FSR 2x, paced copy completion | 60 | 119.82 | 8.35 / 10.51 ms |
| FSR 2x with FSR 3 reconstruction, final build | 60 | 119.83 | 8.42 / 10.26 ms |
| FSR 2x, previous pacing | 30 | 60.00 | 16.56 / 28.73 ms |
| FSR 2x, paced copy completion, final build | 30 | 60.00 | 16.69 / 18.82 ms |
| DLSS 2x, Balanced reconstruction, fixed 60 FPS patch, final build | 60 | 120.00 | 8.34 / 17.37 ms |
| DLSS 3x | 40 | 119.88 | 8.34 / 8.39 ms |
| DLSS 4x, final build | 30 | 119.88 | 8.34 / 8.39 ms |

These are PresentMon display intervals, not rates inferred from a requested multiplier. Foreground ownership was effectively complete and the relevant traces used independent flip, with no reported ETW event loss. FSR now completes its composition copy before the presentation deadline and uses a separate graphics queue when available. The fallback on a shared queue also held 60 displayed FPS at a 30 FPS source cap, with a 19.46 ms p99 interval, though its CPU input-to-present interval was longer. DLSS retains its existing queue and deadline path.

Matching depth/motion captures were finite, correctly sized and within expected ranges. HUD extraction and display fitting were checked from generated-frame captures. FSR's generated and real fitted images were distinct and filled the presentation area. A DLSS 4x capture contained three distinct fitted intermediate images with HUD colors matching the real endpoint. Capture readback was disabled for ordinary pacing measurements.

Native reconstruction tests above 4K that produced no generated frames are excluded from frame-generation claims. A successfully created SDK feature alone is not proof that generation remains active.

## Reflex and latency

Reflex On/Boost initialized successfully. On/Off comparisons did **not demonstrate a meaningful latency reduction** in the tested workloads. At a 60 FPS source cap with DLSS 2x and native 4K reconstruction, the CPU-owned pad-sample-to-real-present median was 65.13 ms On versus 65.50 ms Off; this small difference does not establish a benefit. A Quality reconstruction comparison was similarly inconclusive (72.83 versus 72.95 ms). Other comparisons showed larger variation without a consistent improvement.

The CPU interval carries the guest simulation identity from the first pad sample through its accepted flip request to the real-frame present call. It excludes OS input delivery, driver queuing, scanout and panel response. Driver GPU timing windows on shared queues can be implausibly short and are treated conservatively. Neither measurement is physical input-to-photon latency; hardware measurement would be needed to establish that benefit.

## Stability and reproduction

Five-minute DLSS and FSR runs completed without an early exit or parser assertion. In the steady interval after the first two minutes, private memory and handle counts showed no sustained growth: DLSS private memory remained approximately 7170-7202 MiB and FSR approximately 6944-7035 MiB. This is a short bounded observation, not proof of stability over hours.

CPU validation passed 54 Python tests, nine native suites, two shader/SPIR-V checks and the ABI-2 DLSS adapter table. All five tracked FSR vendor patches were applied to a fresh checkout and produced byte-identical modified files. The package retains the paired adapter/runtimes and license notices; generation defaults to Off.

Core and synchronization validation exposed missing transfer usage on motion/output images and a reset-clear barrier with an insufficient access mask; these were corrected. Remaining validation reports occur with generation disabled as well, including shader interfaces, attachment declarations and legacy presentation/attachment synchronization. The renderer is **not validation-clean**. Validation-layer runs are excluded from frame-pacing numbers. The marker/acquire host-thread warning conflicts with the [specified concurrency exception for `vkSetLatencyMarkerNV`](https://docs.vulkan.org/refpages/latest/refpages/source/vkSetLatencyMarkerNV.html); marker lifetime is serialized with swapchain destruction, without blocking simulation markers behind acquire/present.

All gameplay processes were intentionally stopped at the end of each run. HDR generation, other GPUs/drivers, online co-op, extended combat and long-duration memory stability have not been established. Raw logs, saves and captures remain local.
