# JC full-control commissioning checklist (Instructions 009 / 009A / 010)

**Nothing below has been done.** The full-control image (`env:jc8048w550_fullcontrol_test`) is built and tested
against a simulated FluidNC only. It must not be flashed until the independent stopping system is installed,
functionally verified, and the owner explicitly authorizes the flash. FluidNC is not modified at any step.

## 0. Prerequisites (owner)
- [ ] Independent emergency stop installed (hardware, not the pendant) and functionally tested; written result.
- [ ] Written procedure and a clear workspace, spindle removed or unable to start, no workpiece for the first tests.
- [ ] Z homing direction and Z home switch side verified physically. A axis calibration / orientation recorded.

## 1. Communication (ordinary image first)
- [ ] Wi-Fi connects by itself; endpoint shown (LINK page / dashboard) is the saved one; edit it; reconnect after Wi-Fi loss.
- [ ] Real read-only queries: Machine > capabilities read; Homing page shows configured / fitted / active for X, Y, Z; A shows `?`.

## 2. Jog (full-control image)
- [ ] Arm; X+ / X- 0.1 mm at 100 mm/min; the position display changes by that amount; cancel while moving stops it.
- [ ] Y, then Z (up first, clear travel), then A (confirm CW / CCW against the machine and the sign convention).
- [ ] Wi-Fi loss during a jog: unresolved, cancel attempted, no replay after reconnect; arming cleared.
- [ ] Unhomed machine accepts jogs (no homing needed) unless the controller says otherwise (must_home).

## 3. Homing (only after section 0 Z checks)
- [ ] HOME X, HOME Y individually, then Z after the session confirmation, then HOME ALL. Reply arrives at the end of the cycle.
- [ ] A shows "no homing"; nothing sends `$HA`.

## 4. Work offsets and positioning
- [ ] G54..G59 select shows the active WCS; GO TO WORK X0 Y0: clearance Z entered by you; Z moves first; the XY move waits for CONTINUE; arrives; A and offsets unchanged.

## 5. Probing (physical probe validated first)
- [ ] Probe input assigned; the probe pin state toggles (Pn:P) when touched by hand; "already triggered" is refused.
- [ ] FIND X CENTRE on a known bore: measured size equals the gauge; wrong tip radius is flagged.
- [ ] FIND Y, FIND XY; MOVE TO CENTRE; SET WORK X0 / Y0: offsets read back match ($#); no G92.
- [ ] Outside feature (boss) X / Y; contact timeout and no-contact reported; stop with cancel.
- [ ] FIND ROTARY A CENTRELINE on a reference cylinder: Y / Z centreline and tilt plausible; A calibration unchanged.

## 5a. GO TO WORK positioning (full-control image, spindle removed, owner authorization first)
- [ ] GO TO WORK Z0: nothing moves until the explicit CONFIRM; Z reaches work Z0 of the ACTIVE WCS (check with G55 selected), X/Y/A and the offsets in `$#` are unchanged.
- [ ] GO TO WORK X0 Y0 Z0: enter a clearance above Z0; Z rises, X/Y move, then a SECOND confirmation is required before Z lowers to Z0; CANCEL at that pause leaves Z at the clearance.
- [ ] Work zero is not machine home: the machine position at work X0 Y0 Z0 differs from the homed position unless the origin was set there.

## 6. SD card and jobs (ordinary image first, then full-control)
- [ ] Ordinary image, real controller: FILES > REFRESH SD LIST shows the files that are really on the controller's SD card (compare with the card / WebUI). The parser matches the v4.1.1 reply read from this controller (relative names, folder depth as leading spaces, `[/sd Free: Used: Total:]` summary). Record any difference.
- [ ] Remove the card: the page says NO SD CARD (error:60); empty card: SD CARD HAS NO FILES; the pendant's own flash files never appear.
- [ ] Long names and deep folders display ellipsized without overlap; a name with a space or `#` is listed and can be selected (nothing on the pendant starts a file).
- [ ] The Files page has NO START JOB and NO CYCLE START / RESUME control (Instruction 010A). Start a harmless program (spindle removed, no workpiece) **from the PC**; the pendant shows its name and the percentage from the controller's `SD:` field, not a timer.
- [ ] Full-control image only, with the owner's explicit authorization: FEED HOLD (`!`) pauses at once and is labelled "FEED HOLD (PAUSE)"; resume it from the PC. RESET (Ctrl-X) always shows its confirmation dialog (job ends, cannot be resumed from here, position may be lost, not an E-stop).
- [ ] Macros page: RUN and EDIT are disabled and explain why; no macro file can be run from the pendant.
- [ ] Pull the Wi-Fi link during a job: the page shows JOB STATE UNKNOWN; after reconnecting nothing is restarted or resumed (a held job stays held); the controller's job state is simply re-read.

## 7. Tools and tool length (full-control image; spindle removed)
- [ ] READ TOOL STATE shows the controller's T word and `[TLO:]`. If `[TLO:]` is not reported, the tool routines stay disabled.
- [ ] Measure the reference tool on a known surface: the pin must react to a hand touch before any probing move; contact is stored (machine Z).
- [ ] CHANGE TOOL BY HAND + MEASURE: Z rises to your clearance, waits for you; no M6 / T is ever sent (check the serial log).
- [ ] APPLY TOOL OFFSET sends `G43.1 Z<x>` and the readback matches. **Verify the sign convention on the machine**: after applying the offset for a tool that is longer by d, the work Z display at the same physical tip position must be unchanged. If FluidNC v4.1.1 does not accept G43.1 / G49, record the error; nothing else depends on it.
- [ ] CLEAR TOOL OFFSET (G49) reads back 0. Work offsets (`$#` G54..G59) are identical before and after.

## 8. Positioning and centre finding (review of 009A)
- [ ] The planning preview shows the safe Z clearance and the XY move as separate steps; clearance is operator-entered and is not claimed to be clear of obstacles.
- [ ] Inside X / Y, outside X / Y, XY inside and the rotary reference cylinder are checked against gauge pins of known diameter; the measured size equals the gauge and a wrong tip radius is flagged.
- [ ] No work offset changes unless SET WORK ORIGIN is confirmed; the readback matches.

## 9. Bluetooth LE UART (needs the external module; NOT verified)
- [ ] CC2541 / HM-10 wired to the controller UART at the right baud rate; module advertises FFE0/FFE1. Select BLE on CONNECTIONS, open BLE SETUP, SCAN: the module appears with the `serial` marker.
- [ ] Connect: the page reaches READY, `?` status frames arrive, the dashboard shows 4 axes. Record the negotiated chunk size (20 bytes if the MTU stays at 23).
- [ ] Fragmentation: long `$SD/List` and `$#` replies arrive intact (invalid frames stay 0, dropped stays 0).
- [ ] Power-cycle the module: the page shows RECONNECTING and returns to READY by itself; nothing queued earlier is sent.
- [ ] Wi-Fi, UART and BLE are never active together: switching transport stops the others and the dashboard shows no data until the new one delivers a frame.

## 10. Still not implemented / not claimed
Automatic tool changer or M6 macro (never assumed), rotary-axis probing, outside-feature XY centre in one routine (use X then Y), SD file deletion / upload / rename (deliberately not offered), file names with spaces, job resume after a controller reset, any BLE hardware verification, any physical commissioning.
