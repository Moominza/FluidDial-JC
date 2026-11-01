# Live read-only controller check, 2026-11-01 (Instruction 014)

Source: HTTP command endpoint of the real FluidNC v4.1.1 controller, queried from the PC, **reads only** (`?`, `$I`, `$G`, `$#`, `$SD/Status`, `$/axes/...`, `$/probe/...`, `$/start/must_home`; the script refuses any text containing `=` or any other command). Nothing was written, homed, jogged, probed or positioned. Network identifiers (SSID, IP, MAC) from `$I` are intentionally not reproduced.

| Query | Reply |
|---|---|
| `$I` | `[VER:4.1 FluidNC v4.1.1 (esp32s3-wifi) :]`, `[OPT:MPHS]`, `[CLUSTER:16]`, `[MSG:Machine: CNC]` |
| `?` | `<Idle\|MPos:0.000,0.000,0.000,0.000\|FS:0,0>` (four axes, machine position; the pendant derives work values from the WCO and the offsets) |
| `$G` | `[GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]` (active WCS G54, tool 0) |
| `$#` | G54..G59, G59.1..3, G28, G30, G92 all `0.000` (4 values each); `[TLO:0.000,0.000,0.000,0.000]` |
| `$SD/Status` | `SD card detected` |
| probe / start | probe pin `gpio.48:low`, toolsetter `NO_PIN`, `must_home=false` |

Axes (`$/axes/<a>/...`), compared with the 2026-10-30 baseline ([2026-10-30-controller-read-capture.md](2026-10-30-controller-read-capture.md)):

| Axis | steps/mm | max travel | max rate | accel | homing cycle / positive dir | soft limits | hard limits | limit pins |
|---|---:|---:|---:|---:|---|---|---|---|
| X | 40 | 1000 | 6000 | 250 | 1 / false | false | false | neg `gpio.41:low` |
| Y | 40 | 2300 | 6000 | 250 | 1 / false | false | false | neg `gpio.40:low` |
| Z | 200 | 150 | 1500 | 100 | 1 / true | false | false | neg `gpio.39:low` |
| A | 1000 | 3000 | 1000 | 25 | refused (no homing setting) | false | false | none |

**Differences from the baseline: none** in every value both reads contain. The baseline recorded no limit-pin assignments; these are new facts (switch pins exist on X/Y/Z, `hard_limits` and `soft_limits` are false everywhere, A has no switch). The owner's conditions stand: soft limits OFF, hard limits OFF, machine position not established, no independent E-stop installed.
