# JC first-live-jog readiness report (Instructions 006 to 009A)

Status: **experimental. Nothing was flashed that can move the machine, and no jog or
cancel byte was transmitted to any controller during this work.**
Branch `feature/jc8048w550`. Hardware: Guition JC8048W550C_I (COM3).
Controller: MKS DLC32 MAX V1.2, FluidNC v4.1.1, `192.168.0.134:23`.

## Current state (after Instruction 006A)

| | |
| --- | --- |
| Instruction 006 read-only capability discovery | **COMPLETE.** Accepted by the owner. |
| Real controller read | 50 answered / 3 refused, **identical on three reads** |
| Travel protection | **NONE ACTIVE** (soft limits OFF, hard limits OFF on every axis) |
| Machine-position validity | **NOT ESTABLISHED** (`must_home` is false and nothing read says the machine is homed) |
| First motion policy | **UNPROTECTED MICRO-JOG TEST MODE**: X and Y only, 1 mm maximum, 100 mm/min maximum, one bounded jog at a time, operator physical-clearance acknowledgement required |
| Z | **HOLD**: homing direction / limit-side verification required (section 2.4). No Z jog, no Z homing test. |
| A | **DISABLED** |
| Experimental build | built and tested, **NOT flashed**. Owner review is required before it is flashed. |
| First real motion | **NOT AUTHORIZED** |
| Independent emergency-stop system | **REQUIRED PHYSICAL PREREQUISITE. NOT installed, NOT tested, NOT designed by this software.** Nothing in this repository represents it as present. |
| Interface verification (006B / 006C) | Host renderings of the real page code checked for clipping and overlap; 6 defects found and fixed (section 6b). **Operator photographs of the experimental build are still outstanding.** |
| Automatic reconnection (006C.1) | Implemented in the ordinary firmware; two hardware resets reconnected by themselves. Wi-Fi-loss recovery on hardware pending. See [jc-auto-connect.md](jc-auto-connect.md). |

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
| Classes the gate can authorize | none | `Jog` and `JogCancel` only, and `Jog` only for X and Y. Home, probe, work offset, spindle, job, macro, unlock, reset, hold and unclassified commands return `BlockedBuildPolicy` |
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

**READ ON THE REAL CONTROLLER, 2026-10-30.** The ordinary read-only firmware (53 queries)
was run LIVE against FluidNC v4.1.1 three times from Settings > Machine > Controller
Capabilities. Every read gave the same result: **50 answered, 3 refused** (2285, 1955 and
2261 ms). The third read is the one the operator photographed on the LIMITS / HOMING page
(08:37); the photograph matches the serial log. Evidence:
[evidence/2026-10-30-controller-read-capture.md](evidence/2026-10-30-controller-read-capture.md).

**What the evidence is.** The passive COM3 capture holds the firmware's own log of the
parsed result of each read. The firmware does not log every raw query / reply line, so this
is not a byte-level transcript of the 53-query exchange. Nothing but `?` and read-only
queries was transmitted: the capture has no `jogtest:`, `SAFETY:` or dispatch line.

| Axis | Soft limit | Hard limit | Limit pin: neg / pos / all | Homing cycle | Homing direction | Home MPOS |
| --- | --- | --- | --- | --- | --- | --- |
| X | **OFF** | **OFF** | `gpio.41:low` / unassigned / unassigned | 1 | negative | 0 |
| Y | **OFF** | **OFF** | `gpio.40:low` / unassigned / unassigned | 1 | negative | 0 |
| Z | **OFF** | **OFF** | `gpio.39:low` / unassigned / unassigned | 1 | **positive** | 0 |
| A | **OFF** | **OFF** | unassigned / unassigned / unassigned | `error:3` (refused) | `error:3` (refused) | `error:3` (refused) |

| Item | Value |
| --- | --- |
| `$/start/must_home` | **false** (the controller does not refuse motion until homed) |
| Probe pin / toolsetter pin | `gpio.48:low` / `NO_PIN` |
| Firmware, modal state | `4.1 FluidNC v4.1.1 (esp32s3-wifi)`, G54, T0 |

How to read it:

* The three refused queries are exactly A's `homing/cycle`, `homing/positive_direction` and
  `homing/mpos_mm` (`error:3`). They stay **refused / unknown** and are never shown as OFF or
  "none". This replaces the earlier inference in the Instruction 005 report.
* The OFF values for X, Y, Z **and A** are real answers (`false`), not gaps. For A the three
  limit pins were also answered (`NO_PIN`), so they are "not assigned", not "unavailable".
