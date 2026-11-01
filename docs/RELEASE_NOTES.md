# FluidDial-JC 1.5.1-rc1 release notes (release candidate, feature branch only)

Not a stable release and not merged to main. Hardware acceptance is incomplete (see below).

## Artifacts

| Image | Source commit | Version string | SHA-256 of firmware.bin |
|---|---|---|---|
| `jc8048w550_fullcontrol_test` (flashed on COM3, esptool verify_flash digest matched) | 947aef2 | `1.5.1feature/jc8048w550-947aef2` | 1DD631DB33D2518965B72066B79389E2E6579EF434038C797A670134CF829280 |
| `jc8048w550` (ordinary, motion-disabled; built, not flashed) | 947aef2 | same scheme | 34C5115C7C380738F9E0B7CEFA37D4A38287EB51522683FA93B47961586F2658 |

Build with `pio run -e <env>`; the binaries are in `.pio/build/<env>/firmware.bin` and are not committed. Docs-only commits after 947aef2 do not change the firmware.

## Implemented

Four-axis dashboard (X/Y/Z mm, A degrees, machine/work, active WCS); jogging with separate STEP and FEED presets (linear 0.01/0.1/1/10/100 mm, A 0.1/1/5/45/90 deg; feeds 300/1000/3000 mm/min and 200/500/1000 deg/min) and jog cancel; homing commands; WCS G54-G59 select, zero and offsets with readback; five work-positioning functions (Z clearance sequence for X0 Y0 Z0); guided probing, rotary centreline, tool reference and TLO; SD/job monitoring; FEED HOLD and confirmed RESET; Wi-Fi TCP auto-connect; UART and BLE transports; one shared command policy for all transports.

## Command policy (enforced, tested)

No program start or resume (`$SD/Run`, `$Localfs/Run`, `~`), no user macros, no auto-restart, no replay after a reconnect, no automatic re-arming, no motion command at boot. Ordinary image has no transmit function at all.

## Tested (tool-verified)

Host suites all pass (core 108/121, app 172/169, control 836/892, connection 228, capability 23, jog 1267/1274/1355, command interface 865, workflow 357, SD/job 182, BLE mock 330); simulator integration PASS; UI renderer 41/89/65 frames with 0 problems; static guard and firmware symbol check PASS; all five PlatformIO environments build; on-device boot: banner, GT911, saved calibration, automatic Wi-Fi, TCP, LIVE.

## Hardware results

Operator-reported only: X moved correctly (100 mm, back to Idle), Y moved fine, physical E-stop checks passed. Everything else is pending.

## Known limitations / pending physical verification

Z jog direction and holding; A direction and degrees calibration; jog cancel on hardware; X/Y homing; Z homing (positive_direction=true with only a negative limit input) and home-all; work positioning, probing, centre finding and tool measurement on the machine; probe contact and failure handling; connection-loss recovery on hardware; UART cable; BLE module; panel photos of every page. Soft and hard limits are OFF on the controller. No tool setter is configured. Saved jog presets from older firmware persist until edited. The Settings hub still has a "JOG PREVIEW TEST" simulator tile in the full-control image.
