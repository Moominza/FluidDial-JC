# JC machine-control readiness report (Instruction 005)

Status: **experimental. Machine control remains disabled. Nothing that moves the machine is transmitted.**
Branch: `feature/jc8048w550`. Hardware: Guition JC8048W550C_I (COM3). Controller: MKS DLC32 MAX V1.2, FluidNC v4.1.1, `192.168.0.134:23`.

This report separates what was measured on the real controller from what was only simulated.

## 1. Actual controller capabilities (real hardware)

Source: operator photograph of the Controller Capabilities page, taken 2026-10-30 07:34 after the operator tapped
READ FROM CONTROLLER in LIVE mode. The page reported "Read complete: 26 answered, 2 refused, 2917 ms" and
"Motion limits: confirmed by the controller for X Y Z A". No serial capture of this read exists (the capture
window ran before the operator tapped READ), so the photograph is the only evidence. Values were transcribed from
the photograph.

| Axis | Steps/mm | Max travel (mm) | Max rate (mm/min) | Acceleration (mm/s²) | Homing |
| --- | --- | --- | --- | --- | --- |
| X | 40 | 1000 | 6000 | 250 | cycle 1 |
| Y | 40 | 2300 | 6000 | 250 | cycle 1 |
| Z | 200 | 150 | 1500 | 100 | cycle 1 |
| A | 1000 | 3000 | 1000 | 25 | `error:3` (refused) |

| Item | Value |
| --- | --- |
| Firmware | `4.1 FluidNC v4.1.1 (esp32s3-wifi) :` (the trailing ` :` is part of the controller's `[VER:...]` line; the UI now strips it) |
| Modal state | WCS G54, tool T0 |
| Probe pin | `gpio.48:low` |
| Toolsetter pin | `NO_PIN` |

Findings:

* The real controller answered the `$I`, `$G`, `$/probe/*` and `$/axes/<x|y|z|a>/<item>` queries in the formats the
  parser assumed. This partly closes the earlier "reply formats unverified" item (see section 5).
* Two queries were refused. The A-axis homing cycle shows `error:3`; the second refused query is not visible in the
  photograph and is most likely `$/axes/a/homing/positive_direction` (inferred, not observed). The
  A axis therefore has no homing configuration on this controller, and homing for A stays unavailable.
  *Update 2026-10-30: confirmed by the 53-query capture, which shows A's `homing/cycle` and
  `homing/positive_direction` (and the newly added `homing/mpos_mm`) refused with `error:3`; see
  [jc-live-jog-readiness.md](jc-live-jog-readiness.md) section 2.2.*
* All four axes have steps/mm, travel, max rate and acceleration, so all four have **confirmed limits**. Placeholder
  limits are no longer used on this machine while the connection stays up. The values are RAM-only and are discarded
  when the link is lost.
* **A is not verified as rotary.** FluidNC does not report units. 1000 steps/mm, 3000 travel and 25 acceleration are
  consistent with degrees, but only the operator can confirm that. It stays operator-declared.
* The jog step limit still equals the full travel (up to 2300 mm on Y). This is not a safe jog limit
  (section 5).

Not yet captured: photographs of the Dry Run page and the Machine page LIMITS column on real values.

## 2. Command acknowledgement handling

`JcCommandExec` allows one request in flight and completes it on one of:

| Controller reply | Outcome |
| --- | --- |
| `ok` | Acked |
| `error:N` | ControllerError (N and its text are recorded) |
| `ALARM:N` | Alarm (remaining steps dropped) |
| nothing within the class timeout | TimedOut (jog 2 s, home 120 s, probe 60 s, query 2.5 s) |
| link lost | LinkLost |

A request is never resent. After a timeout or abandonment a 1 s quiet period applies, so a late `ok` is counted as
unsolicited and never mistaken for the next request's reply. A reconnect discards all state and replays nothing.

## 3. Dispatcher tests

All run on the host. Counts after this checkpoint:

| Suite | Machine control off | Machine control compiled in (test only) |
| --- | --- | --- |
| Core | 111 | 125 |
| App | 168 | 165 |
| Control (response, splitter, allowlist, exec, loader, dispatcher, dry run) | 734 | 790 |

All pass. Covered: valid and invalid controller responses, acknowledgements, controller errors, timeouts,
connection loss during a command, stale machine state, unexpected alarms, four-axis command validation, sequences
released one step at a time only after an acknowledgement, and reconnect without command replay. The dispatcher
tests use a fake transport installed only in the host build.

Simulated FluidNC server (`tools/sim_fluidnc_server.py`, `tools/sim_caps_tests.py`): PASS. It audits every line it
receives and flags anything that is not `?` or an allowlisted read-only query. The audit self-test confirms it
flags `$H`, G-code, `$J` and a real-time byte. The simulator proves the host logic only. It cannot prove FluidNC
behaviour, and it does not exercise the ESP32 network stack.

`tools/check_no_sink.py`: PASS. Firmware code installs no transmit sink, `JC_MACHINE_CONTROL_ENABLED` is not set,
and there is no raw transmit site outside the two read-only links.

## 4. Build and device state

* Builds: `jc8048w550`, `m5dial`, `cyddial` all succeed.
* The firmware was uploaded to COM3 with data hash verified. `JC_MACHINE_CONTROL_ENABLED` is 0, so every command
  class is blocked by the build policy.
* The only transmissions are `?` and the 28 allowlisted read-only queries, sent after the operator taps READ.
* The controller configuration was not modified and no machine-changing command was sent.
* The page flashed while reading; it now repaints only the cells that changed. The operator confirmed on the
  device that the flashing is gone.

## 5. Outstanding safety requirements before any motion is enabled

1. A separate safety review and explicit owner approval to enable `JC_MACHINE_CONTROL_ENABLED` or to add a transmit
   sink. Not given.
2. Verify the reply formats for the **response to a command** (`ok` / `error:N` / `ALARM:N` after a motion command,
   `[PRB:...]` after a probe). Only query replies were seen on the real controller; no command was sent.
3. Confirm with the operator that A is rotary (degrees) and decide its feed semantics. FluidNC cannot report this.
4. Replace "jog step limit = full travel" with a deliberate per-axis jog limit (for example a small fixed step and
   a feed cap) before any jog is allowed.
5. Read whether soft limits and hard limits are enabled on the controller (not currently queried).
6. Resolve A homing: the controller reports no homing configuration for A, so homing must stay blocked for A.
7. Sequences and probe-result handling are host-tested but have never run against a controller.
8. The on-screen STOP is not an emergency stop. The physical emergency stop remains the only emergency stop.
9. A first real-motion test, if approved, needs its own written procedure with the machine safe, a clear
   workspace and an operator at the emergency stop.

**Stop point:** real motion is not enabled, and this report does not request that it be.
