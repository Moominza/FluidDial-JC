# JC first-live-jog readiness report (Instruction 006)

Status: **experimental. Nothing was flashed that can move the machine, and no jog or
cancel byte was transmitted to any controller during this work.**
Baseline `3e29251`, branch `feature/jc8048w550`. Hardware: Guition JC8048W550C_I (COM3).
Controller: MKS DLC32 MAX V1.2, FluidNC v4.1.1, `192.168.0.134:23`.

The device on COM3 runs the **ordinary** firmware (`env:jc8048w550`,
`JC_MACHINE_CONTROL_ENABLED=0`). The live-jog build exists only as a compiled image
(`env:jc8048w550_livejog_test`) and has not been flashed anywhere.

This report separates what was **measured on the real controller**, what was
**simulated** and what is an **unverified assumption**.

## 1. Build configurations

| | Ordinary firmware | Experimental live-jog test build |
| --- | --- | --- |
| PlatformIO env | `jc8048w550` (unchanged) | `jc8048w550_livejog_test` (never a default env) |
| `JC_MACHINE_CONTROL_ENABLED` | **0** | 1 |
| `JC_LIVE_JOG_TEST` | 0 | 1 |
| Command sink | none | `JcJogSession::sink`, installed in one place (`JcApp.cpp`) |
| Bytes it can transmit | `?` and the read-only queries | the same, plus one restricted `$J=` line and the jog-cancel byte `0x85` |
| Classes the gate can authorize | none | `Jog` and `JogCancel` only. Home, probe, work offset, spindle, job, macro, unlock, reset, hold and unclassified commands return `BlockedBuildPolicy` |
| Continuous jog | never generated | never generated (builder refuses it in test mode) |
| Transport | Wi-Fi (`?`, queries) | Wi-Fi only; the wired UART link has no jog path |
| Boot / UI text | "motion commands disabled" | "LIVE JOG TEST BUILD" on boot, menu and settings; the "disabled" claim is removed |

