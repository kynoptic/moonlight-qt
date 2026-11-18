# DualSense Bluetooth haptics probe

Tests whether macOS can stream waveform ("HD") haptics to a DualSense over Bluetooth fast enough for a Moonlight port of Vibeshine's waveform haptics. The controller needs one 142-byte output report `0x32` every 10.67 ms, which is about 94 reports per second.

Requires the Xcode command line tools and a DualSense paired over Bluetooth. From this folder:

```bash
clang -O2 -Wall -o ds5_bt_haptics_probe ds5_bt_haptics_probe.c -framework IOKit -framework CoreFoundation
```

```bash
./ds5_bt_haptics_probe
```

To try two blocks per report instead, pass all four options:

```bash
./ds5_bt_haptics_probe 0.8 async 4 pack2
```

The probe plays five two-second patterns (both sides, left only, right only, a sweep, and taps), then prints how many reports macOS delivered and how long each send took.

## Measured: macOS delivered well under half the reports needed

Each block is 10.67 ms of vibration, so smooth playback needs 94 blocks per second.

| Send method | Blocks delivered | Notes |
| --- | --- | --- |
| `0x32`, one block per report, wait for each send | ≤39 per second | 25.5 ms average per 142-byte send, 93 ms worst; derived from call time, since this run recorded no wall time |
| `0x32`, one block per report, up to 4 in flight | ~33 per second | 446 and 463 of 1,310 finished in two runs |
| `0x33` (`pack2`), two blocks per report, wait for each send | ~44 per second | 650 reports in 29.6 s; 34.2 ms average per 206-byte send |
| `0x33` (`pack2`), two blocks per report, up to 4 in flight | ~44 per second | 304 of 650 reports finished in 13.7 s |
| 78-byte no-op `0x31` report, wait for each send | — | 15.0 ms average per send |

Every row with a recorded wall time moved about 4.5-4.7 KB/s, and send time grows with report size, so the link limits bytes, not reports. Four reports in flight delivered no more than waiting for each send. Packing two blocks into a 206-byte `0x33` report spends 103 bytes per block instead of 142, which is where the extra third comes from.

Conditions for every row: MacBookAir10,1 (BCM_4378 Bluetooth), macOS 27.2.1, a DualSense on firmware 1.0.0 at RSSI -46, measured 2026-11-17. An Apple keyboard and trackpad were also connected over Bluetooth, and a Moonlight app was running but not streaming. Each run plays 14 s of patterns at the ideal rate: 1,310 one-block or 650 two-block reports.

macOS accepted every report it was handed. The limit is how fast its Bluetooth stack lets reports out, and send time grows with report size. P5M for Mac reports about 50-65 reports per second during a game, with roughly 15% of blocks dropped, and reports that an idle link is slower than an active one. These rows were taken with no game input, so they may understate what a streaming session gets.

Pacing uses the Mach timer. A run-loop timeout used as the wait gets coalesced by macOS for a background process and stalls about 100 ms at a time, which shows up as dropped reports that have nothing to do with Bluetooth.

Thread priority is not the limit. At user-interactive priority, the `pack1` and `pack2` runs started each send an average of 1.8 and 3.3 ms after its deadline (worst 3.0 and 14.9 ms), and every send call returned in under 1.5 ms. Blocks were dropped only because four reports were still waiting on Bluetooth.

In a hand test, the one-block `0x32` patterns at ~33 blocks per second did not feel broken. Run back to back with the user told which was which, the `pack2` patterns at ~44 blocks per second felt a little better, so the controller plays the two-block `0x33` layout.

## The app's player matches the probe

`haptics_harness.cpp` drives Moonlight's own player (`app/streaming/input/dualsensehaptics_mac.cpp`) with Vibeshine-shaped packets: 240 frames at 48 kHz every 5 ms, through the real gate and a second IOKit handle beside SDL's. The build and run commands are in its header.

| Result | Value |
| --- | --- |
| Device lookup | Matched by SDL's `DevSrvsID:` path, which equals the IOKit registry entry ID |
| Delivery while a pattern plays | 42-48 blocks per second, from the per-second log lines with no silence in them |
| Failed sends | 0 |
| Late, stale, overflowed, or trimmed audio | 0 in every line |
| Reports skipped at the 2-in-flight cap | 22-26 per second while a pattern plays |
| Second block of `0x33` | Plays: the "second block only" pattern buzzed, with no timing shift during it |

Conditions: same Mac, controller, and macOS as above, measured 2026-11-18 with Moonlight closed, `ML_DS5_HAPTICS_PACK=2` and 2 in flight (the defaults), strength 0.8. The user felt all six patterns.

## A real stream delivers close to the full rate

During a game streamed from Vibeshine, the same player delivered nearly every block, roughly double the idle measurements above. P5M for Mac's note that an active link outpaces an idle one held here.

| Result, per second while the waveform played | Value |
| --- | --- |
| Blocks delivered | 84-94 (94 is the full rate); typically 86-90 |
| Host packets in | 198-204 (the host sends one every 5 ms, continuously while a game drives the actuators) |
| Reports skipped at the 2-in-flight cap | 0-5 |
| Failed sends | 0 |
| Late or stale packets | 0 |
| Frames trimmed to keep the newest | 0-110 of about 3,000 |
| Packets cleared by queue overflow | 8 or 16 in 3 of the 38 lines, after network bursts |

The user felt HD vibration in the game. The log recorded no ordinary rumble commands that session.

Conditions: same Mac, controller, and macOS as above, measured 2026-11-18. Moonlight test build from `feat-ds5-haptics`, 60 FPS stream, defaults (`ML_DS5_HAPTICS_PACK=2`, 2 in flight). Vibeshine 2.0.6 or newer, with its DualSense USB audio and haptics component installed and the gamepad setting on automatic. Per-second lines from `/tmp/Moonlight-*.log`: 38 in all, 33 of them full seconds of play at 84-94 blocks; the other 5 cover a waveform starting or stopping.
