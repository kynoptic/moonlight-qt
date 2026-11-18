# DualSense Bluetooth haptics probe

Tests whether macOS can stream waveform ("HD") haptics to a DualSense over Bluetooth fast enough for a Moonlight port of Vibeshine's waveform haptics. The controller needs one 142-byte output report `0x32` every 10.67 ms, which is about 94 reports per second.

Requires the Xcode command line tools and a DualSense paired over Bluetooth. From this folder:

```bash
clang -O2 -Wall -o ds5_bt_haptics_probe ds5_bt_haptics_probe.c -framework IOKit -framework CoreFoundation
```

```bash
./ds5_bt_haptics_probe
```

The probe plays five two-second patterns (both sides, left only, right only, a sweep, and taps), then prints how many reports macOS delivered and how long each send took.

## Measured: macOS delivered well under half the reports needed

| Send method | Delivered | Notes |
| --- | --- | --- |
| Wait for each send | ~39 per second | 25.5 ms average per send, 93 ms worst |
| Send without waiting, up to 4 in flight | ~32 per second | 446 of 1,310 finished, 864 dropped, 13.9 s wall time |
| 78-byte no-op `0x31` report, waiting | ~67 per second | 15.0 ms average per send |

Conditions for every row: MacBookAir10,1 (BCM_4378 Bluetooth), macOS 27.2.1, a DualSense on firmware 1.0.0 at RSSI -46, measured 2026-11-17. An Apple keyboard and trackpad were also connected over Bluetooth, and a Moonlight app was running but not streaming. Each run sends 1,310 reports, 14 s at the ideal rate.

macOS accepted every report it was handed. The limit is how fast its Bluetooth stack lets reports out, and send time grows with report size. P5M for Mac reports about 50-65 reports per second during a game, with roughly 15% of blocks dropped, and reports that an idle link is slower than an active one. These rows were taken with no game input, so they may understate what a streaming session gets.

Pacing uses the Mach timer. A run-loop timeout used as the wait gets coalesced by macOS for a background process and stalls about 100 ms at a time, which shows up as dropped reports that have nothing to do with Bluetooth.

How the patterns feel on the controller is not recorded here.