* All 12 other items per axis were answered, so the setting paths in the table below are
  **confirmed on this controller** for X, Y, Z and A (A's homing items excepted).
* A homing cycle of 1 on X, Y, Z only says homing is *configured*. It does **not** say the
  machine is currently homed, and nothing read here can say that.

| Item | Path (confirmed answered unless noted) |
| --- | --- |
| Soft limits | `$/axes/<x|y|z|a>/soft_limits` |
| Hard limits | `$/axes/<a>/motor0/hard_limits` |
| Limit pins | `$/axes/<a>/motor0/limit_neg_pin`, `limit_pos_pin`, `limit_all_pin` |
| Homing | `$/axes/<a>/homing/cycle`, `positive_direction`, `mpos_mm` (refused for A) |
| Must home at start | `$/start/must_home` |

### 2.3 What this means for a first jog (findings, not approval)

**No motion test is authorized by this read.**

1. **The controller enforces no travel limit on any axis.** Soft limits are OFF and hard limits
   are OFF on X, Y, Z and A. Nothing in the controller will stop a move at the end of travel, so
   the configured travel (X 1000, Y 2300, Z 150 mm) says nothing about where the carriage can
   safely go. Only the 1 mm / 0.5 mm test step limits and the operator's own eyes bound a jog.
2. **The machine position may not be valid.** `must_home` is false and homing was not read as
   done, so the reported position may be a power-up zero rather than a real machine position.
   Soft limits would be meaningless in that state even if they were on.
3. **The limit switches only act during homing.** With hard limits OFF, the single "negative"
   switch on each of X, Y, Z (pins 41, 40, 39) is used by the homing cycle, not as a travel guard.
4. **Z homes in the positive direction but only a negative-side limit pin is assigned.** This
   may be a deliberate wiring / configuration choice. It is recorded here for the owner to check;
   this report draws no conclusion from it.
5. **A has no homing configuration and no limit pins** (as read), and stays disabled.

**Resolved by Instruction 006A** (section 4a): the gate now represents this case explicitly as
UNPROTECTED MICRO-JOG TEST MODE and requires a deliberate operator acknowledgement of exactly
these facts before any jog.

### 2.4 Accepted real-controller baseline (owner-accepted, Instruction 006A)

| | Soft | Hard | Limit pin | Homing cycle | Homing direction | Home MPOS |
| --- | --- | --- | --- | --- | --- | --- |
| X | OFF | OFF | negative: `gpio.41:low` | 1 | negative | 0 |
| Y | OFF | OFF | negative: `gpio.40:low` | 1 | negative | 0 |
| Z | OFF | OFF | negative: `gpio.39:low` | 1 | positive | 0 |
| A | OFF | OFF | unassigned | UNKNOWN (`error:3`) | UNKNOWN (`error:3`) | UNKNOWN (`error:3`) |

`start.must_home = false`. `error:3` is UNKNOWN and is never reinterpreted as OFF, false, zero
or unsupported.

Interpretation fixed by the owner and implemented in the gate:

* `max_travel_mm` is **not** a safe remaining path. MPos is **not** a trustworthy position
  reference after boot just because it reports numbers. A configured homing cycle is **not**
  proof that homing has occurred.
* With `hard_limits` OFF, a configured limit **input** is not protection against an ordinary
  jog (it may still be used by homing). The jog screen words these separately: LIMIT INPUT
  CONFIGURED versus HARD LIMIT PROTECTION OFF.
* `start.must_home` is **not** used as a jog gate. The owner states that FluidNC's
  documentation says it governs ordinary motion acceptance while jogging and settings can still
  operate unhomed; this project has not checked that against FluidNC's source. Either way the
  application-level jog gate does not depend on it.

**Z HOMING CONFIGURATION: OWNER / PHYSICAL VERIFICATION REQUIRED.** The controller reports Z
homing direction positive with only a negative-side limit input (`gpio.39:low`) configured.
FluidNC defines `positive_direction=true` as homing toward the positive end and `limit_neg_pin`
as the negative-end switch. This combination needs physical and configuration verification:
whether GPIO39 is the negative-end switch, a positive-end switch labelled incorrectly, a
shared / all-limit input, a wiring or configuration mismatch, or another deliberate
arrangement; and which Z homing direction is intended. This report does not conclude it is
wrong, does not change the FluidNC configuration, and does not run `$H` / `$HZ`. Z stays on hold.

## 3. Jog restrictions (test mode)

Proposed conservative test limits, **not values established as safe for every set-up**:

| Axis | Max increment | Max feed | Notes |
| --- | --- | --- | --- |
| X | 1 mm | 100 mm/min | first live motion |
| Y | 1 mm | 100 mm/min | after X, only if the X test passed |
| Z | **none (HOLD)** | none | removed by Instruction 006A (was 0.5 mm / 50 mm/min). Refused by the builder and by the transport at every size and feed. |
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
* **Envelope**: see the gate in section 4a. A configured travel is never treated as a safe
  movement envelope and nothing on screen claims one.

## 4a. The jog gate (Instruction 006A)

Computed per axis by `jc_jog_gate()` (pure, `JcJogTest.hpp`), from the capability read of the
**current** LIVE session.

| Mode | Meaning | On this controller |
| --- | --- | --- |
| **PROTECTED** | travel protection AND a valid position are both established | **Not reachable.** Nothing the firmware can read establishes that the machine is homed, so position trust is never "established". The branch exists so the policy has one deliberate place to change. |
| **UNPROTECTED MICRO-JOG** | every needed value is known and current, but there is no trusted travel protection | X and Y |
| **BLOCKED** | anything unknown, refused, stale or not read, or an excluded axis | Z (on hold), A (disabled), and any axis whose values are not all known |

An axis is BLOCKED when: it is excluded (Z, A); there is no completed read in this session; the
read is older than 5 minutes; the soft-limit state, the hard-limit state or any of the three
limit-pin settings is not known (refused, unreadable, not queried). **UNKNOWN is never treated
as OFF.** `error:3` on A's homing items does not affect X or Y.

For UNPROTECTED MICRO-JOG the gate additionally requires, for the selected axis:

1. **A deliberate operator acknowledgement** of the warning below, recorded per axis for
   this arming only (`BlockedUnprotectedNotAcknowledged` otherwise). It is cleared by a disarm,
   by arming lapsing (120 s), by leaving the jog page and by any loss of the connection. It is
   never restored after a reconnect. The acknowledgement authorizes only the tightly bounded
   test move; the text does not call the machine safe.
2. A confirmation of every individual jog, with a shorter version of the same warning.
3. The existing conditions: LIVE, fresh Idle status, no unresolved operation, X/Y 1 mm /
   100 mm/min maximum through the three-layer check.

Warning shown before arming (values filled from the real read):

> NO CONTROLLER TRAVEL PROTECTION
> X: soft limits OFF, hard limits OFF.
> Machine position may not match the physical position.
> Limit switches will not stop an ordinary jog.
> Check: machine still, Idle, spindle cannot start, no job.
> Check clear space in the commanded direction.
> Keep E-stop / power removal in reach. Not 'safe': 1 mm only.

Reconnect: the link loss discards the capability read and the arming. A new read, a new arming
and a new acknowledgement are required. Nothing is replayed.

Not used for safety, on purpose: `max_travel_mm`, MPos, home MPOS, `must_home`, and a configured
homing cycle. They may be displayed informationally; the gate never turns them into "safe path",
"within travel" or "enough remaining travel". `jc_position_trust()` is always Untrusted and
`jc_path_claim()` is always None, and the tests show that no combination of `must_home`, homing
cycle or travel changes that.

**Cancel is independent of all of this.** `JogCancel` needs only LIVE mode: not arming, not the
acknowledgement, not a current capability read, not a fresh link, not a free executor.

## 5. Requirement to evidence map

| Instruction 006 item | Mechanism | Evidence |
| --- | --- | --- |
| 1 baseline preserved | ordinary env unchanged and still the default | HEAD `3e29251` before work; guard; image scan |
| 2, 3 jog restrictions, larger steps rejected | `jc_profile_apply_test_limits`, builder `TestLimit` / `JogDisabled`, transport check | host tests, simulator, guard |
| 4 limit / homing configuration | 25 added read-only queries, LIMITS / HOMING page | host tests + simulator; **real read done 2026-10-30** (section 2.2) |
| 5 travel is not an envelope | gate modes (section 4a), `BlockedEnvelopeUnread`, `BlockedUnprotectedNotAcknowledged`, dialog text | host tests + simulator |
| 006A gate modes, UNKNOWN blocked, stale read blocked, reconnect clears, Z excluded | `jc_jog_gate`, `jc_jog_gate_masks`, dispatcher acknowledgement mask | host tests (mutation-checked), simulator |
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
| **Jog** (policy 0 / rules in / live-jog policy) | **1267 / 1274 / 1354 pass** (587 / 595 / 641 before 006A) |
| Connection policy (`test_jc_conn.cpp`, both policies) | 228 / 228 pass |
| Host UI renderer (`tools/run_ui_preview.py`, ordinary + live-jog builds) | PASS: 35 frames, 0 layout problems |
| Static guard (`tools/check_no_sink.py`) | PASS |
| Simulator integration (`tools/run_sim_integration.py`) | PASS, 260 checks (225 at 006A) |

Instruction 006A coverage, all in `test/host/test_jc_jog.cpp` against a fixture that mirrors the
real controller baseline: soft OFF + hard OFF + current read gives UNPROTECTED MICRO-JOG for X and
Y; UNKNOWN soft, hard or any limit pin (refused, unreadable, not queried) blocks; not read,
incomplete and stale (exactly at the limit passes, one ms over blocks) block; Z and A are
excluded; Protected needs both trusted protection and an established position; `must_home`
true / false / unknown, homing cycle and travel never change position trust or produce a path
claim, and never change the mode; a configured limit input with hard limits OFF is worded as
input-configured / protection-OFF, an unknown hard limit is never worded OFF; no gate wording
claims a safe path; unacknowledged unprotected jogs are blocked, per axis; the acknowledgement,
the arming and the capability read all die on link loss and are not restored; cancel stays
available with no arming, no acknowledgement, no current read, a stale link or a pending
operation; the 1 mm X/Y cap, the 10 / 50 / 100 mm presets, Z and A are refused at the builder,
at the transport and by the simulator's independent audit.

Jog test coverage (item 12): first jog; acceptance and rejection (`error:15`); cancellation
while moving, before the `ok`, with nothing in flight, with a failing transport, and when the
controller ignores it; connection loss while awaiting, moving and idle; stale status with the
socket open; alarm as the reply, mid-move, as a stray line, and while idle; timeouts (no
reply, motion overrun); late-start and foreign motion; touch bounce (a burst of six presses
transmits one); oversized increments including the 10 / 50 / 100 mm presets; rotary-axis
blocking; position mismatch and unavailable position; send failure; reconnect without replay.

Checks that the tests can fail:

* Jog unit tests: 35 deliberate faults injected into the session, gate, dispatcher and
  builder (including 16 aimed at the 006A gate: UNKNOWN treated as OK, stale read accepted,
  `must_home=false` taken as a valid position, Z re-enabled, the acknowledgement surviving a
  disarm, a safe-path claim without a valid position, ...). **34 were caught.** The one survivor
  is an equivalent mutant (an explicit zero-limit check that the following magnitude check makes
  redundant). An earlier survivor revealed a real gap (the late-start case), now handled.
* Static guard: 13 deliberate violations (control enabled in the wrong env, flag in another
  env, sink installed outside a live block, an extra `::send`, a missing strict-line check,
  a jog path in the wired link, ...). **All 13 caught.**
* Simulator integration: two faults injected into the C++ session. **Both caught.**

The simulator's audit is an independent Python re-implementation of the jog rule. In the
`jog-*` scenarios it accepts only strictly formatted in-limit jogs and `0x85`, and flags
everything else, including a jog line sent while a previous jog is unresolved.

## 6b. Interface verification (Instructions 006B / 006C)

`tools/run_ui_preview.py` compiles the **real page code and fonts** against a host stub of the display,
drives the pages with simulated touches and the accepted real-controller baseline (replayed through the
real capability loader), writes PNG screenshots ([ui-preview/](ui-preview/)) and checks every frame for
text outside the screen, text drawn over other text, text wiped out without being redrawn, and, for
dialogs, text leaving the box or touching a button. Its detector is self-tested (it must flag overlap,
off-screen text and a fill over text) and is part of `tools/run_host_tests.py`.

**What this proves:** layout, wording and control state of the pages. **What it cannot:** the colours and
brightness of the real panel, touch response, or anything about the controller. These are *renderings of
the code*; the operator's photographs of the experimental build remain the real visual check and are
still outstanding.

Checked in both builds, 35 frames: menu, settings, jog page (unread, ready, armed, accepted, moving,
completed, Z hold, A disabled, 10 mm preset refused), arming dialog, per-jog confirmation, "jog sent",
failure latched, cancel unconfirmed, no communication, "cancel not sent", Limits and Capabilities pages.

The arming dialog (seven lines) has no clipping and nothing touches its buttons. The dialog's true
capacity is seven lines (an eighth would touch the buttons); `kMaxLines` is now 7. The distinctions
requested are all on screen: **UNPROTECTED MICRO-JOG** (the gate line and the arming dialog),
**LIMIT INPUT CONFIGURED** and **HARD LIMIT PROTECTION OFF** (per-jog dialog and the Limits page),
**Z ON HOLD** (jog buttons, arming refusal, dialog) and **A DISABLED**. The banner **LIVE JOG TEST
BUILD** is in every page header (shortened to "JOG TEST BUILD" beside the longest titles) and on the
dashboard header; the menu, dashboard, connection page and serial log no longer claim motion is
disabled, and the dashboard STOP / jog buttons say they send nothing.

**Defects the renderings exposed and this round fixed:**

| Defect | Why it mattered |
| --- | --- |
| An unconfirmed cancel showed "waiting for Idle" instead of "CANCEL NOT CONFIRMED - USE THE E-STOP" | the operator would not be told the machine may still be moving |
| The jog status line was cut off at "UNPROTECTED MIC" (64-byte buffer) | the gate state was truncated |
| "CANCEL NOT SENT" began with the word "allowed" | read as success in a failure dialog |
| The warning under the jog panel wrapped onto the panel border | text crossing the border and the screen edge |
| The capabilities note ran off the bottom of the screen | my own earlier addition |
| A result from X looked like a result for the selected Z | results now name their axis |

Each is guarded now (six re-introduced defects are caught by the layout checks or the unit tests).
Arming also was **not** dropped on switching to DEMO (the link can stay fresh); it is now, through one
pure, tested function (`JcDispatcher::enforce_arming_scope`) that also covers leaving the page and
expiry, and no arming or acknowledgement is ever restored afterwards.

## 7. Build results

| Target | Result | Flash | RAM |
| --- | --- | --- | --- |
| `jc8048w550` (ordinary) | SUCCESS | 1,039,325 B (31.1%) | not re-measured |
| `jc8048w550_livejog_test` (experimental, **not flashed**) | SUCCESS | 1,049,853 B (31.4%) | not re-measured |
| `m5dial` | SUCCESS | 1,262,241 B (unchanged) | |
| `cyddial` | SUCCESS | 1,185,829 B (unchanged) | |

The only compiler warnings are two pre-existing narrowing warnings in `JcScreenOffsets.cpp`,
a file this work did not touch. The ordinary firmware on COM3 was uploaded during Instruction 006
(hashes verified, boots with "motion commands disabled"); it was **not** re-flashed for 006A, so
the device runs the 006 ordinary build, not the 006A one. The 006A ordinary image has no
motion-transmit strings (binary scan) and differs only by a few shared-code bytes.

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
6. ~~The setting paths in section 2.2.~~ Confirmed on this controller for X, Y, Z, A (A's
   homing items refused); see section 2.2.
7. Whether soft limits are enforced when the machine is not homed (this pendant does not
   assume they are).
8. The A axis is rotary, in degrees. FluidNC cannot report this; it stays operator-declared
   and A jogging is disabled.

## 8a. Loss of communication: the software cancel cannot be relied on

The jog-cancel byte `0x85` travels over the same Wi-Fi / TCP link as the jog itself. **If that link is
down, stalled or dropped, no software cancel can reach the controller.** This is a property of the
design, not a bug that can be fixed in software:

* with the connection lost, `CANCEL JOG` cannot transmit; the page then says "CANCEL NOT SENT ... USE
  THE PHYSICAL E-STOP NOW" and, while a jog is unresolved and the link is not fresh, shows the red
  line "NO COMMUNICATION: software cancel unreliable. USE E-STOP";
* the software cancels what it can locally on a loss (the session, the arming, the capability read)
  and never replays; it cannot stop a move the controller is already executing;
* even with the link up, a cancel that the controller does not confirm is reported as "CANCEL NOT
  CONFIRMED - USE THE E-STOP" after 3 s;
* the bound on unattended motion in this test configuration is the move itself: 1 mm at 100 mm/min
  (about 0.6 s plus acceleration). That is a property of the test limits, not a safety system.

Every statement above is tested on the host (section 6) and visible in the screenshots (section 6b),
but none of it has been observed on the real controller.

## 9. Remaining prerequisites before a real jog

0. **An independent emergency-stop system (physical, hardware).** It is a **required prerequisite**
   for any live motion. **It is not installed, not tested, and not designed or specified by this
   software project; this repository must not be read as saying otherwise.** Until the owner has
   installed it and tested it independently of this pendant, no live motion test may be run. The
   software cancel is explicitly not a substitute (section 8a).
1. **Owner review of this experimental implementation (006A / 006B / 006C), then owner approval to
   flash the experimental build and to send the first X jog.** Not given. This checkpoint
   stops before flashing.
2. ~~Real configuration read.~~ **Done and accepted** (sections 2.2 and 2.4).
3. ~~Decide whether the gate should be stricter when soft limits are OFF.~~ **Done in 006A**:
   UNPROTECTED MICRO-JOG mode with a per-axis, per-arming operator acknowledgement (section 4a).
4. **Physical conditions before the first jog** (the operator establishes these; the software
   cannot): machine stationary and reporting Idle; spindle / router / laser cannot start; no
   macro or job running; workpiece and tooling clear of the tested axis movement; the carriage
   visibly not against the end of travel in the commanded direction; an immediate stop /
   power-removal method within reach. Homing is **not** required, and FluidNC's configuration is
   **not** to be altered for this test.
5. **Z validation, a separate later gate.** Before any Z jog or any homing test: establish what
   GPIO39 physically is (negative-end switch, a positive-end switch labelled incorrectly, a
   shared / all-limit input, a wiring or configuration mismatch, or another deliberate
   arrangement), and the intended physical Z homing direction. Only then may a separate Z test
   be authorized. Z configuration is not altered by this project.
6. **Optional, recommended before homing is ever authorized:** verify the physical X / Y / Z
   switch inputs with FluidNC's own read / test mechanism (for example `$limits`) without
   commanding movement: actuate each switch by hand, record the reported input changes, exit the
   test mode normally. This is **not** required to prove the first X / Y micro-jog command path,
   and the pendant does not send it (it is not in the read-only allowlist).
7. **Verify the assumptions in section 8** on the real controller. Items 1 to 5 can only be
   confirmed by the first jog itself, which is why that jog is so tightly bounded.
8. **A written, owner-agreed test procedure** (the draft is in section 10).
9. **A axis:** rotary calibration (degrees per step, direction, wrap-around) and feed units
   before it is ever enabled. It stays disabled.
10. **Operator visual check** of the Jog, Capabilities and Limits / Homing pages in the
    experimental build and of the arming and confirmation dialogs. They were laid out without a
    photo review. The arming dialog uses seven lines, which is the most the dialog can show
    without touching its buttons.
11. **Review the experimental flash procedure**: how to return to the ordinary firmware afterwards
    (flash `env:jc8048w550`) and how to confirm it from the boot log.
12. **Wi-Fi behaviour during motion**: loss and stale status are handled in the software and
    simulated, but how the real link and controller behave when the connection drops mid-jog has
    never been observed.
13. **Separate approval for anything beyond one restricted X / Y jog at a time**: Z, A, larger
    steps, continuous jogging, homing, probing, macros, spindle, laser, files.
14. No merge into `main` and no release without separate approval.

## 10. DRAFT first-motion procedure (not approved, not executed)

A starting point for the owner's review. It must not be followed until section 9 item 1 is
satisfied and the owner has approved it. **X first, then Y. Z is not tested.**

**Conditions** (section 9 item 4) are established and nothing is changed in FluidNC.

**First test: X only, one ordinary 1 mm jog.**

1. With the experimental build flashed (after owner approval): confirm the boot log says
   "LIVE JOG TEST BUILD" and the Settings page shows the red banner.
2. LIVE connected. Wait for Idle.
3. READ CONTROLLER. Confirm X: soft OFF, hard OFF, and that the read is current (a new read is
   needed after every connection loss or reboot).
4. Open Jogging, select X, 1 mm, 100 mm/min. The page shows `gate: UNPROTECTED MICRO-JOG`.
5. Tap ARM TEST. Read the NO CONTROLLER TRAVEL PROTECTION warning and tap I ACKNOWLEDGE.
6. Confirm physical clearance, and choose the physically clearer direction.
7. Jog **X +1.0 mm or X -1.0 mm at 100 mm/min**. Read the per-jog confirmation, confirm.
8. Observe the actual movement. Require all of: command acknowledged ("ACCEPTED"); movement
   observed; a post-acknowledgement Idle; the position change consistent with the requested
   magnitude and sign ("COMPLETED: moved ..."). **No further jog is sent automatically.**
9. If successful: repeat the equivalent bounded test on Y. **Do not test Z.**

**Separate cancel test** (only after one ordinary 1 mm X or Y jog succeeded; same axis and rate,
no raised limit; not part of the first transmitted motion):

10. Jog 1 mm and press CANCEL JOG. Verify: `0x85` reached the controller; movement stopped; no
    queued jog started after the cancel; the final state is shown correctly; any failure result
    stays latched until acknowledged; nothing is replayed. The overtaken-cancel protection
    (a jog that starts after a cancel is detected, cancelled again and latched) stays in force.
    A 1 mm move at 100 mm/min lasts about 0.6 s, so a cancel race may be hard to hit; if the move
    finishes first, that is a valid "completed" result and nothing is raised to make it longer.

**Stop at once (E-stop / power removal first, then CANCEL JOG) and return to investigation on:**
movement direction differing from the command; movement distance materially wrong; the machine
not returning to Idle as expected; acknowledgement semantics differing from section 8; a cancel
that does not stop motion; queued motion appearing; a reconnect causing a replay; an unexpected
Alarm; inconsistent position reporting; any axis approaching an obstruction or end stop; the UI
state differing from the controller state. **Never compensate by increasing the movement
distance.**

11. Afterwards, reflash `env:jc8048w550` and confirm "motion commands disabled" in the boot log.

**Stop point:** real motion is not enabled and this report does not request it.

## 11. Remaining-test checklist (nothing below has been done)

Software and host verification is complete for this checkpoint. What is left needs the owner, the
operator or the machine:

**Before any flashing of the experimental build**
- [ ] Independent physical emergency stop installed and tested by the owner (section 9, item 0).
- [ ] Owner review of the experimental implementation and written approval to flash it (section 9, item 1).
- [ ] Operator photographs of the experimental build: arming dialog, jog page (X ready, armed, Z hold,
      A disabled), per-jog dialog, Limits page, dashboard banner. Compare with `ui-preview/`.
- [ ] Z homing direction / GPIO39 side verified physically (section 9, item 5), if Z is ever to be tested.
- [ ] Optional, before homing is ever authorized: `$limits` switch check (section 9, item 6).

**On the machine, only after the above, ordinary firmware first**
- [ ] LIVE connected, Idle, controller read current; X soft OFF / hard OFF confirmed on the page.
- [ ] Physical conditions of section 9 item 4 established.

**First live motion (experimental build, owner-approved)**
- [ ] X +/- 1.0 mm at 100 mm/min: acknowledged, movement seen, Idle after the acknowledgement, position
      change matches. Then Y. (Section 10.)
- [ ] Separate cancel test at the same restricted rate: `0x85` reaches the controller, motion stops, no
      queued jog starts, final state correct, failure latched, no replay.
- [ ] Confirm or correct the assumptions in section 8 (the `$J` form, what `ok` means, `0x85` over Telnet).
- [ ] Observe what the real controller and link do when the connection drops during a jog (section 8a).

**Automatic connection (ordinary firmware)**
- [ ] A temporary Wi-Fi or controller loss and the recovery, on hardware (see `jc-auto-connect.md`).
- [ ] A true power-cycle (supply removed and restored), not only a reset.

## 12. Non-motion jog test mode (Instruction 007)

**What it is.** The ordinary firmware (`env:jc8048w550`) now carries the experimental jog interface
in a **non-motion test mode** (`-DJC_JOG_PREVIEW=1`, set only in that environment). The real jog page,
arming dialog, per-axis acknowledgement, per-jog confirmation, result and cancel flows, and the real
gate and jog session run unchanged. Their only transport is `JcPreviewController`
(`src/jc8048w550/JcJogPreview.hpp`): a few hundred bytes of RAM that pretend to be a controller. It has
no socket, UART, Wi-Fi or Arduino reference. The real FluidNC read-only status and capability queries
still run over the real link.

**Why nothing can move.**
- `JC_MACHINE_CONTROL_ENABLED` stays 0; `JC_JOG_PREVIEW` is a compile `#error` together with
  `JC_LIVE_JOG_TEST` or enabled machine control.
- The dispatcher's gate is in preview mode (`set_preview(true)` before the sink is installed). An
  accepted command is counted as `previewed`, a declined one as `declined`; `sent()` cannot move.
- The simulation re-checks every line with the strict jog-line rule and refuses anything else.
- `tools/check_no_sink.py` (guard rule 6) fails the build if the preview block references a real
  transport, if the simulated controller header references any link or Arduino API, if the preview
  flag appears outside `[env:jc8048w550]`, or if the live-jog env inherits it.
- The real transmit code (`send_jog_bytes`, the 0x85 write) is compiled only into
  `env:jc8048w550_livejog_test`. It is absent from the ordinary image (checked on the binary).
- While a rehearsed jog is in flight the session hears only the simulation: real controller `ok`
  lines (replies to read-only queries) are not given to the jog session.

**Marking.** "PREVIEW ONLY - MOTION DISABLED" appears in every page header (shorter wordings where the
title is long: "PREVIEW - NO MOTION", "PREVIEW ONLY"), the dashboard header, the jog warning line, the
Menu and Settings footers, the arming title ("PREVIEW ONLY: ARM MICRO-JOG?"), the per-jog confirmation,
and the result dialog ("SIMULATED JOG - NOTHING SENT"). Result lines carry a "SIM:" prefix. The ASCII
font has no em dash, so a hyphen is used.

**Simulated scenarios** (SETTINGS > JOG PREVIEW TEST > NEXT SCENARIO): NORMAL, REJECTS (error:15),
ALARM (ALARM:1), NO REPLY, SLOW ACK, IGNORES CANCEL (ends in "CANCEL NOT CONFIRMED - USE THE E-STOP").
A simulated move shifts the position the session sees by the rehearsed displacement (the real machine
does not move). The page also shows the gate counters (previewed / SENT / blocked) and the simulation
counters (jog lines / cancels / refused). Preview mode is compiled in and always on in the ordinary
image; there is no runtime toggle and nothing is persisted.

**Loss of the link.** Disconnect, DEMO, stale status and Wi-Fi loss all end in
`note_link_fresh(false)` / `enforce_arming_scope`: arming, the per-axis acknowledgement, the capability
read and the simulation are cleared, and nothing is replayed after reconnecting.

### 12.1 Verification run on the development PC (2026-10-30)

| Check | Result |
|---|---|
| Host suites (core, app, control, conn; each policy) | PASS |
| Jog tests (ordinary, rules compiled in, live policy) | PASS (1267 / 1274 / 1354 checks) |
| `test_jc_preview.cpp` (real gate, dispatcher and session against the simulation; ordinary and rules-compiled-in policies) | PASS |
| Static guard `check_no_sink.py` including rule 6 | PASS |
| Mutation checks, 13 mutants (preview counted as sent, policy skips acknowledgement, simulation accepts any line, displacement dropped, declined counted as previewed, preview mode not set before sink, real transport in the preview block, Serial in the simulation, preview flag in the live env or an unrelated env, live env inherits ordinary flags, jog UI calls a link transmit, preview block uses the socket thunk) | all 13 caught |
| Host UI renderer (real page code and fonts): ordinary / preview / live builds | 8 / 29 / 27 frames, 0 problems |
| PlatformIO builds: `jc8048w550`, `jc8048w550_livejog_test`, `m5dial`, `cyddial` | all SUCCESS (2 existing narrowing warnings in JcScreenOffsets.cpp) |
| Ordinary binary has "PREVIEW ONLY - MOTION DISABLED" and "app: motion commands disabled"; lacks "LIVE JOG TEST BUILD", "jogtest:", "send_jog_bytes" | verified on `firmware.bin` |
| Live-jog binary has "LIVE JOG TEST BUILD"; lacks "PREVIEW ONLY", "jogpreview", "SIMULATED JOG" | verified (**not flashed**) |
| Upload of `env:jc8048w550` to COM3 | SUCCESS, hash verified |
| Boot on the panel (serial log) | preview banner, "app: motion commands disabled", auto-connect, Wi-Fi 192.168.0.135, TCP connected, "ui: LIVE entered", "mode=LIVE (PREVIEW ONLY - MOTION DISABLED)" |

The renderer output is a rendering of the page code, **not** a photograph of the panel. The live-jog
binary still contains the page title "JOG PREVIEW TEST" (a title table shared by all builds); the page
itself is not compiled into it.

### 12.2 Operator acceptance on the panel

**STATUS: PENDING.** Nothing in the checklist has been confirmed by a person at the panel yet. The
checklist is [jc-jog-preview-acceptance.md](jc-jog-preview-acceptance.md); results are recorded there.

### 12.3 What this does NOT prove

The simulation proves the interface and the software gate, not any property of the machine. No jog
or cancel byte has been sent to the controller. The independent emergency-stop system is a required
physical prerequisite and is **not installed or tested**. Sections 8 and 9 still apply.

## 13. UI corrections and capability-read workflow (Instruction 008)

Defects reported from the panel after Instruction 007, and what was done. The restricted jog implementation
(X/Y 1 mm at 100 mm/min, Z blocked, A disabled, per-axis acknowledgement, arming, per-jog confirmation,
fresh Idle, capability validation, transport validation, acknowledgement tracking, motion verification,
cancel and timeout handling, no replay) is unchanged and its tests still pass.

| Defect | Fix |
|---|---|
| Dashboard: MENU / LINK overlapped other content; the live dashboard carried "NO JOG HERE: USE MENU" and "NO CANCEL HERE" placeholder buttons that overlapped the status cells | New shared geometry `JcDashLayout.hpp` (drawn AND hit-tested from the same rectangles): title, mode chip, link chip, build banner, MENU, LINK on one non-overlapping header row. The placeholders are removed. Their space now shows the saved endpoint (host:port), status age and a one-line mode note |
| Jog page: crowded rows, footer text over a control | New shared geometry `JcJogLayout.hpp`: controls above a status panel; panel rows (preview, result, motion, machine settings) and a two-line bottom area that never overlaps the ACKNOWLEDGE button |
| "CANNOT ARM THE JOG TEST: no capability read in this LIVE session" required navigating to unrelated pages | Capability workflow on the jog page (below) |
| Preview and live-jog builds both used a red banner | PREVIEW is blue (cannot move); the live-jog build is red (can move). Wordings differ as well |
| IP address hint | No address is hard-coded (grep of src, test, tools). The saved endpoint is shown on the dashboard and on the LINK page ("TAP TO EDIT HOST / IP", "EDIT HOST / IP" in Connections); auto-reconnect is unchanged |

**Capability workflow** (`JcCapsWorkflow.hpp`, pure; reads use the existing read-only loader and the same allowlisted queries):
- On entering the jog page in LIVE, if the settings are not current and a read is safe (LIVE, fresh status, machine Idle or Alarm, no unresolved jog, no read running), **one** automatic read starts per visit; a second one only after a lost-and-recovered connection. A failed read is never retried automatically.
- The button that shares the ARM slot shows what tapping does: READ SETTINGS / READING n/55 / RETRY READ / ARM / ARMED nnn s. ARM is offered only when the read is complete and current.
- A status line under the panel says what is known: "Machine settings NOT read", "Reading: n of 55 queries", "Read FAILED: <reason>", "EXPIRED (n min old)", "read n s ago (valid 5 min)". In the preview it is prefixed "REAL controller:" while jog results are prefixed "SIM:", so real and simulated data are never mixed.
- Expiry (5 minutes) and session binding (the read is discarded on every link loss) are unchanged. UNKNOWN values are never replaced; the gate's own checks still decide.

**Verification (development PC, 2026-10-30).** All host suites PASS (new: 23 checks for the workflow, 71/69 for the preview, existing 1267/1274/1354 jog checks); simulator integration PASS; static guard PASS; renderer: ordinary 8, preview 34, live 32 frames, 0 problems, plus pure geometry checks for the dashboard header/body and the jog page (no overlaps, labels fit, touch targets). 11 mutants for the new rules (arming on an expired read, auto-retry after failure, automatic read with stale status or a moving machine, repeated automatic read, changed expiry, UI arming without a current read, overlapping dashboard / jog geometry) were all caught. All four PlatformIO environments build. The ordinary binary contains no live-jog strings and no `send_jog_bytes` / `jog_tx_thunk`.

**Limits of this verification.** The dashboard is drawn by `PendantUi`, which the host renderer cannot compile; its geometry is verified from the shared layout header, not by rendering the page. The operator's photographs of the dashboard and the jog page are needed to confirm the result on the panel (see [jc-jog-preview-acceptance.md](jc-jog-preview-acceptance.md), part F).

## 14. Complete machine-control interface (Instructions 009 and 009A)

FluidDial-JC is an HMI and command client; **FluidNC is the authority** on motion, travel, homing, probing and
acceptance. The pendant checks command integrity only (syntax, units, axis, one operation at a time, fresh
status, no alarm, confirmation of consequential actions, no replay, clear results).

**Builds.** `env:jc8048w550` (ordinary): machine control stays disabled, no transmit path to the machine; the
whole interface runs against a **simulated FluidNC** (PREVIEW ONLY - MOTION DISABLED).
`env:jc8048w550_fullcontrol_test` (**new, never flashed**): the same interface over the real transports.
`env:jc8048w550_livejog_test` (unchanged): the owner's restricted X/Y 1 mm test build.

**One command interface** (`JcCommandSystem.hpp`, `JcCommandPolicy.hpp`). Gate sink -> jog session (motion
verification) or the single-operation command link -> `checked_tx` -> transport `SendFn`. The strict grammar of
every line the builders generate (jog on X/Y/Z/A, home, WCS select, G10 L2/L20, probe, retract, positioning,
SD run, macro run, M5, real-time bytes) is **one function** (`jc_command_bytes_allowed`) used in front of Wi-Fi,
UART and the simulator. The links only add framing. BLE UART: no BLE transport exists in this firmware; it would
supply only a `SendFn`.

**Gate policy `Authority`** (preview and full build): armed, LIVE, fresh status, fresh command, no alarm, one
operation, state for the class, and the operator's confirmation for Home / Probe / WorkOffset / Spindle / Job /
Macro. **No** capability-read, homing, limit-pin, travel-envelope or per-axis acknowledgement prerequisite; the
jog needs only arming (the explicit confirmation). Stop (jog cancel, feed hold) is always available when LIVE.
Alarm recovery (unlock, reset) stays available. The restricted live-jog build keeps its old, stricter rules.

**Homing facts** (`JcHomingState.hpp`, Homing page): configured, switch fitted, switch active (Pn:), homed in this
connection, position trusted, accepts jog, must_home - shown separately; unknown is "?" not "NO". Jogging needs
none of them. An unconfigured or inactive homing input is a configuration fact, not an error. Z homing (and
therefore HOME ALL, which includes Z) is not initiated until the owner confirms the Z direction for the session;
A has no homing configuration (`error:3`) and is never homed.

**Four-axis jog:** X/Y/Z linear, A in degrees (CW / CCW labels, positive = CCW per G-code convention: verify at
commissioning), incremental steps and feeds from the presets, one jog at a time, acknowledgement is not completion
(Idle + position checked), cancel (0x85), timeouts, link loss, no replay.

**Results** of every non-jog command are reported once (accepted / REJECTED error:N / ALARM:N / NO REPLY /
CONNECTION LOST, not resent).

**Positioning and probing routines** (Macros > POSITIONING / PROBING; `JcWorkflow.hpp`, `JcProbeCalc.hpp`):
GO TO WORK X0 Y0 / X0 / Y0 (operator-defined safe Z clearance first, never defaulted; the XY move only after a
separate CONTINUE; absolute work coordinates in the active WCS; no G53/G92; A and spindle untouched);
FIND X / Y / XY CENTRE (inside features; outside for X or Y), FIND ROTARY A CENTRELINE (two stations, Y pair + top
contact each, tilt reported, A calibration untouched). Parameters: search, feed, retract, optional slow second
touch, tip radius, expected size, contact timeout, clearance. Read-only `$G` and `$#` (new allowlisted query) read
the active WCS and offsets before the preview. Probe replies `[PRB:x,y,z:1|0]`, ALARM:4/5 are validated; a centre is
only computed from complete contacts. MOVE TO CENTRE and SET WORK ORIGIN (G10 L2 on the active WCS, read back and
compared; never G92) are separate, confirmed actions. A failed step, alarm, timeout, cancel (feed hold) or
connection loss ends the routine; nothing continues or resumes.

### 14.1 Verification (development PC, 2026-10-30)

| Check | Result |
|---|---|
| Existing host suites (core, app, control, conn, jog x3 policies, capability workflow, preview x2) | PASS |
| New `test_jc_commands.cpp` (policy accepts every builder line, rejects free G-code / chained / wrong class; four-axis formatting; authority gate; every class through a simulated FluidNC: ack, rejection, alarm, timeout, link loss, cancel, reconnection, invalid, unsupported, single operation, no duplicate) | 767 checks PASS |
| New `test_jc_workflow.cpp` (go-to, clearance refusals, spindle, WCS change, cancel, disconnect; X / Y / XY / outside / rotary centres; probe never triggered, already triggered, alarm on the second touch, timeout, wrong radius, parameters, slow touch; set-origin read-back, ignored G10, other WCS, G92) | 180 checks PASS |
| Simulator integration (Python TCP simulator) | PASS |
| Static no-motion guard (incl. full-control env rules, shared byte policy in both links, pure-code rules) | PASS |
| Mutation checks: 21 mutants (free G-code accepted, capability prerequisite, alarm rule, single-operation rule, byte policy skipped, homed state kept across link loss, Z silently homed, routine continues after failure, XY without pause, unchecked pair, WCS change, read-back, probe triggered, spindle, clearance below Z, real transport in preview, env flags, link checks, Serial in pure code) | all 21 caught |
| Host UI renderer (ordinary / preview / live) | 8 / 35 / 32 frames, 0 problems |
| PlatformIO: `jc8048w550`, `jc8048w550_fullcontrol_test`, `jc8048w550_livejog_test`, `m5dial`, `cyddial` | all SUCCESS |
| Ordinary binary: no full-control / live-jog strings, no transmit-bytes messages | verified |
| Upload of the ordinary image to COM3 and boot | SUCCESS (auto-connect, LIVE, PREVIEW ONLY) |

The renderer output is a rendering of the page code, not a photograph. The motion in the preview is simulated.
The full-control image was **not** flashed and no machine-changing command was transmitted to any controller.

### 14.2 Not done / limits

- Physical commissioning of every function (see [jc-commissioning-checklist.md](jc-commissioning-checklist.md)).
- Not implemented in the pendant: SD file **browsing** (needs the `$SD/List` / `$Files` query family, not allowlisted),
  job progress / pause / resume UI beyond the status fields and the `!` / `~` / reset controls, a tool-change
  workflow (the Tools page still previews M6 sequences only), probing of the rotary axis, FIND XY CENTRE for
  outside features (use FIND X then FIND Y), BLE UART. These are unfinished pendant features, not controller
  limitations. FluidNC itself supports them.
- Probe offsets / tool length offset handling (TLO from a Z tool probe) is not yet part of the new routines.
- The independent emergency stop is **not installed or tested**; machine position is not established.
