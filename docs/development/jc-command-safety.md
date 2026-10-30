# JC command safety design

Status: **experimental**. Machine control is disabled in the shipping firmware.

## Layers

| Layer | File | Role |
| --- | --- | --- |
| Status parsing | `src/jc8048w550/JcStatusFrame.hpp` | Pure parser used by firmware and host tests. Accepts only a known state, exactly four finite MPos/WPos values and an FS pair. Also extracts WCO, feed, spindle and `Pn` pins. `jc_convert_position()` refuses to convert between machine and work coordinates when no WCO is known. |
| Command generation | `src/jc8048w550/JcCommandBuilder.hpp` | Pure formatting and validation of jog, cancel, hold/resume, reset, unlock, home, WCS select, axis zero/set, probe move and Z probe sequence, SD file run, macro run and spindle stop. Formats mirror the pinned upstream FluidDial scenes. Nothing is transmitted. |
| Authorization | `src/jc8048w550/JcCommandAuth.hpp` | `JcCommandGate` is the single submission point for every machine-changing command. |
| Transport | `JcReadOnlyFluidNc`, `JcUartLink` | Only `?` and one allowlisted read-only query line (`send_query_line()`, checked by `jc_query_allowed()` in `JcQuery.hpp`) can leave the device; everything else is blocked and counted. The gate has no transmit sink. |

## Policy

* `JC_MACHINE_CONTROL_ENABLED` defaults to `0`. With it, `jc_authorize()`
  returns `BlockedBuildPolicy` for every command class; the host tests
  assert this for all classes.
* Even when compiled in for host tests, an authorized command only reaches a
  sink if one is supplied. The firmware supplies none.
* When enabled, authorization additionally requires: operator arming, LIVE
  mode, fresh status, a command no older than 500 ms (no stale jog replay),
  no alarm (except stop/unlock/reset), a state that permits the class, and
  operator confirmation for home, probe, offsets, spindle, jobs, macros and
  unclassified commands.
* Reconnecting never refreshes data or re-arms control.
* The A axis is rotary (degrees); it is never probed, and its limits are
  validated separately from the linear axes.
* The on-screen STOP is not an emergency stop and must never be presented as
  a replacement for the machine's physical emergency stop.

## Limitations to resolve before any motion is enabled

* Axis limits and maximum feeds in `jc_default_profile()` are conservative
  placeholders, not values read from the controller (`$/axes/*`).
* Mixed linear/rotary feed-rate semantics are not modelled; only single-axis
  moves are generated.
* The command-timeout and response-correlation layer does not exist yet.
* The FluidNC v4.1.1 command syntax has been checked only against the pinned
  upstream FluidDial source, not against a live controller.
* A separate safety review and explicit owner approval are required to enable
  `JC_MACHINE_CONTROL_ENABLED` or to add a transmit sink.

## Instruction 004 additions

* `JcDispatcher` (`JcDispatch.hpp`) is the only path from UI pages to the
  gate. It keeps no queue and has no resend API, so a command can never be
  replayed after a connection loss; `on_link_lost()` disarms, and nothing
  re-arms on reconnect. Nothing in the shipping UI calls `arm()`.
* Every motion-capable page (jog, homing, probing, offsets, files/jobs,
  macros, tools) uses the same staged flow: command preview, confirmation
  dialog, dispatcher, blocked-result dialog. Rejected commands (bad axis,
  value, feed or path) are recorded as rejected and never reach the gate.
* DEMO mode and simulated values can never authorize anything: the gate
  requires LIVE mode and fresh live status, and the shipping build blocks
  every class regardless.
* Link freshness is one shared rule (`jc_status_fresh`) and the only
  permitted outgoing byte is `?` (`jc_tx_byte_allowed`), used by the Wi-Fi
  link, the UART link and the host tests. (Instruction 005 adds one further,
  separately checked path: allowlisted read-only query lines; see below.)
* The software cancel / STOP controls are not emergency stops and are
  labelled as such on the jogging page and in the job-cancel dialog.

## Instruction 005 additions (machine-control preparation; motion still disabled)

* **Read-only queries.** `JcQuery.hpp` defines the only text besides `?` that may
  be transmitted: `$I`, `$G`, `$/probe/pin`, `$/probe/toolsetter_pin` and
  `$/axes/<x|y|z|a>/<steps_per_mm|max_travel_mm|max_rate_mm_per_min|
  acceleration_mm_per_sec2|homing/cycle|homing/positive_direction>`. A line with
  `=` (an assignment), a space, a control character or any other text fails
  `jc_query_allowed()`. (Instruction 006 added `soft_limits`, `motor0/hard_limits`,
  `motor0/limit_{neg,pos,all}_pin`, `homing/mpos_mm` per axis and `$/start/must_home`: 53
  fixed lines in total, all reads.) Queries are sent one at a time, only after the operator
  taps READ FROM CONTROLLER, only in LIVE mode with fresh status in the Idle or
  Alarm state, and are never repeated automatically. Nothing is written to the
  controller and its configuration is not modified.
* **Replies and timeouts.** `JcResponse.hpp` classifies controller lines (`ok`,
  `error:N`, `ALARM:N`, `[MSG:]`, `[GC:]`, `[VER:]`, `$path=value`).
  `JcStreamSplitter.hpp` separates them from status frames. `JcCommandExec.hpp`
  allows one request in flight, completes it on `ok` / `error:N` / `ALARM:N`,
  times it out (2.5 s for queries; per-class values for commands: jog 2 s, home
  120 s, probe 60 s), and reports a lost connection. A request is never resent.
  After a timeout or abandonment a 1 s quiet period applies, so a late `ok` is
  counted as unsolicited and never mistaken for the next request's reply.
