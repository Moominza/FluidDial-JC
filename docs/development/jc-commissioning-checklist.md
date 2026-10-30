# JC full-control commissioning checklist (Instructions 009 / 009A)

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

## 6. Not implemented yet (do not test)
SD browsing, job progress / pause / resume UI, tool-change workflow, TLO from a tool probe, BLE UART.
