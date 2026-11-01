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

## Program execution policy

No program start, resume, user macro or auto-restart on any transport or build. Proven by the shared command gate and policy, the static guard (`tools/check_no_sink.py`), the symbol check (`tools/check_firmware_symbols.py`) and the absence of `SD/Run`, `Localfs/Run` and `SD/Resume` from every binary.
