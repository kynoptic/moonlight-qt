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
| `0x32`, one block per report, wait for each send | ~39 per second | 25.5 ms average per 142-byte send, 93 ms worst |
| `0x32`, one block per report, up to 4 in flight | ~33 per second | 446 and 463 of 1,310 finished in two runs |
| `0x33` (`pack2`), two blocks per report, wait for each send | ~44 per second | 34.2 ms average per 206-byte send |
| `0x33` (`pack2`), two blocks per report, up to 4 in flight | ~44 per second | 304 of 650 reports finished in 13.7 s |
| 78-byte no-op `0x31` report, wait for each send | — | 15.0 ms average per send |

Every row moved about 4.6-4.7 KB/s, so the link limits bytes, not reports. Packing two blocks into a 206-byte `0x33` report spends 103 bytes per block instead of 142, which is where the extra third comes from. Whether the controller plays `0x33` the way it plays the documented two-block `0x39` layout is not confirmed by feel.

Conditions for every row: MacBookAir10,1 (BCM_4378 Bluetooth), macOS 27.2.1, a DualSense on firmware 1.0.0 at RSSI -46, measured 2026-11-17. An Apple keyboard and trackpad were also connected over Bluetooth, and a Moonlight app was running but not streaming. Each run plays 14 s of patterns at the ideal rate: 1,310 one-block or 650 two-block reports.

macOS accepted every report it was handed. The limit is how fast its Bluetooth stack lets reports out, and send time grows with report size. P5M for Mac reports about 50-65 reports per second during a game, with roughly 15% of blocks dropped, and reports that an idle link is slower than an active one. These rows were taken with no game input, so they may understate what a streaming session gets.

Pacing uses the Mach timer. A run-loop timeout used as the wait gets coalesced by macOS for a background process and stalls about 100 ms at a time, which shows up as dropped reports that have nothing to do with Bluetooth.

In a hand test, the one-block `0x32` patterns at ~33 blocks per second did not feel broken. The `pack2` patterns have no feel result recorded.