* **Controller values replace placeholders only when confirmed.**
  `JcCapsLoader.hpp` / `JcMachineCaps.hpp` validate each value (finite, within
  plausibility ranges, never clamped). An axis's limits become "confirmed" only
  when steps/mm, travel, maximum rate and acceleration were all read and valid;
  then `jc_profile_apply_caps()` sets that axis's maximum step (= travel) and
  feed. Homing permission follows `homing/cycle` (cycle 0 = not homed); for A the
  operator's "has a home switch" declaration can only restrict it. The values are
  RAM-only and are discarded when the connection is lost or the mode leaves LIVE.
* **Authorization.** `JcAuthDecision::BlockedLimitsUnconfirmed`: when machine
  control is compiled in, a jog or probe move on an axis whose limits are still
  placeholders is refused. Stop / hold / recovery commands are never gated on it.
* **Dispatcher.** With a transmit sink installed (host tests only) a sequence is
  sent one step at a time: the next step is released by `advance_sequence()` only
  after an acknowledgement, re-authorized with a fresh context; any error, alarm,
  timeout or link loss drops the remaining steps. `tools/check_no_sink.py` fails
  the checks if firmware code installs a sink, enables machine control, or adds a
  raw transmit site outside the two read-only links.
* **Dry run.** The Dry Run page (Settings > Machine > Controller Capabilities >
  Dry Run) shows the generated command, the real gate's decision, the expected
  reply and the timeout / error / link-loss handling. It calls
  `JcDispatcher::evaluate()`, which records, counts and transmits nothing.

## Instruction 006 additions (first-live-jog preparation; nothing flashed or sent)

Full report: [jc-live-jog-readiness.md](jc-live-jog-readiness.md). Summary of the safety design:

* **Two builds.** The ordinary firmware is unchanged (`JC_MACHINE_CONTROL_ENABLED=0`, no
  sink). The explicitly named `env:jc8048w550_livejog_test` is the only place either macro
  is set, and its image has never been flashed. `tools/check_no_sink.py` fails if the macros
  appear anywhere else, if the sink or the jog transmit function is referenced outside a
  `#if JC_LIVE_JOG_TEST` block, if a second raw `::send` appears, or if the strict-line check
  is missing before the live send.
* **Class allowlist.** In the live-jog build the gate authorizes only `Jog` and `JogCancel`.
  Home, probe, offsets, spindle, jobs, macros, unlock, reset, feed hold and unclassified
  commands are `BlockedBuildPolicy` even when armed, confirmed and in Idle.
* **Three independent checks of the same jog rule**: the builder (`TestLimit`, `JogDisabled`),
  the transport (`jc_jog_line_allowed`, a separate strict parser) and, in the simulator, a
  Python copy of the rule. Test limits: X/Y 1 mm @ 100 mm/min only; Z is on hold (Instruction
  006A) and A is disabled.
* **Stopping is never blocked by bookkeeping.** `JogCancel` (and `Hold`, where compiled in)
  need only LIVE mode: not arming, not a fresh link, not a recent command, not a free executor.
* **Unresolved operations block new motion** (`BlockedOperationPending`): an in-flight jog,
  an unacknowledged failure, a configuration read in progress.
* **Envelope / gate (Instruction 006A)**: per axis, PROTECTED (unreachable: no valid-position
  indicator can be read), UNPROTECTED MICRO-JOG, or BLOCKED. Anything UNKNOWN, refused, stale
  (older than 5 minutes), not read, or an excluded axis (Z on hold, A disabled) is BLOCKED and is
  never treated as OFF. `BlockedEnvelopeUnread` covers those; `BlockedUnprotectedNotAcknowledged`
  requires a deliberate per-axis operator acknowledgement, valid for one arming only and lost on
  any link loss. A configured travel, MPos, `must_home` and a configured homing cycle are never
  treated as a safe path, a valid position or proof of homing. A configured limit input is not
  hard-limit protection. Full detail: `jc-live-jog-readiness.md` section 4a.
* **Arming** lapses after 120 s, on leaving the jog page, in DEMO and on any loss of fresh data.
* **Late start** of a jog after a cancel is detected, cancelled again and latched; motion by
  another client is counted and never cancelled by this pendant.
* **Motion completion** is judged from an Idle status frame newer than the acknowledgement and
  from the reported position, never from the `ok` alone.

### Outstanding safety requirements before any motion is enabled

* A separate safety review and explicit owner approval (unchanged).
* Real-controller verification of the reply formats the parser assumes (`$I`,
  `$G`, `$/axes/...` echoes, error numbers); the simulator uses the same
  assumptions, so agreement between them proves nothing about FluidNC.
* A controller-side soft-limit / hard-limit check: FluidNC's `max_travel_mm` is
  read, but whether soft limits are enabled is not, and the jog step limit equals
  the full travel.
* Rotary-axis semantics (degrees, wrap-around, feed units on the A axis) cannot
  be read from FluidNC and remain operator-declared.
* Sequence execution and probe-result handling (`[PRB:...]`) are specified and
  host-tested but have never run against a controller.
* An independent stop path (physical e-stop) remains the only emergency stop.