Evidence that the ordinary image cannot jog: the static guard (section 6), and a scan of
the compiled binaries. The ordinary `firmware.bin` has none of `jogtest: SENT`,
`jogtest: ARMED`, `LIVE JOG TEST BUILD` or the transport's `not an allowed live-jog test
line`; the experimental image has all four. (Both contain the preview formatter string
`$J=G91G21F`, which is expected: previews are generated but not sent.)

## 2. Actual FluidNC configuration (real controller)

### 2.1 Axis limits and rates (measured, Instruction 005, operator photo 2026-10-30)

| Axis | Steps/mm | Max travel (mm) | Max rate (mm/min) | Accel (mm/s²) | Homing cycle |
| --- | --- | --- | --- | --- | --- |
| X | 40 | 1000 | 6000 | 250 | 1 |
| Y | 40 | 2300 | 6000 | 250 | 1 |
| Z | 200 | 150 | 1500 | 100 | 1 |
| A | 1000 | 3000 | 1000 | 25 | refused (`error:3`) |

### 2.2 Soft limits, hard limits, limit pins, homing position, must_home

**PENDING: not yet read on real hardware.** The firmware now reads these (53 read-only
queries instead of 28) and shows them on Settings > Machine > Controller Capabilities >
LIMITS / HOMING. The section is completed from the serial capture and the operator's
photographs once the operator has tapped READ CONTROLLER.

The setting paths queried are:

| Item | Path |
| --- | --- |
| Soft limits | `$/axes/<x|y|z|a>/soft_limits` |
| Hard limits | `$/axes/<a>/motor0/hard_limits` |
| Limit pins | `$/axes/<a>/motor0/limit_neg_pin`, `limit_pos_pin`, `limit_all_pin` |
| Homing | `$/axes/<a>/homing/cycle`, `positive_direction`, `mpos_mm` |
| Must home at start | `$/start/must_home` |

These paths come from general knowledge of FluidNC's configuration tree, not from a source
in this repository, and are **assumptions until a real read confirms them**. A path the controller does not recognise returns `error:3` and
is shown as `error:3`; it is never shown as OFF. "UNKNOWN" means not read, refused or
unreadable.

## 3. Jog restrictions (test mode)

Proposed conservative test limits, **not values established as safe for every set-up**:

| Axis | Max increment | Max feed | Notes |
| --- | --- | --- | --- |
| X | 1 mm | 100 mm/min | |
| Y | 1 mm | 100 mm/min | |
| Z | 0.5 mm | 50 mm/min | |
| A | disabled | n/a | pending rotary calibration; refused by the builder and by the transport |

* The limits are applied to the machine profile **after** the controller's values, so a
  controller travel of 2300 mm can only ever lower the allowed step, never widen it.
* A larger step is refused even when the UI offers the 10 / 50 / 100 mm presets. The
  presets stay selectable (dimmed) so the refusal is visible: "larger than the live-jog
  test limit".
* The transport re-validates the exact line (`jc_jog_line_allowed`) against the same
  limits with an independent parser, so a bug in the builder cannot widen what leaves the
  device. The simulator audits it a third time, in Python.
* Worst-case motion from one test jog: 1 mm at 100 mm/min takes 0.6 s plus acceleration.

## 4. Operation model

One jog at a time. Acknowledgement, motion and completion are separate states.

| Phase | Meaning | Left by |
| --- | --- | --- |
| Idle | nothing outstanding | a jog is sent |
| Awaiting ok | line sent, no reply yet | `ok`, `error:N`, `ALARM:N`, or the 2 s timeout |
| Accepted | `ok` received; the jog is accepted, **not finished** | a post-acknowledgement status frame |
| Moving | status shows Jog / Run after the acknowledgement | status Idle |
| Cancelling | `0x85` sent; waiting for the controller to report Idle | a newer status frame showing Idle |
| Recovering | connection lost or status stale | a fresh Idle frame after a 1 s quiet period |

| Result | Latched until acknowledged? |
| --- | --- |
| Completed (position matches) | no |
| Rejected by controller (`error:N`, nothing started) | no |
| Cancelled by the operator | no |
| Alarm, no reply (timeout), motion timeout, stale status, connection lost, send failed, cancel not confirmed, position mismatch, unexpected motion | **yes** |

* "Completed" needs an Idle frame **newer than the acknowledgement**. The reported position
  must have moved by the commanded distance (tolerance 0.05 mm). For a step so small that
  zero movement would also match, observed motion or a second Idle frame is required.
* New jogs are refused while any phase other than Idle is active, while a failure is
  unacknowledged, within 400 ms of the previous start or 300 ms of the previous result
  (touch bounce), while the machine is not Idle, while status is stale, and while the
  executor's resync quiet period (1 s after a timeout / cancel) is running. The gate refuses
  them too (`BlockedOperationPending`), and a configuration read in flight counts as pending.
* **Cancel** is the real-time byte `0x85` (`JogCancel`, confirmed in the GrblParser header
  the project builds against). It is attempted in every phase, never needs arming,
  confirmation, a fresh link or a free executor, and can be sent again. It only requires
  LIVE mode. If the controller is not seen Idle within 3 s the result is "CANCEL NOT
  CONFIRMED - USE E-STOP".
* **Late start**: if the cancel overtook the queued `$J` line and the jog starts after the
  session believes it is over, a `Jog` state appearing within 5 s of the last jog is treated
  as our own jog: it is cancelled again and latched. Later than that it is counted as foreign
  motion (another client) and never cancelled by this pendant.
* **No replay.** The session holds no queue and no copy of a sent line. A timeout, loss or
  failure never resends. After a reconnect nothing is transmitted until the operator
  re-arms, acknowledges the failure, status is fresh and Idle, and confirms the new jog.
* **Arming** lapses after 120 s, on leaving the jog page, in DEMO mode and on any loss of
  fresh data. Every test jog also needs its own confirmation dialog.
* **Envelope**: a configured travel is never treated as a safe movement envelope. A jog is
  refused until the soft / hard limit settings were read (`BlockedEnvelopeUnread`) and the
  confirmation dialog shows soft limits, hard limits, limit pins, homing and `must_home` as
  the controller reported them, plus "Travel N mm is NOT proof the path is clear". The
  operator still has to look.

## 5. Requirement to evidence map

| Instruction 006 item | Mechanism | Evidence |
| --- | --- | --- |
| 1 baseline preserved | ordinary env unchanged and still the default | HEAD `3e29251` before work; guard; image scan |
| 2, 3 jog restrictions, larger steps rejected | `jc_profile_apply_test_limits`, builder `TestLimit` / `JogDisabled`, transport check | host tests, simulator, guard |
| 4 limit / homing configuration | 25 added read-only queries, LIMITS / HOMING page | host tests + simulator; **real read pending** |
| 5 travel is not an envelope | `BlockedEnvelopeUnread`, dialog text | host tests |
| 6 ack, error, alarm, timeout, completion distinct | `JcJogSession` phases and results | host tests + simulator |
| 7 cancellation | `0x85`, always attempted | host tests + simulator, incl. late start and ignored cancel |
| 8 reject while unresolved | session + gate | host tests + simulator audit (independent) |
| 9 no replay | no queue; re-arm + acknowledge | host tests + simulator |
| 10 other features disabled | class allowlist + builder + transport | host tests (all 10 other classes) |
| 11 position and state after completion | position check, stale / mismatch results | host tests + simulator; **real hardware pending** |
| 12 test coverage | see section 6 | |
| 13 three targets built | see section 7 | |
| 14 explicit experimental config | `env:jc8048w550_livejog_test` | built, not flashed |
| 15 ordinary `JC_MACHINE_CONTROL_ENABLED=0` | unchanged | guard + image scan + boot log |
| 16 no motion flash / transmit | only `env:jc8048w550` was uploaded | upload log, boot log |

## 6. Test results

All run on the host. The "controller" is a fake or the Python simulator; none of it is
FluidNC.

| Suite | Result |
| --- | --- |
| Core (policy 0 / rules compiled in) | 111 / 125 pass |
| App | 169 / 166 pass |
| Control (replies, exec, loader, dispatcher, dry run) | 832 / 888 pass |
| **Jog** (policy 0 / rules in / live-jog policy) | **587 / 595 / 641 pass** |
| Static guard (`tools/check_no_sink.py`) | PASS |
| Simulator integration (`tools/run_sim_integration.py`) | PASS, 225 checks (was 119) |

Jog test coverage (item 12): first jog; acceptance and rejection (`error:15`); cancellation
while moving, before the `ok`, with nothing in flight, with a failing transport, and when the
controller ignores it; connection loss while awaiting, moving and idle; stale status with the
socket open; alarm as the reply, mid-move, as a stray line, and while idle; timeouts (no
reply, motion overrun); late-start and foreign motion; touch bounce (a burst of six presses
transmits one); oversized increments including the 10 / 50 / 100 mm presets; rotary-axis
blocking; position mismatch and unavailable position; send failure; reconnect without replay.

Checks that the tests can fail:

* Jog unit tests: 19 deliberate faults injected into the session, gate and builder. **18 were
  caught.** The one survivor is an equivalent mutant (an explicit zero-limit check that the
  following magnitude check makes redundant). One earlier survivor revealed a real gap
  (the late-start case) that is now handled and tested.
* Static guard: 13 deliberate violations (control enabled in the wrong env, flag in another
  env, sink installed outside a live block, an extra `::send`, a missing strict-line check,
  a jog path in the wired link, ...). **All 13 caught.**
* Simulator integration: two faults injected into the C++ session. **Both caught.**

The simulator's audit is an independent Python re-implementation of the jog rule. In the
`jog-*` scenarios it accepts only strictly formatted in-limit jogs and `0x85`, and flags
everything else, including a jog line sent while a previous jog is unresolved.

## 7. Build results

| Target | Result | Flash | RAM |
| --- | --- | --- | --- |
| `jc8048w550` (ordinary) | SUCCESS | 1,034,293 B (30.9%) | 94,904 B (29.0%) |
| `jc8048w550_livejog_test` (experimental, **not flashed**) | SUCCESS | 1,042,061 B (31.2%) | 94,912 B (29.0%) |
| `m5dial` | SUCCESS | 1,262,241 B (unchanged) | |
| `cyddial` | SUCCESS | 1,185,829 B (unchanged) | |

The only compiler warnings are two pre-existing narrowing warnings in `JcScreenOffsets.cpp`,
a file this work did not touch. The ordinary firmware was uploaded to COM3 (hashes verified)
and boots with "motion commands disabled".

## 8. Assumptions not verified on a real controller

None of these has been exercised on FluidNC, because no jog or cancel was sent:

1. `$J=G91G21F<feed><axis><dist>` is accepted by FluidNC v4.1.1 in exactly this form.
2. The controller answers `ok` when the jog is **accepted**, and the status then shows `Jog`
   until the move ends. If it instead answers when the move finishes, the "Accepted" and
   "Moving" phases collapse into one and completion detection still holds, but this is
   unconfirmed.
3. `error:N` / `ALARM:N` numbers and texts for a rejected or limit-violating jog.
4. `0x85` stops a running jog over Telnet (port 23) without side effects, and does not
   break a pending `$J` line (the late-start case).
5. The controller keeps answering `?` while a jog runs. The firmware polls once a second; a
   slower or paused status stream while moving is handled (stale: cancel attempted) but unseen.
6. The setting paths in section 2.2.
7. Whether soft limits are enforced when the machine is not homed (this pendant does not
   assume they are).
8. The A axis is rotary, in degrees. FluidNC cannot report this; it stays operator-declared
   and A jogging is disabled.

## 9. Remaining prerequisites before a real jog

1. **Owner approval, in writing, to flash the live-jog build and to send the first jog**, and
   a separate safety review of this design. Not given.
2. **Complete the real configuration read** (section 2.2) and review soft limits, hard limits,
   limit pins and homing for X, Y and Z with the owner.
3. **Decide the machine state for the test**: homed or not, `must_home`, soft limits on or off,
   and where the carriage must be so that a 1 mm move in either direction cannot reach an
   end stop, a clamp or the workpiece.
4. **A physical emergency stop within reach**, tested, and a clear workspace. The software
   cancel is not an emergency stop.
5. **Verify the assumptions in section 8** on the real controller without motion where
   possible (for example `$J=` rejection behaviour is unknown until a first jog is sent).
6. **A written test procedure** with abort criteria, agreed by the owner (a draft is in section 10).
7. **Decide the A axis**: rotary calibration (degrees per step, direction, wrap-around) and
   feed units. Until then A stays disabled.
8. **Operator visual check** of the Jog, Capabilities and Limits / Homing pages in the live-jog
   build (laid out without a photo review so far) and of the arming and confirmation dialogs.
9. **Review the experimental flash procedure**: how to return to the ordinary firmware
   afterwards (flash `env:jc8048w550`) and how to confirm it from the boot log.
10. **Wi-Fi behaviour during motion**: loss and stale status are handled in the software
    (cancel attempted, failure latched, nothing replayed) and simulated, but how the real link
    and controller behave when the connection drops mid-jog has never been observed.
11. **Separate approval for anything beyond a single restricted jog**: continuous jogging,
    larger steps, homing, probing, macros, spindle, files, the A axis. None is implemented
    for transmission.
12. No merge into `main` and no release without separate approval.

## 10. DRAFT first-jog procedure (not approved, not executed)

This is a starting point for the owner's review. It must not be followed until section 9 is
complete and the owner has approved it.

1. Machine powered, E-stop within reach, workspace clear, nothing mounted that a 1 mm move
   could damage. Carriage position noted.
2. With the **ordinary** firmware: connect LIVE, wait for Idle, read the controller
   configuration, and review LIMITS / HOMING with the owner.
3. Flash `env:jc8048w550_livejog_test`. Confirm the boot log says "LIVE JOG TEST BUILD" and the
   Settings page shows the red banner.
4. Connect LIVE, wait for Idle with fresh status, read the controller configuration again
   (values are discarded on every connection loss and on reboot).
5. Open Jogging, select **Z, 0.5 mm, 50 mm/min**, tap ARM TEST and confirm.
6. Jog +0.5 mm. Read the confirmation (soft / hard limits, "Travel is not proof"), confirm.
7. Expected: "sent", then "ACCEPTED ... not finished", then "COMPLETED: moved +0.500". The DRO
   changes by 0.500 mm and the machine is physically seen to move that amount.
8. **Abort at once (E-stop, then CANCEL JOG)** on: any unexpected direction or distance, any
   alarm, "NO REPLY", "POSITION MISMATCH", "CANCEL NOT CONFIRMED", "UNEXPECTED MOTION", a
   frozen DRO, or any doubt.
9. Repeat -0.5 mm, then X and Y at 1 mm / 100 mm/min only after the owner agrees.
10. Test CANCEL JOG once on a deliberately longer-running move only if the owner approves
    raising the test limit for that purpose; otherwise cancellation stays host-tested.
11. Reflash `env:jc8048w550` and confirm "motion commands disabled" in the boot log.

**Stop point:** real motion is not enabled and this report does not request it.
