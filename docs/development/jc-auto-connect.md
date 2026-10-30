# JC automatic connection and reconnection (Instruction 006C.1)

Status: **experimental.** The ordinary firmware (`JC_MACHINE_CONTROL_ENABLED=0`) is unchanged in what
it can transmit: `?` and the read-only queries. Auto-connect restores a *read-only status link*. It
never restores, arms or sends anything that moves the machine.

## 1. Why it did not connect after power-up

By design, nothing ever called `connect()` at boot. `JcReadOnlyFluidNc::begin()` loaded the saved Wi-Fi
settings and logged `auto-connect disabled`; the only paths to `connect()` were the CONNECT button on the
Wi-Fi page and the USE LIVE flow. Separately:

* the link retried on fixed timers (2 s for the FluidNC endpoint, 15 s for Wi-Fi), with no backoff;
* switching to DEMO left the link running and retrying, so it could silently reconnect;
* the connection state text was "FLUIDNC LINK / CNC OFFLINE / DISCONNECTED", not the six states below.

## 2. What is persisted (NVS)

| Item | Where | Notes |
| --- | --- | --- |
| Last selected transport | `jc_app` blob (`transport`: 0 Wi-Fi, 1 UART, 2 BLE) | already existed |
| Wi-Fi SSID, password, BSSID pin | `jcnetwork` | already existed |
| FluidNC host name / IP | `jcnetwork` `host` | already existed |
| FluidNC port | `jcnetwork` `port` | new; default 23 |
| Auto-connect preference | `jcnetwork` `auto` | new; absent = "not yet chosen" |

The credentials are stored unencrypted, as before; they are never logged.

The preference is *what the operator last chose*:

| Operator action | Preference |
| --- | --- |
| CONNECT, or LIVE (when not connected) | On |
| DISCONNECT | **Off** |
| Switch to DEMO | **Off** (and the live connection is closed) |
| First valid status of a connection, when not yet chosen | On (never overrides an explicit Off) |

"Not yet chosen" behaves as On once a valid Wi-Fi / FluidNC configuration is saved, so an already
configured device reconnects after the update without a manual step. This is slightly broader than
"after a successful connect", and is the one deliberate default; turning AUTO off is one tap.

## 3. Boot sequence

1. Display and touch initialize normally; calibration, settings and the saved network configuration load.
2. `jc_boot_connect_decision()` (pure, tested): connect only if the **Wi-Fi transport is the selected
   one**, a **saved configuration exists** and the preference is **not Off**. A wired UART or BLE
   selection is never auto-started and transports are never switched.
3. The link joins Wi-Fi, resolves and opens the FluidNC endpoint, and waits for a **valid, fresh status
   frame**. Only then does the UI enter LIVE (`jc_auto_live_allowed()`); until then it shows the
   connecting state, not LIVE.

## 4. States shown to the operator

`WIFI CONNECTING`, `FLUIDNC CONNECTING`, `CONNECTED`, `RECONNECTING`, `DISCONNECTED`, `STALE`
(`jc_conn_label()`). `CONNECTED` is produced by exactly one input: valid, fresh status. After the first
valid status of a boot, every further attempt is `RECONNECTING`. Data that stopped (3 s without a
frame) or is invalid is `STALE`, never `CONNECTED`. They appear in the dashboard header chip, the
Connections page, the Wi-Fi page and Diagnostics.

## 5. Loss and recovery

| On Wi-Fi or FluidNC loss | How |
| --- | --- |
| Positions become STALE | the link state changes at once on a closed socket or Wi-Fi drop; a silent stall is stale after the 3 s freshness window (not instantaneous) |
| Arming is cleared | `JcApp::note_link_fresh()` -> `JcDispatcher::on_link_lost()` (also clears the per-axis acknowledgement) |
| Pending command state cancelled locally | the jog session and capability read are abandoned; nothing is stored for later |
| Never replayed | no queue, no resend; after a reconnect nothing is transmitted until the operator re-arms, acknowledges a failure and confirms a new jog |
| Bounded exponential backoff | FluidNC endpoint: 2, 4, 8, 16, 30, 30 ... s; Wi-Fi association: 4, 8, 16, 30, 30 ... s; reset by the next valid status (`jc_backoff_ms()`) |
| UI stays responsive | all retries are timer-driven and non-blocking; no wait loops were added |

Not restored after any restart or reconnect: arming, the acknowledgement, the capability read, unfinished
jobs, jogs, command queues.

## 6. Manual disconnect and DEMO

A manual DISCONNECT, or switching to DEMO, sets the preference Off and closes the link, so **nothing
reconnects** (no retry timer runs) until the operator selects CONNECT, LIVE or AUTO: ON again. LIVE
requested without a link starts the saved connection and enters LIVE only when valid status arrives.

## 7. Diagnostics

Connections page: AUTO: ON/OFF toggle (persisted) and the current state. Wi-Fi page and Diagnostics:
current state, auto on/off, **last successful connection** ("12 s ago" / "never"), **reconnect
attempts**, **seconds until the next retry**, **last connection error**. Serial log lines (`net:`)
record each attempt, each backoff delay, the first valid status and the preference changes.

## 8. Evidence

**Host tests** (`test/host/test_jc_conn.cpp`, 228 checks; `tools/sim_conn_tests.py`):
backoff values and bounds; the six labels (CONNECTED from one input only); preference transitions;
the boot decision matrix (including UART / BLE never auto-started); LIVE only after valid status;
the tracker's retry timeline (due exactly on time, not 1 ms early); manual disconnect (12 minutes of
silence); DEMO; stale after 3 s. **Simulator:** real refused TCP connections, the 2/4/8/16/30/30 s
timeline, the controller returning, CONNECTED only after a valid frame, nothing but `?` sent. Mutation
check: 9 deliberate faults in the policy and tracker, all caught.

**Hardware, ordinary firmware, COM3, 2026-10-30 (board reset with the serial RTS line, boot cause
`rst:0x1 POWERON`, no touch input):**

| Boot | Preference at boot | Result |
| --- | --- | --- |
| First after flashing | not yet chosen | auto-connect started; Wi-Fi joined (192.168.0.135); TCP to 192.168.0.134:23; first valid status; preference saved ON; **LIVE entered automatically** |
| Second | on (persisted) | same sequence, LIVE entered automatically |

Both logs show "motion commands disabled": the ordinary firmware, nothing but `?` transmitted.

**Not yet verified on hardware:** a temporary Wi-Fi or controller loss and the recovery. The host and
simulator tests cover the retry timing and state rules, but the Wi-Fi association behaviour and the
real link's recovery are only checked on the device. A passive capture is running for that test; the
result is recorded in `jc-live-jog-readiness.md` / this file when it has been performed.
A true power-cycle (removing and restoring USB / supply power) was approximated by the RTS reset, which
reproduces the boot path and NVS state but not the supply sequence.

## 9. Known limits

* "Stale immediately" is immediate for a closed socket and a Wi-Fi drop, and 3 s for a silent stall.
* The unprotected-state and DEMO rules rely on the preference being written before a power loss; NVS
  writes happen on the operator action, not on a timer.
* The FluidNC port has no editor; it is persisted (default 23) for completeness.
* The legacy dashboard and Wi-Fi pages are not rendered by the host UI checker; they were changed
  conservatively and verified by build and the serial log.
