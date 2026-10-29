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
005). It sends no jog, home, probe, offset, job, macro or unlock command.

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
| Files and jobs | Partial | No SD listing: it needs a file-list command. Manual path + start/pause/resume/cancel preview; live job progress from the status `SD:` field |
| Macros | Preview | User-defined slots in NVS (`$Localfs/Run=`); controller macro list not readable without a query |
| Tools (manual tool-change workflow, tool measurement preview) | Preview | Current tool / TLO unavailable (`$G`); no ATC assumed |
| Settings: connections, display (brightness PWM, decimals, reference), jog presets, machine, calibration, Wi-Fi setup | Uploaded | Settings persisted as a versioned+CRC NVS blob (Host-tested encode/decode) |
| Diagnostics (firmware, build, memory, link, errors, gate counters, state history) | Uploaded | |
| Wi-Fi read-only status link | Uploaded; simulator Host-integrated | Earlier operator photo: connected, Idle |
| Wired UART read-only status link | Uploaded (code); never run | Needs wiring the operator has verified; JC pins not assumed |
| Bluetooth LE | Unavailable | No compatible FluidNC BLE GATT service exists |
| Status parser / frame assembler / machine model / link tracker | Host-tested | 4-axis, fragmentation, malformed, WCO conversion, stale, loss/reconnect |
| Command builders (jog, continuous, home, WCS, zero/set, probe, edge probe, file, macro) | Host-tested | Not transmitted |
| Dispatcher / authorization gate | Host-tested | Blocks every class in the shipping build; no replay; disarm on link loss |
| Simulator integration | Host-integrated | Real TCP to the simulator + real JC C++ logic. The ESP32 Wi-Fi/lwIP firmware itself has NOT been run against it |
| Controller capability read (`$I`, `$G`, `$/axes/*`, `$/probe/*`), Controller Capabilities page | Host-tested + Host-integrated; page Uploaded (see 005 section) | Read-only, operator-triggered, allowlisted; never run on the real controller until the operator taps READ |
| Reply handling: `ok` / `error:N` / `ALARM:N`, timeouts, connection loss, no resend | Host-tested + Host-integrated | `JcCommandExec`; real-controller reply formats unverified |
| Limits replaced by controller values only when confirmed; unconfirmed jog/probe blocked when control is enabled | Host-tested | Placeholders remain until a read completes; discarded on link loss |
| Dispatcher: sequences step only after acknowledgement | Host-tested (fake transport) | No sink exists in the firmware |
| Dry-run page | Uploaded | Evaluates only; records/sends nothing |
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
