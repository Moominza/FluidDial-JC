# FluidDial-JC 1.5.1 release notes

Target: Guition JC8048W550C_I (ESP32-S3, 800x480, GT911) with an MKS DLC32 MAX V1.2 running FluidNC v4.1.1, four axes (X/Y/Z mm, A degrees).

## Firmware images

| Artifact | Environment | Purpose |
|---|---|---|
| `fluiddial-jc-1.5.1-fullcontrol.bin` | `jc8048w550_fullcontrol_test` | **Operational JC firmware.** Red "FULL CONTROL" banner. Sends operator-confirmed commands. |
| `fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin` | `jc8048w550` | Ordinary diagnostic/preview firmware. **Motion-disabled**; blue PREVIEW banner; sends only `?`. |

The live-jog test image (`jc8048w550_livejog_test`, X/Y 1 mm only), `m5dial` and `cyddial` build but are not release artifacts for the JC hardware. Checksums and build metadata are in `SHA256SUMS.txt` and `BUILD_INFO.txt` (release assets, and recorded at the end of this file). Each image's version string embeds the source commit it was built from. The packaging step did not flash either image.

## Implemented

Four-axis dashboard (machine/work, active WCS, controller state, freshness); jogging X/Y/Z (mm) and A (degrees) with separate STEP and FEED presets (linear 0.01/0.1/1/10/100 mm; A 0.1/1/5/15/45/90 degrees; feeds 300/1000/3000 mm/min and 200/500/1000 deg/min, validated against the controller's rates) and jog cancel; homing commands; WCS G54-G59 select, zero, set and readback; five work-positioning functions (GO TO WORK X0, Y0, Z0, X0 Y0, X0 Y0 Z0, with an operator-entered clearance and two confirmations for the combined move); guided probing, rotary centreline, tool reference and TLO; SD/job monitoring; FEED HOLD and a confirmed RESET; Wi-Fi TCP auto-connect; wired UART and BLE transports. Saved settings from earlier firmware are migrated, not reset.

## Command policy (enforced, tested)

The pendant never starts or resumes a program (no `$SD/Run`, `$Localfs/Run`, `~`), never runs user macros or restarts a job, never replays a command after a reconnect, never re-arms automatically and sends no motion command at boot, on any transport and in every build. The diagnostic image has no transmit function at all.

## Verification

Automated (tool-verified; totals in the final report): all host suites, command-interface and workflow suites, simulator integration, transport tests (BLE with a mock radio), static guard, firmware symbol check, UI renderer/geometry checks, and all five PlatformIO environments.

On-device (tool-verified from the boot log): FULL CONTROL banner, GT911 touch, saved calibration restored, automatic Wi-Fi, TCP, LIVE.

## Hardware acceptance

Everything in the owner's acceptance table (X, Y, Z, A, jog cancel, homing, probe, work-zero positioning, tool and rotary workflows, connection-loss recovery, touchscreen, UART and BLE, E-stop) is recorded as **"PASS - operator-reported, observations not supplied."** It is the owner's acceptance, not instrumented validation. See [COMMISSIONING.md](COMMISSIONING.md).

## Known limitations

- Controller: Z `positive_direction=true` with only `limit_neg_pin gpio.39` (unreconciled; configuration unchanged on 2026-11-02); soft and hard limits OFF; no tool setter; A has no homing. The pendant never changes controller configuration.
- No measurements, panel photos, controller logs or UART/BLE module identities were supplied; none are claimed.
- UART and BLE hardware results are operator-reported only; repository tests use a mock radio and a simulator.
- FEED HOLD, CANCEL JOG and RESET are not an emergency stop.

## Checksums (source commit 3c76461, tag v1.5.1, reproducible: two clean builds matched)

```
c8789e72f5f8d14cbfc52f3fc9e495c832f85a9b9e4d114b884e05e02746f99d  fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin
db29f6bac963211442ed10fa0ca2b5440847614dc41d3996e8c717393c88c23c  fluiddial-jc-1.5.1-fullcontrol.bin
```
