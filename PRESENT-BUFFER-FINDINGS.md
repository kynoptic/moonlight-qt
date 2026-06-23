# Client present-buffer experiment (macOS / Apple Silicon)

Branch `experiment/pacer-csv`. Evaluates whether a client-side jitter buffer
reduces present-cadence judder when streaming a paced 60 fps source to a 60 Hz
Mac, measured honestly with a machine-readable ruler rather than "feels smoother."

Companion analyzer + cross-host attribution live in
[play-telemetry](https://gitea.kynoptic.synology.me/kynoptic/play-telemetry)
(`client_present` layer).

## What's in the branch

| Knob (env var) | File | Status |
| --- | --- | --- |
| `ML_PRESENT_CSV` | `vt_metal.mm` | On-glass present cadence at the CAMetalDisplayLink present point. |
| `ML_PRESENT_BUFFER` | `vt_metal.mm` | FIFO present cushion (whole frames). The lever that actually works here. |

The Pacer-side instrumentation that drove the early A/B — `ML_PACER_CSV` (decode→renderer handoff CSV) and `ML_MIN_LATENCY` (a frame-floor buffer in `handleVsync`) — has been removed. Both were inert on macOS because the Pacer is bypassed entirely; see the load-bearing finding below.

`ML_PRESENT_BUFFER` is also exposed in the UI as the macOS-only **Smooth frame delivery** setting — a 2-frame cushion (the verdict's sweet spot) when enabled. The environment variable overrides the setting for tuning other depths.

## The load-bearing finding: the Pacer is bypassed on macOS

The Pacer has no macOS `IVsyncSource` (only Windows `DxVsyncSource` and Wayland).
So `submitFrame` takes the no-vsync branch, the pacing queue is never used, and
`handleVsync` — where `ML_MIN_LATENCY` lives — never runs. On Apple Silicon the
Metal renderer presents through **CAMetalDisplayLink**, sampling the latest
decoded frame at the display refresh and dropping the rest (latest-wins, zero
cushion — the macOS analog of the Pacer's latency-minimizing drop).

Consequences:

- `ML_MIN_LATENCY` does nothing on this platform. A first A/B against it was void
  (all floors ran identical code); the apparent trend was noise.
- `ML_PACER_CSV` logs the **handoff** cadence (arrival jitter), not frames on
  glass. The real judder is quantized to the 60 Hz refresh — a late frame yields
  a repeated frame (a 33.3 ms interval = exactly 2× the 16.67 ms period).
- The correct place for a client buffer is the renderer's frame handoff, not the
  Pacer. `ML_PRESENT_BUFFER` replaces the single latest-wins frame with a FIFO
  cushion of N frames and presents the oldest, so a late arrival is covered from
  spares instead of repeating.

## A/B results

Workload: Unigine Heaven on the host (120 Hz virtual display), 1080p60 stream,
`awdl-toggle` active, interleaved floors, on-glass ruler (`ML_PRESENT_CSV`).

Two flavors of visible cadence break, both counted:

- **repeat** (a pause): a refresh with no fresh frame — present interval > 1.5×
  baseline; the previous frame is shown twice, motion stalls.
- **skip** (a drop): a distinct decoded frame discarded at handoff because the
  source ran ahead; motion jumps a frame.

The buffer attacks both at once — it holds the ahead-bursts that would be
discarded as skips and spends them filling the gaps that would be repeats.

Definitive matrix — 240 s runs, ~13.6k present events each, post-reboot rig:

| Floor | Added latency | Repeats/min | Skips/min | **Total breaks/min** | Effective fps |
| --- | --- | --- | --- | --- | --- |
| 0 (control) | 0 ms | 25.6 | 12.5 | **38.1** | 59.51 |
| **2** | **33 ms** | **18.2** | **3.5** | **21.7** | **59.66** |
| 3 | 50 ms | 19.8 | 3.7 | 23.5 | 59.63 |

Multi-frame repeats (2+ consecutive missed refreshes — the ugliest stalls) also
roughly halved, 38 → 19 across the control vs 2-frame runs.

Earlier 90 s matrix that also covered the 1-frame floor (different absolute
level — pre-reboot, shorter runs — but the ordering holds):

| Floor | Added latency | Repeats/min |
| --- | --- | --- |
| 0 | 0 ms | 27.7 |
| 1 | 16.7 ms | 23.7 |
| 2 | 33.3 ms | 15.6 |

## Verdict

- **2 frames (33 ms) is the sweet spot.** ~43% fewer total cadence breaks
  (38 → 22 per minute) vs the latest-wins control — driven by a ~70% cut in
  frame-skips plus ~29% fewer repeats, and the multi-frame stalls roughly halved.
  Confirmed across three independent clean matrices.
- **1 frame (16.7 ms)** is the lower-latency partial option — it helps, but less
  (intermediate in the only matrix that covered it, and noisier).
- **3 frames (50 ms) plateaus** — no better than 2, within noise. The buffer has
  absorbed the absorbable jitter by 2 frames.
- The residual ~18 repeats/min is the floor a client buffer can't cross: the
  host↔client clock-drift beat (cf.
  [Apollo#372](https://github.com/ClassicOldSong/Apollo/issues/372),
  [Sunshine#2286](https://github.com/LizardByte/Sunshine/issues/2286)) plus the
  sub-half-frame slips the display link's own half-frame wait already absorbs.

Net: a real, repeatable smoothing — it removes ~40% of the residual cadence
breaks (mostly frame-skips) at 33 ms latency. Honest perspective: this is on an
already-mostly-clean stream (eff fps 59.5 → 59.7; ~98% of frames were already
on-cadence), so it polishes the residual rather than rescuing a janky stream. The
dominant levers remain matching stream rate to display refresh and — untested
here — a 120 Hz/VRR client display, which would address both jitter and drift at
no latency cost (`preferredFrameRateRange` on the display link is already wired
for VRR).

## Reproduce

```sh
make release   # Homebrew Qt on PATH
ML_PRESENT_CSV=/tmp/p.csv ML_PRESENT_BUFFER=2 \
  ./app/Moonlight.app/Contents/MacOS/Moonlight stream <host> "Desktop" --1080 --fps 60 --vsync
# floor logs as: "Present jitter buffer: 2 frame cushion (33.3 ms ...)"
# analyze: python -m stutter_analyzer <session-dir>  (place CSV as moonlight-present.csv)
```

Caveats that cost real time here: stream FPS must be ≤ display Hz or pacing/V-sync
disable; run on a rested rig (long headless stream+kill batches degrade the macOS
display-link state to ~3 fps until reboot); wrap streams in `gtimeout --signal=KILL`
so a wedged exit can't hang the harness.
