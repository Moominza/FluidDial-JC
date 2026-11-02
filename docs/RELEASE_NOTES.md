# FluidDial-JC 1.5.1 release notes

Target: Guition JC8048W550C_I (ESP32-S3, 800x480, GT911) with an MKS DLC32 MAX V1.2 running FluidNC v4.1.1, four axes (X/Y/Z mm, A degrees).

## Wi-Fi OTA (new in this release)

Password-protected, deliberately armed Wi-Fi firmware update from a Windows PC (stock Arduino OTA / `Update` / `esp_ota`), dual-slot with bootloader rollback, image-target check, controller-Idle interlock and a machine-command lockout on every transport while armed. Settings, calibration and Wi-Fi survive updates; no partition migration is needed. **Security limits:** password only - the transfer is not encrypted and the firmware is not signed. **Not yet tested on hardware:** a real wireless update, rollback and the negative/interrupted-transfer cases (see [OTA_UPDATES.md](OTA_UPDATES.md)). The stable tag stays unpublished until that acceptance test is done.

## Firmware images

| Artifact | Environment | Purpose |
|---|---|---|
| `fluiddial-jc-1.5.1-fullcontrol.bin` | `jc8048w550_fullcontrol_test` | **Operational JC firmware.** Red "FULL CONTROL" banner. Sends operator-confirmed commands. |
| `fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin` | `jc8048w550` | Ordinary diagnostic/preview firmware. **Motion-disabled**; blue PREVIEW banner; sends only `?`. |

Also shipped for the USB install/recovery path (not stand-alone images): `fluiddial-jc-1.5.1-usb-bootloader.bin`, `-usb-partitions.bin`, `-usb-boot_app0.bin`, with `OTA_INSTALL_GUIDE.md`. The `.bin` application images are written to 0x10000 only, never to 0x0.

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

## Checksums (source commit a53211e = tag v1.5.1, local; reproducible: two clean builds matched)

```
0431c7cd1a77c31ae8af005aadabc6c3a11526ce30f4a8565ea273f6df33815a  fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin
16fd0db3e9663b5a6983ed169dc9dbb9dc8510ebd5072f7fbe07819142d26bd8  fluiddial-jc-1.5.1-fullcontrol.bin
f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0  fluiddial-jc-1.5.1-usb-boot_app0.bin
2a71d69b471e20c2bac7fb469f3c6a807b3ebee780e348e5889db0da849ca363  fluiddial-jc-1.5.1-usb-bootloader.bin
1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c  fluiddial-jc-1.5.1-usb-partitions.bin
```

The full-control image above is what is installed on the pendant (USB, app only; esptool verify matched). The diagnostic image has not been flashed.
