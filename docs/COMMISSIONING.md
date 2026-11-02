# FluidDial-JC commissioning record

Provenance matters here: **operator-reported** means the owner/operator told us; **tool-verified** means the development tooling observed it. Nothing below was independently witnessed by the development agent unless marked tool-verified. The detailed step list is in [development/jc-commissioning-procedure.md](development/jc-commissioning-procedure.md) and [the checklist](development/jc-commissioning-checklist.md).

## Hardware

Guition JC8048W550C_I (ESP32-S3, 800x480, GT911) on COM3; MKS DLC32 MAX V1.2 running FluidNC v4.1.1; axes X/Y/Z mm and A degrees.

## Controller compatibility (tool-verified, read-only, 2026-11-01)

FluidNC v4.1.1, `?` reports four-axis MPos, G54 active, offsets and TLO zero, probe pin `gpio.48:low`, no tool setter, `must_home=false`, soft and hard limits false on all axes. Limit inputs: X `gpio.41`, Y `gpio.40`, Z `gpio.39` (all negative); A has none. See [evidence/2026-11-01-controller-readonly-014.md](development/evidence/2026-11-01-controller-readonly-014.md).

## E-stop verification (operator-reported 2026-11-01, no date/name recorded)

Stops hazardous motion independently of the touchscreen and Wi-Fi; spindle cannot start while engaged; Z remains supported; release causes no automatic movement or spindle start; a deliberate reset/re-enable is required; pendant stays powered and does not replay commands. Not recorded: a stop during a move in progress, as a separate item. Soft and hard limits are still OFF, so the E-stop is the only protection against travel limits.

## Firmware on the device

`jc8048w550_fullcontrol_test`, flash verified with esptool `verify_flash` (digest matched), boot log shows the FULL CONTROL banner, saved calibration, automatic Wi-Fi, TCP and LIVE (tool-verified). Exact revision and hash: see [RELEASE_NOTES](RELEASE_NOTES.md).

## Axis results

| Axis | Result | Provenance |
|---|---|---|
| X | Moved in the correct direction, deliberate 100 mm move, FluidNC returned to Idle | operator-reported |
| Y | Moved, "fine" | operator-reported |
| Z | **PENDING** direction, holding, clearance | not tested |
| A | **PENDING** direction and degrees-per-step against a mark/protractor; `steps_per_mm=1000` with max travel 3000 is not proof of angular calibration | not tested |
| Jog cancel | **PENDING** on hardware | not tested |

## Homing

**PENDING.** X and Y can be tried one axis at a time (`$HX`, `$HY`). **Z discrepancy open:** `homing/positive_direction=true` but the only Z limit input is `limit_neg_pin gpio.39`. Physically check where the Z switch is and which way Z homes before any Z homing or home-all. The controller configuration is not changed by the pendant. A has no homing.

## Work positioning, probing, tools

All **PENDING** on hardware (simulator-tested only). Before using them: establish the work origin and a safe clearance path; verify the probe input by touching the stylus by hand and watching `Pn:P`; check the tip radius; test failure handling by a deliberate no-contact probe at a safe distance; confirm the sign of the tool-length offset against the machine.

## Communications

- Wi-Fi TCP: auto-connect, LIVE, manual DISCONNECT and DEMO observed on hardware (tool-verified from the boot log). Temporary controller loss recovery on hardware: PENDING.
- UART: implemented, host-tested; cable and wiring **not verified**.
- BLE: implemented against a UART-style BLE module (e.g. HM-10/CC2541 class), host-tested with a mock radio; **no hardware verified**. A classic-Bluetooth HC-06 is not compatible with the ESP32-S3.

## Owner acceptance, 2026-11-02 (Instruction 019)

**Every PASS below is "PASS - operator-reported, observations not supplied."** It is the owner's acceptance, not instrumented validation: no distances, angles, homing accuracy, stopping distances, probe repeatability, electrical measurements, controller logs or module identities were supplied, and none are claimed.

