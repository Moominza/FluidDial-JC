# FluidDial-JC 1.5.2 release notes (release candidate)

Production polish on top of the published v1.5.1 (immutable, commit 82ffd31). Same hardware, same FluidNC v4.1.1 controller, same OTA protocol; the pendant is still only the HMI and FluidNC plans and executes every move.

## What changed (full-control firmware only)

- **Jogging no longer needs arming.** ARM JOGGING, the arming dialog, the 120 s timeout and the re-arm after leaving the page or reconnecting are gone, and so are per-jog confirmations. One deliberate tap on JOG - / JOG + submits one incremental jog; a held touch does not repeat; no second jog is accepted until the first is resolved; the next one is possible as soon as FluidNC reports it complete. This is an **intentional production interaction change**, implemented as a new gate policy (`JcGatePolicy::Production`) in the shared dispatcher, not as a bypass. Kept: LIVE transport, fresh status and fresh command (no stale replay), no Alarm, Idle/Jog controller state, single-operation tracking, the OTA transfer lock, and FluidNC's authority to accept or reject. JOG CANCEL and FEED HOLD stay available without arming. Every other consequential class (homing, probing, offsets, tool length, reset/unlock, positioning routines) keeps its arming and confirmation, and program start/resume, user macros and command replay stay prohibited.
- **Jog feeds**: four presets per axis group: X/Y 300 / 1000 / 3000 / 6000 mm/min, Z 100 / 300 / 750 / 1500 mm/min, A 100 / 250 / 500 / 1000 deg/min (the A unit is degrees on this machine, as commissioned). A preset above the axis limit is disabled and the selection moves down to the largest allowed one when the controller reports a lower maximum; the limit shown on the page says whether it comes from the controller. Saved settings are migrated from format 1 and 2 (format 3 now): customised values are kept and never raised. Step presets are unchanged; switching between a linear axis and A selects 1 mm / 1 degree.
- **Jog page**: ARM and INCREMENTAL removed; the feed row uses the whole width; panel rows SELECTED / RESULT / MACHINE / feed limit; states Ready, Jog requested, Motion active, Completed, controller error, Disconnected; no popup on a successful jog; errors and blocked jogs are explained.
- **Production look**: no FULL CONTROL / TEST BUILD banners, no red commissioning footer on the menu, settings or connection pages; the MENU footer is a compact connection + controller-state line; firmware version and mode are under SETTINGS > DIAGNOSTICS. Red is kept for alarms, stop controls and genuine errors. The ordinary diagnostic image still shows PREVIEW - MOTION DISABLED. `tools/check_production_wording.py` guards the screens of the production build.
- **OTA**: unchanged protocol, interlocks and rollback. New: the backlight is switched off while the update image is written and comes back when the transfer ends, because flash writes disturb the RGB panel (the screen flashed during v1.5.1 and during the first v1.5.2 test upload). The updater program is the same engine (version 1.5.2) and installs this image.

## Verification (host, 2026-11-03)

Host suites 0 failed (command interface 928 checks incl. the production gate/flow tests: unarmed jog allowed, one tap = one line, unresolved/duplicate refused, stale/alarm/Hold/Run refused, OTA lock, no replay after link loss, other classes still need arming, program start blocked); settings migration v1/v2/v3; UI renderer ordinary 48, preview 96, live-jog 72, production 95 frames, 0 layout problems; simulator integration PASS; `check_no_sink.py`, `check_firmware_symbols.py`, `check_production_wording.py` PASS; all five PlatformIO environments build; updater protocol tests 25/25 against the new package. The renderer shows page code, not the panel.

## On the pendant

### RC2 (display stability, Instruction 024)

- Panel configuration found (unchanged): ST7262 RGB, 800x480, one 16-bit frame buffer in PSRAM (Arduino_GFX `fb_in_psram`, no bounce buffer, ESP-IDF 4.4), pixel clock 16 MHz, porches 8/4/8, cache write-back after each draw. Timing was not changed: there is no evidence that it is wrong.
- The only flicker actually reported is the full-screen flashing during an OTA upload (flash writes stop the cache while the RGB DMA reads PSRAM). Ordinary runtime flicker has not been reported and has not been measured. The screens redraw only changed regions in normal operation; full-screen clears happen on page changes.
- Change: the OTA backlight-off workaround is now released on every loop pass (not only from the OTA draw callback), so a transfer that ends by timeout, abort or failure cannot leave the panel dark.
- Change: a `health:` line every 30 s on the serial port (worst poll time, heap, PSRAM free/minimum/largest block).
- Reconnect delay after a reboot: OTA is offered only when the controller status is fresh and Idle, so it waits for the FluidNC link. The pendant's own retry back-off is capped at 30 s; delays of minutes are therefore most likely a stale session held on the FluidNC side. This is a hypothesis, not demonstrated, and nothing was changed for it.
- RC2 images: fullcontrol `b28c217ae7e113e17c032a37f519d8449e584bb98894d55c714036b260e21d41`, diagnostic motion-disabled `ea3e9e632df0e022f2a35844d8d4c867f3f8f42d9d1b5c2064b431a31ad8424b`, firmware source 77cdbfe. Physical display behaviour: not verified. See docs/PHYSICAL_TEST_CHECKLIST.md.

