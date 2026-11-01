# Full-control commissioning procedure (Instruction 013)

Status: **PREPARED, NOT STARTED.** The full-control image (`env:jc8048w550_fullcontrol_test`) is built and software-verified but has **not been flashed**, and no machine-changing command has been transmitted to any controller.

## 0. Hard prerequisite: independent stop

The owner has confirmed: no independent physical E-stop is installed; soft limits OFF; hard limits OFF; machine position not established.

**Do not flash the full-control image and do not send physical motion until an independent stopping arrangement is installed and functionally verified (its own test, recorded here, signed off by the owner).** FEED HOLD and RESET on the pendant are not an emergency stop. This section is not marked complete on an assumption.

- [ ] Independent stop installed (describe): ____
- [ ] Functionally verified with the spindle/drives in a safe state (date, who): ____
- [ ] Owner written authorization to flash the full-control image: ____

## 1. What the full-control image does (verified in software)

Enabled through ONE shared command system (gate → `JcCommandSystem` → `checked_tx` → `jc_command_bytes_allowed`, used by Wi-Fi, UART and BLE): incremental jog X/Y/Z (mm) and A (degrees), jog cancel (0x85), homing (`$H`, `$H<axis>`), feed hold (`!`), WCS selection (G54..G59), work-offset set (`G10 L20` / `G10 L2`), guided probing (`G38.2`), tool length offset (`G43.1` / `G49`), built-in positioning (`G21 G90 G0` Z, X/Y), `M5`, `$X`, and a confirmed reset (0x18). FluidNC is the authority on motion; the pendant does not require homing before jogging and has no motion planner.

**Never enabled, in any build or transport:** `$SD/Run`, cycle start / resume (`~`), user macro files, any `$SD/…` or `$Localfs/…` command. Read-only SD browsing and job monitoring remain.

Proof, repeatable: `python tools/check_firmware_symbols.py` (after building the environments) shows the ordinary image has no `send_command_bytes`, the full-control image has the Wi-Fi and UART ones behind the shared policy, and no image contains a program-start string.

Controller replies: `ok` acknowledges (a motion command then waits for FluidNC to report Idle at the verified position), `error:N` and `ALARM:N` are shown with their numbers and end the operation, no reply within the class timeout ends it as "unknown", loss of the connection ends everything in flight. Nothing is ever resent or replayed after a reconnection; reconnecting restores monitoring, never arming.

## 2. Functional tests, in this order (each after the previous passed)

Use the smallest steps, the spindle removed or off, nothing in the work area, one person at the stop. Record the pendant's result line and FluidNC's reply for each.

1. **X incremental jog:** +/- 1 mm at 100 mm/min. Direction and distance match a dial indicator. Repeat 10 mm.
2. **Y incremental jog:** same.
3. **Z direction and positioning:** + must move AWAY from the work (up). 0.5 mm steps, then 5 mm. Confirm mm units against a gauge.
4. **A rotary:** +5 degrees, then 45 degrees; confirm direction and degrees-per-step with a mark/protractor. A is in degrees, never mm.
5. **Verify the homing configuration without homing:** READ FROM CONTROLLER shows the homing switches/cycles and soft/hard limits (read-only). Confirm Z's homing configuration is what you expect. Do not change the FluidNC configuration from the pendant.
6. **X/Y homing, one axis at a time** (`$HX`, then `$HY`) before any all-axis home. Do not use automatic homing as the first test.
7. **Work Z0:** with a known WCS, GO TO WORK Z0 (confirm). Verify it is work Z0 of the active WCS and not machine home.
8. **Work X0 Y0 Z0:** enter a clearance above Z0; check pause 1, X/Y, pause 2, lowering. Cancel at pause 2 once to prove the lowering is not sent.
9. **Probe contact:** touch the stylus by hand, confirm the pin state changes (`Pn:P`) before any probing move.
10. **Guided centre finding** on a known gauge (bore / boss): size equals the gauge, wrong tip radius is flagged; no offset is written without the explicit SET button; read-back matches `$#`.
11. **Tool measurement:** reference tool, then change tool by hand and measure; confirm the sign of the offset on the machine (G43.1) before relying on it.

Stop at the first unexpected result and record it; do not continue to the next test.

## 3. Evidence to keep

Photographs of the pendant at each step, the serial log of the session, FluidNC's replies, the date, the firmware revision (`git describe`) and the image hash.

## 4. Instruction 016 status (2026-11-01, baseline b3a0d40)

Sections 1-2 (software): baseline verified (feature/jc8048w550, HEAD b3a0d40, clean, main 47f8489 unchanged). `jc8048w550` and `jc8048w550_fullcontrol_test` rebuilt: SUCCESS; `check_firmware_symbols.py` PASS (ordinary image has no transmit function; full-control has Wi-Fi and UART transmit behind the shared policy; no program-start string in any image); `check_no_sink.py` PASS.

Section 3 (physical stop): **NOT VERIFIED.** No independent stop, Z hold-on-power-removal, reset-does-not-restart, or spindle-stop evidence has been supplied. Soft limits OFF, hard limits OFF, position not established (unchanged). Owner has not authorized full-control flashing.

Sections 4-8 (axis tests, homing, work-positioning on hardware, probe/tool, hardware connection-loss test): **NOT STARTED** - blocked by section 3. No full-control image was flashed; no machine-changing command was sent. The Z positive-direction / negative-limit-pin discrepancy (`$/axes/z/homing/positive_direction=true`, limit on `limit_neg_pin gpio.39`) remains unresolved and must be physically checked before any Z or home-all.

## 5. Instruction 017: E-stop functional verification record (REQUIRED BEFORE FLASHING FULL CONTROL)

The owner reports the E-stop is wired (2026-11-01). That is a report, not a test result. Gate: every line below must be answered by the operator who performed it, with date. Any NO or untested line blocks flashing.

| # | Check (performed with the spindle off and nothing in the work area first) | Result | Date / who |
|---|---|---|---|
| 1 | E-stop cuts motor/drive power (or otherwise stops all motion) with NO involvement of the pendant or FluidNC software | | |
| 2 | Pressing it during a slow FluidNC motion (PC or web UI, small move) stops that motion immediately | | |
| 3 | Spindle stops / cannot run while E-stop is pressed | | |
| 4 | Z axis does not drop or free-fall with drive power removed (brake / leadscrew self-holding) | | |
| 5 | Releasing the E-stop does NOT start any movement or spindle by itself | | |
| 6 | After release, FluidNC needs an explicit operator action (reset / unlock) before motion; state shown | | |
| 7 | Pendant (COM3) still powers and shows state while the E-stop is pressed; no command replays on recovery | | |
| 8 | Z homing switch location and direction physically checked (config says positive_direction=true with limit_neg_pin gpio.39) | | |

Only after rows 1-7 are YES: flash `jc8048w550_fullcontrol_test`, verify hash and boot, then the X, Y, Z, A tests per section 2 of this document.
