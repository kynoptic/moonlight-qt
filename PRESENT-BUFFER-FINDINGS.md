# Client present-buffer experiment (macOS / Apple Silicon)

Branch `experiment/pacer-csv`. Measures whether a client-side present buffer reduces present-cadence breaks when streaming a 60 fps source to a 60 Hz Mac, from logged present timestamps.

Companion analyzer and cross-host attribution: [play-telemetry](https://gitea.kynoptic.synology.me/kynoptic/play-telemetry) (`client_present` layer).

## What's in the branch

| Knob (env var) | File | Description |
| --- | --- | --- |
| `ML_PRESENT_CSV` | `vt_metal.mm` | Logs on-glass present cadence at the CAMetalDisplayLink present point. |
| `ML_PRESENT_BUFFER` | `vt_metal.mm` | FIFO present cushion, in whole frames. |

`ML_PRESENT_BUFFER` is also exposed in the UI as the macOS-only **Smooth frame delivery** setting, which enables a 2-frame cushion. The environment variable overrides the setting for other depths.

Earlier Pacer-side instrumentation — `ML_PACER_CSV` (decode→renderer handoff CSV) and `ML_MIN_LATENCY` (a frame-floor in `handleVsync`) — has been removed; both were inert on macOS (see below).

## Pacer behavior on macOS

The Pacer has no macOS `IVsyncSource` (only `DxVsyncSource` on Windows and `WaylandVsyncSource` on Wayland). On macOS, `submitFrame` takes the no-vsync branch, the vsync-gated pacing queue is not used, and `handleVsync` does not run. The Metal renderer presents through CAMetalDisplayLink, sampling the latest decoded frame at each display refresh and dropping the rest.

Consequences for the earlier instrumentation:

- `ML_MIN_LATENCY`, placed in `handleVsync`, never ran on macOS; an A/B against it showed no effect.
- `ML_PACER_CSV` logged decode→renderer handoff cadence, not frames on glass.

A Pacer-side floor can still apply on macOS when implemented off the vsync path — for example, on a dedicated thread that handles the no-vsync branch (as in upstream PR #1139) rather than in `handleVsync`.

`ML_PRESENT_BUFFER` replaces the single latest-wins frame in the renderer with a FIFO cushion of N frames and presents the oldest, so a late arrival is covered from buffered frames rather than repeating.

## A/B results

Workload: Unigine Heaven on the host (120 Hz virtual display), 1080p60 stream, `awdl-toggle` active, interleaved cushion depths, on-glass measurement via `ML_PRESENT_CSV`.

Two cadence-break types, both counted:

- **repeat**: a refresh with no fresh frame (present interval > 1.5× baseline); the previous frame is shown twice.
- **skip**: a decoded frame dropped at handoff because the source ran ahead.

240 s runs, ~13.6k present events each, post-reboot:

| Cushion | Added latency | Repeats/min | Skips/min | Total breaks/min | Effective fps |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 ms | 25.6 | 12.5 | 38.1 | 59.51 |
| 2 | 33 ms | 18.2 | 3.5 | 21.7 | 59.66 |
| 3 | 50 ms | 19.8 | 3.7 | 23.5 | 59.63 |

Multi-frame repeats (2+ consecutive missed refreshes) went from 38 to 19 between the 0 and 2-frame runs.

Earlier 90 s matrix covering the 1-frame depth (different absolute level — pre-reboot, shorter runs):

| Cushion | Added latency | Repeats/min |
| --- | --- | --- |
| 0 | 0 ms | 27.7 |
| 1 | 16.7 ms | 23.7 |
| 2 | 33.3 ms | 15.6 |

## Conclusions

- 2 frames (33 ms) gave the largest reduction: ~43% fewer total breaks than the 0-frame control, from a ~70% cut in skips and ~29% fewer repeats, with multi-frame stalls roughly halved. Consistent across three matrices.
- 1 frame (16.7 ms) helped less — intermediate in the one matrix that covered it, and noisier.
- 3 frames (50 ms) showed no improvement over 2 within noise.
- The residual ~18 repeats/min correspond to host↔client clock drift (cf. [Apollo#372](https://github.com/ClassicOldSong/Apollo/issues/372), [Sunshine#2286](https://github.com/LizardByte/Sunshine/issues/2286)) plus sub-half-frame slips absorbed by the display link's half-frame wait.

The effect is a ~40% reduction in residual cadence breaks (mostly skips) at 33 ms added latency, on a stream that was already ~98% on-cadence (effective fps 59.51 → 59.66). Matching stream rate to display refresh, and a 120 Hz/VRR client display (untested here; `preferredFrameRateRange` on the display link is wired for VRR), address jitter and drift without added latency.

## Reproduce

```sh
make release   # Homebrew Qt on PATH
ML_PRESENT_CSV=/tmp/p.csv ML_PRESENT_BUFFER=2 \
  ./app/Moonlight.app/Contents/MacOS/Moonlight stream <host> "Desktop" --1080 --fps 60 --vsync
# cushion logs as: "Present jitter buffer: 2 frame cushion (33.3 ms ...)"
# analyze: python -m stutter_analyzer <session-dir>  (CSV as moonlight-present.csv)
```

Caveats:

- Stream FPS must be ≤ display Hz, or pacing/V-sync is disabled.
- Long headless stream+kill batches can degrade the macOS display-link state until reboot.
- Wrap streams in `gtimeout --signal=KILL` to bound a wedged exit.
