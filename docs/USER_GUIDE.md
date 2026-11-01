# FluidDial-JC user guide (full-control image)

For the Guition JC8048W550C_I running `jc8048w550_fullcontrol_test` against FluidNC v4.1.1. FluidNC does all motion; the pendant sends operator-confirmed commands and shows the controller's replies.

**This is not an emergency stop.** FEED HOLD, CANCEL JOG and RESET are software controls. Keep the machine's physical E-stop within reach whenever commands can be sent.

## Navigation

- Power-up: the pendant loads its saved calibration, joins the saved Wi-Fi, opens the TCP link to FluidNC and enters **LIVE** only after a valid, fresh status arrives. Until then it shows DEMO or "connecting". Nothing is sent to move the machine at start-up or after a reconnect.
- **Dashboard** shows X, Y, Z (mm) and A (degrees), machine/work position selection, the active WCS, the controller state, and link freshness. Stale or missing data is shown as stale, never invented.
- **MENU** tiles: JOGGING, HOMING, PROBING, WORK OFFSETS, FILES / JOBS, MACROS (work positioning), TOOLS, SETTINGS. **BACK** returns.
- Red "FULL CONTROL" banner = commands can be sent. The ordinary `jc8048w550` image has a blue PREVIEW banner and sends nothing.
- Settings: CONNECTIONS (Wi-Fi / UART / BLE, DISCONNECT, DEMO), DISPLAY, JOG PRESETS, MACHINE (read-only controller settings), DIAGNOSTICS, WI-FI SETUP, TOUCH CALIBRATION.

## Jogging (X, Y, Z in mm; A in degrees)

1. MENU > JOGGING. Choose the axis (X, Y, Z, A).
2. Choose the **STEP** (distance per press): linear 0.01 / 0.1 / 1 / 10 / 100 mm; A 0.1 / 1 / 5 / 45 / 90 degrees. The default is the 1 mm / 5 degree button.
3. Choose the **FEED** (speed, per minute): linear 300 / 1000 / 3000 mm/min, A 200 / 500 / 1000 deg/min. A feed above the axis' controller limit is greyed out. Step and feed are separate controls: a 100 mm step is not a 100 mm/min feed.
4. Press **ARM JOG** once per session and confirm. Then JOG - / JOG + send one incremental `$J=` jog each. **CANCEL JOG** sends the jog-cancel byte.
5. The result line shows FluidNC's reply (ok, error:N, alarm) and the position display shows the actual movement. "ok" means the jog was accepted, not finished; completion is the return to Idle.

Presets can be edited under SETTINGS > JOG PRESETS. Presets saved on the device from an older firmware stay until edited.

## Work coordinates

- WORK OFFSETS: select G54 to G59 (sends only the selection), zero X, Y or Z (`G10 L20`, active WCS only), or enter an offset (`G10 L2`), then read the offsets back from the controller. Machine zero, machine home, work zero and the tool length offset are different things and are displayed separately.

## Positioning (MENU > MACROS > POSITIONING)

GO TO WORK X0, Y0, Z0, X0 Y0, X0 Y0 Z0. Targets are work coordinates of the active WCS, not machine home, and the A axis is never moved. Each shows current and target values and asks for confirmation. Z0 is not necessarily a safe height. X0 Y0 Z0 asks for a clearance height you enter, raises Z, **pauses for CONTINUE**, moves X/Y, **pauses for a second confirmation**, then lowers to Z0. Cancel at either pause stops the sequence. Loss of the link, an error, an alarm, a reset or a timeout ends the sequence; nothing is resent after a reconnect.

## Homing

MENU > HOMING sends FluidNC's `$H` commands. X and Y can be homed one axis at a time. **Z homing and home-all stay unused until the Z switch placement and direction are verified** (see [COMMISSIONING](COMMISSIONING.md)). A has no homing. Ordinary jogging does not require homing.

## Probing, centre finding and tools

PROBING offers guided inside/outside edge and centre routines, the rotary centreline workflow and a probe-pin display. A probe result is used only if the controller reports the probe contact; nothing is assumed from an "ok". Calculated offsets are shown and written only after you press the explicit SET/confirm button. TOOLS: reference measurement, subsequent tool measurement, TLO preview/apply (`G43.1`), clear (`G49`) and readback. No dedicated tool setter is configured; the manual probe procedure is used.

## Files and jobs (monitoring only)

FILES / JOBS lists the controller SD card and shows job name, state and progress. The pendant **cannot start, resume or restart a program** on any transport, and cannot run macros. Use the PC. Available: **FEED HOLD (PAUSE)** and a confirmed **RESET (ENDS THE JOB)**.

## Errors

Controller `error:N` and `ALARM:N` are shown with their numbers. After an alarm or reset the pendant needs your explicit action (unlock/re-arm); it never re-arms or replays anything by itself. If the link drops, in-flight operations end as "unknown" and the display marks coordinates stale.