# FluidDial-JC 1.5.1 release notes

Target: Guition JC8048W550C_I (ESP32-S3, 800x480, GT911) with an MKS DLC32 MAX V1.2 running FluidNC v4.1.1, four axes (X/Y/Z mm, A degrees).

## Wi-Fi OTA and FluidDial Updater (new in this release)

Password-protected Wi-Fi firmware update from a Windows PC with **FluidDial Updater** (`FluidDial-Updater-v1.5.1-win-x64.zip`: a portable program, no Python, PlatformIO, VS Code or administrator rights; it uses the stock Arduino espota protocol). Dual-slot with bootloader rollback, image-target check, a controller-Idle interlock and a machine-command lock on every transport during a transfer. There is no arming step: the pendant offers the update whenever a password is set and the controller is fresh and Idle. Settings, calibration and Wi-Fi survive updates; no partition migration is needed. **Security limits:** password only - the transfer is not encrypted and the firmware is not signed.

Device record, 2026-11-02 (this PC and the owner's pendant at 192.168.0.135; no machine command or motion was sent):

- Real wireless updates: stock `espota.py` once, FluidDial Updater three times (including the final image, whose bytes in the app1 slot were read back and match the release file). Each: authentication, 1.7 MB transfer, image accepted, pendant restarted and returned to the network. The boot selector switched slot each time and the new image read VALID (confirmed after 15 s of healthy running).
- Refused on the real pendant: wrong password; transfer cut off at 25 %; corrupted image ("Could Not Activate The Firmware"); a valid M5Dial image (cross-flash) - aborted at the last byte with the client told `Aborted`, no restart, boot selector byte-identical to before.
- Defects found by these tests and fixed: restoring the boot slot after a rejected image left the selector on the foreign image (now rejected before the switch, with a verified restore as a second layer; recovered over USB twice); progress-bar flicker (partial paint); saved settings of the previous format were not migrated (earlier).
- **Not tested:** update refused while the controller is not Idle (host-tested only; inducing it needs a machine command), power loss during a transfer, a crashing image being rolled back, an oversized image on the device, the firewall flow and the updater on a PC that has never had development tools, and the operator's visual check of the panel and settings after an update (the owner reported panel flashing during the first wireless update; the cause in software was fixed and the new build has not been re-checked by the operator). See [OTA_UPDATES.md](OTA_UPDATES.md).

## Firmware images

| Artifact | Environment | Purpose |
|---|---|---|
| `fluiddial-jc-1.5.1-fullcontrol.bin` | `jc8048w550_fullcontrol_test` | **Operational JC firmware.** Red "FULL CONTROL" banner. Sends operator-confirmed commands. |
| `fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin` | `jc8048w550` | Ordinary diagnostic/preview firmware. **Motion-disabled**; blue PREVIEW banner; sends only `?`. |
| `FluidDial-Updater-v1.5.1-win-x64.zip` | `tools/windows-updater` | Portable Windows updater with both images and a hash manifest. Not firmware. |

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

## Checksums (firmware source commit b8b5f82; two clean builds of an earlier tree matched; these were built once from the final tree)

```
eb8ec74f590ea33aab4c5470f0051db9a45ed3040aeef654040c0b69903d1717  fluiddial-jc-1.5.1-fullcontrol.bin
1380603b4688487692e5485887b70d95494e5ce61efd102f22e880b06289a24a  fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin
f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0  fluiddial-jc-1.5.1-usb-boot_app0.bin
2a71d69b471e20c2bac7fb469f3c6a807b3ebee780e348e5889db0da849ca363  fluiddial-jc-1.5.1-usb-bootloader.bin
1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c  fluiddial-jc-1.5.1-usb-partitions.bin
e07a2a89d4fb169960671665dbe16aa5f3d4820d9b19926ad0275bc8e3dd5fb7  FluidDial-Updater-v1.5.1-win-x64.zip
```

The full-control image above is what is installed on the test pendant (delivered by Wi-Fi OTA with FluidDial Updater; read back from flash and compared). The diagnostic image has never been installed on the pendant; it was checked by the updater's compatibility analysis and by the protocol tests only. The updater package is not bit-reproducible (rebuilding it changes the ZIP hash); the ZIP in `release/` is the one hashed here. The firmware images were built once from the final tree.
