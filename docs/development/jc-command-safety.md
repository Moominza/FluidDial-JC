# JC command safety design

Status: **experimental**. Machine control is disabled in the shipping firmware.

## Layers

| Layer | File | Role |
| --- | --- | --- |
| Status parsing | `src/jc8048w550/JcStatusFrame.hpp` | Pure parser used by firmware and host tests. Accepts only a known state, exactly four finite MPos/WPos values and an FS pair. Also extracts WCO, feed, spindle and `Pn` pins. `jc_convert_position()` refuses to convert between machine and work coordinates when no WCO is known. |
| Command generation | `src/jc8048w550/JcCommandBuilder.hpp` | Pure formatting and validation of jog, cancel, hold/resume, reset, unlock, home, WCS select, axis zero/set, probe move and Z probe sequence, SD file run, macro run and spindle stop. Formats mirror the pinned upstream FluidDial scenes. Nothing is transmitted. |
| Authorization | `src/jc8048w550/JcCommandAuth.hpp` | `JcCommandGate` is the single submission point for every machine-changing command. |
| Transport | `JcReadOnlyFluidNc` | Only `?` can leave the device (`parser_putchar()` blocks and counts everything else). The gate has no transmit sink. |

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
  link, the UART link and the host tests.
* The software cancel / STOP controls are not emergency stops and are
  labelled as such on the jogging page and in the job-cancel dialog.