| Item | Result |
|---|---|
| X-axis movement (reported earlier: correct direction, 100 mm, Idle) | PASS - operator-reported, observations not supplied |
| Y-axis movement (reported earlier) | PASS - operator-reported, observations not supplied |
| Z-axis jogging | PASS - operator-reported, observations not supplied |
| A-axis rotation | PASS - operator-reported, observations not supplied |
| Jog cancellation | PASS - operator-reported, observations not supplied |
| Limit switches and homing | PASS - operator-reported, observations not supplied |
| Probe input | PASS - operator-reported, observations not supplied |
| Work-zero positioning | PASS - operator-reported, observations not supplied |
| Tool and rotary workflows | PASS - operator-reported, observations not supplied |
| Connection-loss recovery | PASS - operator-reported, observations not supplied |
| Physical touchscreen | PASS - operator-reported, observations not supplied (no panel photos in the repository) |
| UART and BLE | PASS - operator-reported, observations not supplied (wiring, adapter and BLE module identity not supplied) |
| Physical E-stop (2026-11-01, six checks) | PASS - operator-reported, no date/name recorded |

## Controller limitations that remain on record

Re-read on 2026-11-02 ([evidence](development/evidence/2026-11-02-controller-readonly-019.md)); the configuration was unchanged:

- **Z homing:** `positive_direction=true` while the only Z limit input is `limit_neg_pin gpio.39`. No available evidence reconciles this, and the "limit switches and homing" PASS does not explain it. It is not claimed to be corrected.
- **Soft limits OFF and hard limits OFF** on all axes. The E-stop is the only travel protection.
- **No dedicated tool setter** is configured (`toolsetter_pin=NO_PIN`); tool measurement is manual-probe based.
- **A axis** has no homing and no limit input.
- Machine position is reported in MPos only; the pendant derives work coordinates from the offsets.

## Wi-Fi OTA (Instructions 020 and 021)

Device results from 2026-11-02 (tool-verified from this PC; the pendant stayed on the owner's network and COM3; **no machine command and no motion was sent** by any of these tests): first install over USB (app + boot selector only; verify matched; the partition table, bootloader and otadata already equalled the build; NVS not erased); real wireless updates with the stock `espota.py` and with FluidDial Updater (authentication, 1.7 MB transfer, image accepted, pendant restarted and returned to the network, boot selector switched to the other slot and the image confirmed VALID, installed bytes identical to the release file); wrong password refused; transfer cut off at 25 % and corrupted image refused with the old firmware kept; a valid M5Dial image (cross-flash) refused. See [OTA_UPDATES.md](OTA_UPDATES.md) for the table of what is host-tested, device-tested and not tested, and RELEASE_NOTES for the record.

Findings on record: (1) after the first wireless update the owner reported the panel flashing during the transfer; the progress bar redrew about 100 times, fixed (partial paint); residual flicker from flash writes has not been re-checked by the operator. (2) The cross-flash test showed that restoring the boot slot after a rejected image did not reliably work on the real pendant (the selector stayed on the foreign image; recovered over USB); the check was moved before the switch and the restore now verifies. (3) At the owner's request the touchscreen ARM UPDATE step was removed; the Idle/fresh-status interlock stays. (4) After a pendant reset FluidNC can keep the previous telnet session for a while, so the controller link (and with it the update offer) returns after 3 to 6 minutes; existing reconnect behaviour, not an OTA effect. (5) A test password was exposed in conversation: change the OTA password.

Hardware acceptance items that remain with the owner: a visual check on the panel of the OTA page and of the settings after an update, and the Windows updater on a PC that has never had development tools (not available to the development environment). Nothing in the owner-acceptance table above changed.

## Program execution policy

No program start, resume, user macro or auto-restart on any transport or build. Proven by the shared command gate and policy, the static guard (`tools/check_no_sink.py`), the symbol check (`tools/check_firmware_symbols.py`) and the absence of `SD/Run`, `Localfs/Run` and `SD/Resume` from every binary.
