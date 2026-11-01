# JC feature status matrix

Legend: **Verified** = operator-confirmed on hardware. **Uploaded** = built,
flashed and booted on the JC (boot log checked) but not operator-confirmed.
**Host-tested** = automated host tests / simulator integration only.
**Preview** = full UI flow and command generation; the authorization gate
blocks every transmission. A successful compile is never counted as hardware
verification. Instruction 004 checkpoint.

Machine control is disabled (`JC_MACHINE_CONTROL_ENABLED=0`). This firmware
sends a controller only the `?` status query and, when the operator taps READ
FROM CONTROLLER, a closed set of read-only configuration queries (Instruction
005, extended in 006). It sends no jog, home, probe, offset, job, macro or unlock
command. A separate, explicitly named experimental build
(`env:jc8048w550_livejog_test`) can send one restricted jog and the jog-cancel byte;
it has not been flashed to any device.

| Area | Status | Evidence / blocker |
| --- | --- | --- |
| Display, GT911 touch, five-point calibration, NVS calibration, Roboto fonts | Verified | Phase 2A acceptance |
| Four-axis dashboard (X/Y/Z mm, A deg), DEMO mode | Verified (Phase 2A); extended layout Uploaded | |
| Dashboard LIVE: machine state, MACHINE/WORK toggle, feed, spindle, alarm, job %, transport | Uploaded | Operator saw LIVE / connected / Idle in an earlier build; new info panel not yet seen. Active WCS shows "unknown": reading it needs a `$G` query. |
| Application menu (9 sections) and navigation | Uploaded; navigation graph Host-tested | `test_jc_app.cpp` |
| Jogging page (X/Y/Z/A, presets, feeds, incremental + continuous, cancel, motion indicator) | Preview | Continuous jog is never generated for the rotary A axis |
| Homing page (all / per axis, preflight, confirm) | Preview | A has no assumed home switch (Settings > Machine) |
| Probing page (Z tool, edge X-/X+/Y-/Y+, params, probe pin status, preflight) | Preview | A axis never probed; probe pin comes from the controller `Pn:` field |
| Work offsets (G54-G59, machine/work, zero, set, WCO) | Preview | Unknown WCO shown as `--`, never 0; active WCS unavailable without `$G` |
| Files and jobs (Instruction 010) | Host-tested + renderer-checked; **live read-only SD listing** (`$SD/Status`, `$SD/List`) works in the ordinary image; hold and reset are the only job controls (simulated in that image); **no START and no RESUME exist (Instruction 010A: programs are started from the PC)** | The controller's SD card only (never the pendant's flash). Selected file, size, folders, scrolling, missing card / empty / error states. Job state and percentage are the controller's own `SD:` report. Listing format checked against the real controller's v4.1.1 reply (59 files, 2 folders reconstructed); names with spaces are listed normally; nothing on the pendant starts a file |
| Built-in positioning (Instruction 011A): GO TO WORK X0 / Y0 / X0 Y0 / **Z0** / **X0 Y0 Z0** | Host-tested against a simulated FluidNC + renderer-checked; rehearsed (not executed) in the ordinary image | Work coordinates of the selected G54..G59 system, never machine home. Combined XYZ: clearance first, XY, then a separate confirmation before Z0. A axis never commanded, no offset written |
| Macros | **Disabled** (Instruction 010A) | User macros cannot be run or edited from the pendant until the owner defines their command permissions; stored slots can be cleared. Built-in positioning routines are separate and unaffected |
| Tools (Instruction 010): controller T word and `[TLO:]` from `$G` / `$#`, reference tool, manual tool change + tool-length measurement, TLO apply (G43.1) / clear (G49) with readback | Host-tested against a simulated FluidNC; routines are rehearsed (not executed) in the ordinary image | No ATC or M6 is assumed or ever sent. Probe contact is verified by hand before any probing move. Tool length compensation is separate from work zero. G43.1/G49 support is unverified on the real controller until a readback matches |
| Settings: connections, display (brightness PWM, decimals, reference), jog presets, machine, calibration, Wi-Fi setup | Uploaded | Settings persisted as a versioned+CRC NVS blob (Host-tested encode/decode) |
| Diagnostics (firmware, build, memory, link, errors, gate counters, state history) | Uploaded | |
| Wi-Fi read-only status link | Uploaded; simulator Host-integrated | Earlier operator photo: connected, Idle |
| Wired UART read-only status link | Uploaded (code); never run | Needs wiring the operator has verified; JC pins not assumed |
| Bluetooth LE UART (Instruction 010): HM-10 / CC2541 FFE0/FFE1 and Nordic UART, scan, connect, GATT validation, notifications, bounded queues, saved device, reconnect, diagnostics | Core host-tested with a MOCK radio; firmware radio compiled; **no hardware communication has been tested** | The external CC2541 module is not physically verified. One command policy shared with Wi-Fi/UART; the ordinary image has no BLE command path |
| Status parser / frame assembler / machine model / link tracker | Host-tested | 4-axis, fragmentation, malformed, WCO conversion, stale, loss/reconnect |
| Command builders (jog, continuous, home, WCS, zero/set, probe, edge probe; no program start / resume / macro file) | Host-tested | Not transmitted |
| Dispatcher / authorization gate | Host-tested | Blocks every class in the shipping build; no replay; disarm on link loss |
| Simulator integration | Host-integrated | Real TCP to the simulator + real JC C++ logic. The ESP32 Wi-Fi/lwIP firmware itself has NOT been run against it |
| Controller capability read (`$I`, `$G`, `$/axes/*`, `$/probe/*`), Controller Capabilities page | Host-tested + Host-integrated; Uploaded; real read operator-confirmed 2026-10-30 (photo) | Read-only, operator-triggered, allowlisted. Real values in [jc-machine-control-readiness.md](jc-machine-control-readiness.md) |
| Reply handling: `ok` / `error:N` / `ALARM:N`, timeouts, connection loss, no resend | Host-tested + Host-integrated | `JcCommandExec`; query replies seen on the real controller, command replies (after a motion command) still unverified |
| Limits replaced by controller values only when confirmed; unconfirmed jog/probe blocked when control is enabled | Host-tested | Placeholders remain until a read completes; discarded on link loss |
| Dispatcher: sequences step only after acknowledgement | Host-tested (fake transport) | No sink exists in the firmware |
| Dry-run page | Uploaded | Evaluates only; records/sends nothing |
| Soft / hard limit, limit-pin, homing, must_home read-only queries (25 added) and Limits / Homing page | Host-tested + Host-integrated; Uploaded; real read operator-confirmed 2026-10-30 (photo + COM3 capture) | 50 answered, 3 refused (A homing). Soft and hard limits OFF on all axes, `must_home` false. UNKNOWN / `error:3` is never shown as OFF. Evidence: [evidence/2026-10-30-controller-read-capture.md](evidence/2026-10-30-controller-read-capture.md) |
| First-live-jog test configuration (`env:jc8048w550_livejog_test`): X/Y 1 mm only (Z on hold, A disabled), jog session, cancel, per-axis gate (PROTECTED / UNPROTECTED MICRO-JOG / BLOCKED) with operator acknowledgement | Host-tested + Host-integrated; **built, NOT flashed, never run** | See [jc-live-jog-readiness.md](jc-live-jog-readiness.md). Real controller: no travel protection, position not established. No jog or cancel byte has been sent to any controller. Owner review required before flashing |
| Non-motion jog test mode in the ordinary firmware (`-DJC_JOG_PREVIEW=1`): the experimental jog interface (arming, acknowledgement, confirmation, results, cancel) runs against a SIMULATED controller; "PREVIEW ONLY - MOTION DISABLED" throughout | Host-tested + renderer-checked + **uploaded to COM3, booted**; operator acceptance PENDING | `sent()` cannot move; nothing is transmitted. See [jc-live-jog-readiness.md](jc-live-jog-readiness.md) section 12 and [jc-jog-preview-acceptance.md](jc-jog-preview-acceptance.md) |
| Complete FluidNC operator interface (Instructions 009 / 009A): unified command system, four-axis jog, homing facts, positioning and probe-centre routines, work-offset write + read-back | Host-tested against a simulated FluidNC; ordinary image uploaded (simulated motion); `env:jc8048w550_fullcontrol_test` **built, NOT flashed** | No machine-changing command has been transmitted. SD browsing, job control UI, tool routines and BLE were added in Instruction 010 (rows above). See readiness report sections 14 and 15 and [jc-commissioning-checklist.md](jc-commissioning-checklist.md) |
| Automatic connection and reconnection (Wi-Fi): persisted choice, boot auto-connect, bounded backoff, six honest states, manual-disconnect / DEMO suppression, diagnostics | Host-tested + Host-integrated; Uploaded; **two hardware resets reconnected by themselves (operator input: none)** | Wi-Fi-loss recovery on hardware pending. [jc-auto-connect.md](jc-auto-connect.md) |
| Host UI renderings and layout checks of the real page code | Host-verified (35 frames, 0 problems) | Not photographs of the panel; operator photos still the real visual check. [ui-preview/](ui-preview/) |
| Independent emergency-stop system | **Not installed, not tested, not part of this software** | Required physical prerequisite for any live motion |
| Physical CNC command transmission | Disabled by design | Needs a separate safety review and owner approval |

