# Windows GPU validation

Gameplay tests used an RTX 5080, driver 617.14, a custom ultrawide output, DLSS Quality reconstruction, object motion enabled and FIFO presentation. Tests used copied saves and offline play, including scripted camera movement. Each final comparison ran for 105 seconds; the reported interval excludes loading and initialization.

| Mode | Source cap | Observed display FPS | Display median / p99 |
| --- | ---: | ---: | --- |
| FG off | 60 | 59.91 | 16.65 / 31.49 ms |
| DLSS 2x | 30 | 60.00 | 16.74 / 20.98 ms |
| DLSS 2x | 60 | 115.88 | 8.34 / 22.53 ms |
| DLSS 3x | 40 | 119.87 | 8.34 / 8.38 ms |
| DLSS 4x | 30 | 119.78 | 8.34 / 8.38 ms |
| FSR 2x | 30 | 60.00 | 15.95 / 31.46 ms |

These are PresentMon display intervals, not rates inferred from the requested multiplier. The final matrix used hardware-composed independent flip with all or almost all foreground samples belonging to the game. FSR 2x remains less evenly paced than DLSS in this scene. A 3x run at 30 FPS also generated approximately 90 presentations per second, but its unfocused/composed trace is unsuitable for a smoothness claim.

The tests exposed and corrected three integration failures:

- Source cap waits could expire every interpolated slot. Pacing now starts at the permitted source flip and keeps a fixed preparation allowance, avoiding a feedback loop after a stall.
- Deferred PM4 parsing could observe recycled guest command memory. FG enables owned command-buffer snapshots by default.
- A display-space/scene-space comparison classified almost the entire picture as UI. Matching snapshots before and after UI now isolate the HUD, preserve display color adjustments and retain generated scene pixels. Generated post-processing views also use the ordinary display's channel order.

Matching depth/motion captures were finite, correctly sized and in their expected ranges. The corrected HUD extraction covered about 1.4% of a gameplay frame instead of approximately 99.98%. A final 4x capture contained three distinct fitted intermediate images; their difference from the real endpoint decreased with temporal position, and their HUD colors matched the real image. A final color verification measured approximately 119.79 displayed FPS, with an 8.34 ms median and 8.55 ms p99.

No parser assertions or early exits occurred in the final matrix. Test processes were intentionally stopped at the end of each run. Reflex On/Boost initialized successfully; driver timing attribution is not an input-to-photon measurement and remains conservative. HDR generation, other GPUs/drivers, online co-op, long combat sessions and long-duration memory stability have not been established by these runs.

The test harnesses, raw captures and full logs remain local. The packaged build keeps FG off by default.
