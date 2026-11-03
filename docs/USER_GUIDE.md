# FluidDial-JC user guide (v1.5.2, full-control image)

For the Guition JC8048W550C_I running `jc8048w550_fullcontrol_test` against FluidNC v4.1.1. FluidNC does all motion; the pendant sends operator-confirmed commands and shows the controller's replies.

**This is not an emergency stop.** FEED HOLD, CANCEL JOG and RESET are software controls. Keep the machine's physical E-stop within reach whenever commands can be sent.

## Navigation

- Power-up: the pendant loads its saved calibration, joins the saved Wi-Fi, opens the TCP link to FluidNC and enters **LIVE** only after a valid, fresh status arrives. Until then it shows DEMO or "connecting". Nothing is sent to move the machine at start-up or after a reconnect.
- **Dashboard** shows X, Y, Z (mm) and A (degrees), machine/work position selection, the active WCS, the controller state, and link freshness. Stale or missing data is shown as stale, never invented.
- **MENU** tiles: JOGGING, HOMING, PROBING, WORK OFFSETS, FILES / JOBS, MACROS (work positioning), TOOLS, SETTINGS. **BACK** returns.
- The operational (full-control) firmware has no mode banner; its build mode is shown under SETTINGS > DIAGNOSTICS. The ordinary `jc8048w550` diagnostic image keeps a blue PREVIEW - MOTION DISABLED banner and sends nothing, so it cannot be mistaken for it.
- The MENU footer shows the connection and the controller state (CONNECTED / DISCONNECTED, Idle / Run / Hold / Alarm).
- Settings: CONNECTIONS (Wi-Fi / UART / BLE, DISCONNECT, DEMO), DISPLAY, JOG PRESETS, MACHINE (read-only controller settings), DIAGNOSTICS, WI-FI SETUP, TOUCH CALIBRATION.

## Jogging (X, Y, Z in mm; A in degrees)

1. MENU > JOGGING. Choose the axis (X, Y, Z, A).
2. Choose the **STEP** (distance per press): linear 0.01 / 0.1 / 1 / 10 / 100 mm; A 0.1 / 1 / 5 / 15 / 45 / 90 degrees (six buttons on the A axis, five on X/Y/Z). The default selection is the third button (1 mm, or 5 degrees on A).
3. Choose the **FEED** (speed, per minute), four presets per axis group: X/Y 300 / 1000 / 3000 / 6000 mm/min, Z 100 / 300 / 750 / 1500 mm/min, A 100 / 250 / 500 / 1000 deg/min (these are the maxima of the owner's machine, not mandatory speeds). A feed above the axis limit is greyed out and cannot be selected: the limit is the controller's own maximum rate once the pendant has read it (the page shows "Feed limit ... from the controller"), and a selected feed above a newly read lower limit moves down, never up. Step and feed are separate controls: a 100 mm step is not a 100 mm/min feed. Switching between a linear axis and A selects 1 mm or 1 degree, never a tiny step.
4. Tap **JOG -** or **JOG +**. **One tap sends one incremental `$J=` jog straight to FluidNC**: there is no ARM button, no arming timeout and no confirmation dialog (an intentional production change in v1.5.2). Holding the button does not repeat. Taps while a jog is still unresolved are ignored; nothing is queued. **CANCEL JOG** sends the jog-cancel byte and, like FEED HOLD, needs nothing.
5. The panel shows SELECTED (axis, step, feed), RESULT (JOG REQUESTED, MOTION ACTIVE, Last jog completed, or the controller's error/alarm), MACHINE (state and connection) and the feed limit. Completion is judged from the controller's status (Idle again and the position moved as asked), not from the "ok". A failed or uncertain jog stays on screen until you tap ACKNOWLEDGE RESULT. Jogging is refused (and the reason shown) when the connection is stale or lost, the controller is in Alarm, Hold or Run, another operation is in progress, or a firmware update is running. A jog is never replayed after a reconnect.

Presets can be edited under SETTINGS > JOG PRESETS (X/Y, Z and A feeds are separate rows). Presets you saved with an earlier firmware are kept: only sets still equal to the old factory values move to the new factory values, customised feeds are kept as they were (a fourth preset is added) and no selected feed is ever raised. Going back to v1.5.1 afterwards resets the jog presets to its defaults (everything else is kept).

Other consequential operations keep their confirmation: work offsets, tool length offset, probing and positioning routines, controller reset/unlock and firmware updates. Since v1.5.3 **no function needs a separate ARM step**: homing, work-coordinate selection, work offsets and work zero, work-zero positioning, probing and centre finding, tool measurement and offsets, and unlock work as select -> confirm (once) -> FluidNC executes. A sent command closes its confirmation without a second popup; a refused one says why. Nothing is armed and nothing expires. The safety checks stay: LIVE connection, fresh status, no Alarm, the right controller state, one operation at a time, no command replay after a reconnect, no commands during a firmware update, and no program start or resume.

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

## Firmware update over Wi-Fi

SETTINGS > FIRMWARE UPDATE. Set an OTA password once (SET PASSWORD; it is never shown again). There is nothing to arm: the page says **UPDATE: READY** whenever a password is set, Wi-Fi is up and the controller is Idle with fresh status; otherwise it says UNAVAILABLE and why (after a pendant restart the controller link can take a few minutes to return). Jogging and other normal use are not affected while it is READY. On the PC open **FluidDial Updater** (no installation, no Python or PlatformIO; see [OTA_INSTALL_GUIDE.md](OTA_INSTALL_GUIDE.md)), enter the pendant IP, choose the firmware, type the password and click Update. From the moment a transfer starts ALL machine-changing commands are locked and the machine pages are blocked; the bar shows progress; after the image is checked the pendant restarts by itself and the program confirms it came back. A rejected, corrupt or interrupted update keeps the old firmware and says so (the message clears itself after a minute, or tap CLEAR FAILURE). Five wrong passwords pause the service for 5 minutes. The update path is password-protected but not encrypted or signed: use it on your own network. Do not reset the pendant in the first 15 seconds after an update.

## Errors

Controller `error:N` and `ALARM:N` are shown with their numbers. After an alarm or reset the pendant needs your explicit action (unlock); it never replays anything by itself. If the link drops, in-flight operations end as "unknown" and the display marks coordinates stale.