## Instruction 003B physical acceptance log

Baseline firmware `4e53f87`, JC on COM3, machine control disabled. Results are
operator statements; no photographs were supplied, so none are claimed.

| Item | Result |
| --- | --- |
| Dashboard DEMO and LIVE, MACHINE/WORK toggle, menu, Back | Operator-confirmed, no defects |
| Jogging (A in degrees, 50/100 mm, blocked dialog, Cancel/Back) | Operator-confirmed, no defects |
| Homing page and confirmation dialog | Operator-confirmed, no defects |
| Live read-only test against FluidNC at 192.168.0.134:23 (reachable: ping and TCP 23 checked from the PC) | Operator-confirmed and approved: four-axis values, MACHINE/WORK, LIVE/DEMO separation, stale on loss, reconnect without arming |
| Probing, Work Offsets, Files pages | Operator-confirmed, no defects (no photos) |
| Macros, Tools, Settings, Diagnostics pages | Operator-confirmed, no defects (no photos) |
| Settings reboot persistence, brightness PWM, touch responsiveness | Operator-confirmed, no defects (no photos) |

Outstanding defects: none reported. No firmware change was made in this round, so the
acceptance applies to firmware `4e53f87`. Host tests (111/125 core, 166/163 app) and
simulator integration were re-run and pass. PlatformIO builds were not re-run (no source change).
