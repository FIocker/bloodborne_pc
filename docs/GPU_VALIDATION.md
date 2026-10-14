# Windows GPU validation

Tests used an RTX 5080, driver 617.14, FIFO presentation and object motion enabled, with copied saves in offline play. The current desktop is a native ultrawide at approximately 120 Hz. Earlier runs used a 1920x1080 desktop and fitted reconstructed 4K and custom ultrawide targets to it; those results are kept below as historical. Camera movement was scripted and the reported intervals exclude loading and initialization. Matched comparisons ran 150 seconds, the scripted gameplay run 900 seconds, and the soaks one hour.

## Frame pacing

Matched 150 second native-ultrawide runs, PresentMon display intervals, independent flip:

| Mode | Source cap | Observed display FPS | Display median / p99 |
| --- | ---: | ---: | --- |
| DLSS 2x, Reflex On | 60 | 119.97 | 8.34 / 8.89 ms |
| DLSS 2x, Reflex On, repeat | 60 | 119.97 | 8.34 / 9.19 ms |
| DLSS 2x, Reflex Off | 60 | 120.02 | 8.34 / 13.26 ms |
| DLSS 2x, driver-only pacing | 60 | 119.88 | 8.34 / 8.37 ms |
| DLSS 2x, Reflex Boost | 60 | 119.97 | 8.34 / 8.66 ms |
| FSR 2x | 60 | 119.88 | 8.34 / 8.37 ms |
| DLSS 3x | 40 | 120.03 | 8.34 / 10.69 ms |
| DLSS 4x | 30 | 119.99 | 8.34 / 15.08 ms |
| DLSS 2x | 30 | 60.01 | 16.34 / 23.80 ms |
| FSR 2x | 30 | 60.00 | 16.60 / 22.28 ms |

These are PresentMon display intervals, not rates inferred from a requested multiplier. Each row owned the foreground for essentially the whole run (149-150 of 150 samples) and presented through independent flip, with no reported ETW event loss. FSR completes its composition copy before the presentation deadline and uses a separate graphics queue when available; DLSS keeps its existing queue and deadline path.

The 30 and 40 source-cap runs contain many repeated input-cycle identifiers in the CPU interval stream, so only their display pacing is reported. Repetition also occurs less frequently at 60 FPS. Multiple flips can share a pad sample; these records must not be treated as independent input events.

Matching depth/motion captures were finite, correctly sized and within expected ranges. HUD extraction and display fitting were checked from generated-frame captures. FSR's generated and real fitted images were distinct and filled the presentation area. A DLSS 4x capture contained three distinct fitted intermediate images with HUD colors matching the real endpoint. Capture readback was disabled for ordinary pacing measurements.

Native reconstruction tests above 4K that produced no generated frames are excluded from frame-generation claims. A successfully created SDK feature alone is not proof that generation remains active.

### Historical fitted-desktop session

These earlier runs fitted reconstructed 4K and custom ultrawide targets to a 1920x1080 desktop and are retained for comparison; they do not establish native high-resolution scanout. Foreground ownership was effectively complete and the relevant traces used independent flip, with no reported ETW event loss.

| Mode | Source cap | Observed display FPS | Display median / p99 |
| --- | ---: | ---: | --- |
| FSR 2x, previous pacing | 60 | 113.40 | 8.34 / 18.92 ms |
| FSR 2x, paced copy completion | 60 | 119.82 | 8.35 / 10.51 ms |
| FSR 2x with FSR 3 reconstruction | 60 | 119.83 | 8.42 / 10.26 ms |
| DLSS 2x, Balanced, fixed-60 patch | 60 | 120.00 | 8.34 / 17.37 ms |
| DLSS 3x | 40 | 119.88 | 8.34 / 8.39 ms |
| DLSS 4x | 30 | 119.88 | 8.34 / 8.39 ms |
| FSR 2x, paced copy | 30 | 60.00 | 16.69 / 18.82 ms |

## Reflex and latency

At a 60 source cap with DLSS 2x on native ultrawide, the CPU-owned interval from the first pad sample of a guest cycle to the real-frame present call was about 32.89 ms with Reflex On, about 32.89 ms on a repeat run, about 47.98 ms with Reflex Off, and about 40.00 ms with the application queue gate disabled (driver-only pacing). Display pacing held at about 120 FPS through independent flip, with a display p99 near 9 ms on the On runs.

The CPU interval carries the guest simulation identity from the first pad sample through its accepted flip request to the real-frame present call. It excludes OS input delivery, driver queuing, scanout and panel response, so it is a diagnostic interval and not physical input-to-photon latency; hardware capture would be needed to establish that. Driver GPU timing windows on shared queues can be implausibly short and are treated conservatively.

Reflex On/Off comparisons in the earlier fitted-desktop session were inconclusive (about 65 ms both ways) and are superseded by the native-ultrawide numbers above.

## Stability and reproduction

One hour of DLSS 2x generation on the intermediate safe-drain build kept private memory in a flat ~7100 MiB band over the last 30 minutes (net +1 MiB), with handle counts drifting down and dedicated VRAM constant; display p99 after the first two minutes was about 15 ms, with a single ~120 ms display gap and a minimize/restore cluster. That run used an intermediate build identity, so it does not validate the final candidate's DLSS path.

One hour of FSR 2x generation on the input-cycle queue candidate (probe D1EF) kept private memory flat (~6630 MiB, net -19 MiB over the last 30 minutes), dedicated VRAM constant at 4054 MiB, and handle counts trending down (-84); display p99 after the first two minutes was 8.37 ms and the Reflex backlog gate recorded no timeouts. The game owned the foreground for 3592 of 3593 samples. The later queue correction counts each accepted flip separately and requires its own regression checks.

A 300 second DLSS run on that candidate held the game window minimized for ten seconds and restored it; the game re-acquired the foreground and generation resumed (about 120 FPS presented, 290 of 300 samples owned). A 900 second FSR gameplay run exercised inventory, a Hunter's Mark reload and one enemy encounter ending in player death and respawn; generation stayed active in the open scene and temporarily bypassed during loads before returning. These are limited encounter, death and reload checks, not extended combat.

CPU validation passed 54 Python tests, nine native suites, two shader/SPIR-V checks and the ABI-2 DLSS adapter table. All five tracked FSR vendor patches were applied to a fresh checkout and produced byte-identical modified files. The package retains the paired adapter/runtimes and license notices; generation defaults to Off.

Core and synchronization validation exposed missing transfer usage on motion/output images and a reset-clear barrier with an insufficient access mask; these were corrected. The targeted synchronization fixes for the `08912` VUID and the RedirectColor/LOADRAW path are gone in matched 150 second On/Off runs. Remaining reports occur with generation disabled as well: legacy shader-interface warnings (`Location-06272`, `OpEntryPoint-08743`), threading warnings, and a write-after-present hazard. The renderer is **not validation-clean**. Validation-layer runs are excluded from frame-pacing numbers. The marker/acquire host-thread warning conflicts with the [specified concurrency exception for `vkSetLatencyMarkerNV`](https://docs.vulkan.org/refpages/latest/refpages/source/vkSetLatencyMarkerNV.html); marker lifetime is serialized with swapchain destruction, without blocking simulation markers behind acquire/present.

All gameplay processes were intentionally stopped at the end of each run. HDR generation, other GPUs/drivers, online co-op, extended combat, repeated deaths and travel between regions, variable GPU load, and multi-hour stability have not been established. Raw logs, saves and captures remain local.
