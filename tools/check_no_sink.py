#!/usr/bin/env python3
"""Static guard for the JC firmware: in the ORDINARY firmware machine control must
stay disabled and no transmit sink may exist. The only exception is the explicitly
named experimental live-jog test build, and only through code that is compiled out
of every other build.

Checks (exit status 1 on any failure):
  1. JC_MACHINE_CONTROL_ENABLED and JC_LIVE_JOG_TEST are turned on in platformio.ini
     ONLY inside [env:jc8048w550_livejog_test] (and there both are set to 1); no
     source file defines either macro as anything but 0; the live-jog environment is
     not a default environment.
  2. In the ORDINARY text of every firmware .cpp (everything outside
     "#if JC_LIVE_JOG_TEST" blocks) there is no command sink, no jog transmit
     function and no reference to the jog session sink. The sink is installed in
     exactly one place (JcApp.cpp), inside a live-jog block.
  3. Raw transmit calls (::send, Serial1.) appear only in the two links; in the
     ordinary text of the Wi-Fi link there are exactly two ::send sites ('?' and the
     allowlisted query); the one extra site lives inside a live-jog block and is
     preceded by the strict jog-line check.
  4. Both links route every non-'?' query write through jc_query_allowed().
  5. The ordinary build creates its jog session without a transport.
  6. NON-MOTION TEST MODE (JC_JOG_PREVIEW, Instruction 007): the macro is set only in
     [env:jc8048w550] and never in the live-jog environment; code under it contains no
     link/transmit reference; its only jog transport is the in-RAM JcPreviewController,
     its dispatcher is put in preview mode before the sink is installed; and the header
     of the simulated controller refers to no socket, UART, Wi-Fi or Arduino API.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "jc8048w550")
LIVE_ENV = "env:jc8048w550_livejog_test"
FULL_ENV = "env:jc8048w550_fullcontrol_test"
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL " + msg)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


_FEATURE = (("JC_LIVE_JOG_TEST", "live"), ("JC_FULL_CONTROL_TEST", "live"), ("JC_JOG_PREVIEW", "preview"), ("JC_JOG_UI", "ui"),
            ("JC_AUTHORITY", "ui"))


def _classify(cond):
    """(kind_if_true, kind_if_false) for one #if / #elif condition."""
    for macro, kind in _FEATURE:
        if macro in cond:
            negative = cond.lstrip().startswith("!") or re.search(r"!\s*" + macro, cond) is not None
            return ("ordinary", kind) if negative else (kind, "ordinary")
    return ("ordinary", "ordinary")


def split_buckets(text):
    """Splits source text into the code that is compiled in each build flavour:
    ordinary (no feature macro), live (JC_LIVE_JOG_TEST), preview (JC_JOG_PREVIEW) and
    ui (JC_JOG_UI = live or preview). #elif chains and #else branches are followed."""
    buckets = {"ordinary": [], "live": [], "preview": [], "ui": []}
    stack = []  # items: {"cur": kind, "else": kind}
    for line in text.splitlines():
        s = line.strip()
        directive = s.startswith("#")
        if directive and re.match(r"#\s*if(def|ndef)?\b", s):
            cond = re.sub(r"^#\s*if(def|ndef)?\b", "", s)
            if re.match(r"#\s*ifndef", s):
                cond = "!" + cond
            tr, fl = _classify(cond)
            stack.append({"cur": tr, "else": fl})
            line_kind = None
        elif directive and re.match(r"#\s*elif\b", s) and stack:
            tr, fl = _classify(re.sub(r"^#\s*elif\b", "", s))
            stack[-1]["cur"], stack[-1]["else"] = tr, fl
            line_kind = None
        elif directive and re.match(r"#\s*else\b", s) and stack:
            stack[-1]["cur"] = stack[-1]["else"]
            line_kind = None
        elif directive and re.match(r"#\s*endif\b", s) and stack:
            stack.pop()
            line_kind = None
        else:
            line_kind = "ordinary"
            kinds = [item["cur"] for item in stack if item["cur"] != "ordinary"]
            for pref in ("live", "preview", "ui"):   # the most specific compile condition wins
                if pref in kinds:
                    line_kind = pref
                    break
        for k in buckets:
            buckets[k].append(line if k == line_kind else "")
    return {k: "\n".join(v) for k, v in buckets.items()}


def split_live(text):
    """Returns (non_live_text, live_text): everything compiled into a build WITHOUT
    JC_LIVE_JOG_TEST (ordinary, preview and shared ui code) and the live-only code."""
    b = split_buckets(text)
    return "\n".join((b["ordinary"], b["preview"], b["ui"])), b["live"]


# ---- 1. policy macros -------------------------------------------------------------
ini = read(os.path.join(ROOT, "platformio.ini"))
section = None
env_flags = {}   # (section, macro) -> value
for raw in ini.splitlines():
    line = raw.strip()
    m = re.match(r"\[(.+)\]", line)
    if m:
        section = m.group(1)
        continue
    if line.startswith(";"):
        continue
    for macro in ("JC_MACHINE_CONTROL_ENABLED", "JC_LIVE_JOG_TEST", "JC_FULL_CONTROL_TEST", "JC_JOG_PREVIEW"):
        mm = re.search(r"-D\s*" + macro + r"\s*=?\s*(\S*)", line)
        if mm:
            env_flags[(section, macro)] = mm.group(1).strip("'\"") or "1"
        elif macro in line and "-D" not in line:
            fail(f"platformio.ini [{section}]: unexpected reference to {macro}")
ALLOWED = {
    "JC_MACHINE_CONTROL_ENABLED": (LIVE_ENV, FULL_ENV),
    "JC_LIVE_JOG_TEST": (LIVE_ENV,),
    "JC_FULL_CONTROL_TEST": (FULL_ENV,),
    "JC_JOG_PREVIEW": ("env:jc8048w550",),
}
for (sec, macro), value in env_flags.items():
    if sec in ALLOWED[macro]:
        if value != "1":
            fail(f"platformio.ini [{sec}]: {macro} must be 1 (got {value!r})")
    elif value != "0":
        fail(f"platformio.ini [{sec}]: {macro}={value} outside {ALLOWED[macro]}")
live_set = {k for (s, k) in env_flags if s == LIVE_ENV}
full_set = {k for (s, k) in env_flags if s == FULL_ENV}
if live_set - {"JC_MACHINE_CONTROL_ENABLED", "JC_LIVE_JOG_TEST"} or {"JC_MACHINE_CONTROL_ENABLED", "JC_LIVE_JOG_TEST"} - live_set:
    fail(f"platformio.ini [{LIVE_ENV}]: must set exactly JC_MACHINE_CONTROL_ENABLED and JC_LIVE_JOG_TEST (got {sorted(live_set)})")
if full_set != {"JC_MACHINE_CONTROL_ENABLED", "JC_FULL_CONTROL_TEST"}:
    fail(f"platformio.ini [{FULL_ENV}]: must set exactly JC_MACHINE_CONTROL_ENABLED and JC_FULL_CONTROL_TEST (got {sorted(full_set)})")
# neither experimental environment may inherit the ordinary build_flags (which carry JC_JOG_PREVIEW)
sec = None
for raw in ini.splitlines():
    line = raw.strip()
    m = re.match(r"\[(.+)\]", line)
    if m:
        sec = m.group(1)
        continue
    if sec in (LIVE_ENV, FULL_ENV) and not line.startswith(";") and ("JC_JOG_PREVIEW" in line or "${env:jc8048w550.build_flags}" in line):
        fail(f"platformio.ini [{sec}]: must not carry JC_JOG_PREVIEW or inherit the ordinary build_flags")
dm = re.search(r"default_envs\s*=\s*(.+)", ini)
if dm and ("livejog" in dm.group(1) or "fullcontrol" in dm.group(1)):
    fail("platformio.ini: an experimental motion environment must not be a default environment")

for name in sorted(os.listdir(SRC)):
    if not name.endswith((".cpp", ".hpp", ".h")):
        continue
    text = read(os.path.join(SRC, name))
    for macro in ("JC_MACHINE_CONTROL_ENABLED", "JC_LIVE_JOG_TEST", "JC_FULL_CONTROL_TEST"):
        for m in re.finditer(r"#\s*define\s+" + macro + r"\s+(\S+)", text):
            if m.group(1) != "0":
                fail(f"{name}: defines {macro} as {m.group(1)}")

# ---- 2. no sink in the ordinary text of firmware translation units ----------------------
SINK_WORDS = ("set_sink_ex", "JcCommandLink", "JcCommandSink", "JcCommandGate(", "JcJogSession::sink", "JcCommandSystem::sink",
              "send_jog_bytes", "jog_tx_thunk", "send_jog(", "send_command_bytes", "cmd_tx_thunk", "send_command")
ordinary_cpp = {}
live_cpp = {}
preview_cpp = {}
PREVIEW_SINK_OK = ("set_sink_ex", "JcCommandSystem::sink")
for name in sorted(os.listdir(SRC)):
    if name.endswith(".cpp"):
        b = split_buckets(read(os.path.join(SRC, name)))
        o = "\n".join((b["ordinary"], b["ui"]))
        l = b["live"]
        ordinary_cpp[name], live_cpp[name], preview_cpp[name] = o, l, b["preview"]
        for word in SINK_WORDS:
            if word in o:
                fail(f"{name}: references {word} outside a JC_LIVE_JOG_TEST / JC_JOG_PREVIEW block (ordinary firmware must not install a sink)")
            if word in b["preview"] and not (name == "JcApp.cpp" and word in PREVIEW_SINK_OK):
                fail(f"{name}: references {word} inside a JC_JOG_PREVIEW block (only the simulated sink wiring in JcApp.cpp is allowed)")
sink_sites = [n for n, t in live_cpp.items() if "set_sink_ex" in t]
if sink_sites != ["JcApp.cpp"]:
    fail(f"the command sink must be installed only in JcApp.cpp inside a live-jog block (found in {sink_sites})")
preview_sites = [n for n, t in preview_cpp.items() if "set_sink_ex" in t]
if preview_sites != ["JcApp.cpp"]:
    fail(f"the preview sink must be installed only in JcApp.cpp inside a preview block (found in {preview_sites})")

# headers: the sink wiring (JcApp.hpp) and the transmit method (link header) are live-only too
for name in ("JcApp.hpp", "JcReadOnlyFluidNc.hpp", "JcUartLink.hpp"):
    o, _ = split_live(read(os.path.join(SRC, name)))
    for word in ("jog_tx_thunk", "send_jog", "send_jog_bytes", "send_command_bytes", "cmd_tx_thunk", "send_command"):
        if word in o:
            fail(f"{name}: declares {word} outside a JC_LIVE_JOG_TEST / JC_FULL_CONTROL_TEST block")

# ---- 3. raw transmit sites ------------------------------------------------------------------
ALLOWED_TX = {"JcReadOnlyFluidNc.cpp", "JcUartLink.cpp"}
TX_PATTERN = re.compile(r"::send\s*\(|Serial1\s*\.\s*(write|print|println)\s*\(|Serial2\s*\.")
for name in sorted(os.listdir(SRC)):
    if name.endswith(".cpp") and name not in ALLOWED_TX and TX_PATTERN.search(read(os.path.join(SRC, name))):
        fail(f"{name}: contains a raw transmit call outside the two links")

wifi = read(os.path.join(SRC, "JcReadOnlyFluidNc.cpp"))
uart = read(os.path.join(SRC, "JcUartLink.cpp"))
wifi_ord, wifi_live = split_live(wifi)
if len(re.findall(r"::send\s*\(", wifi_ord)) != 2:
    fail("JcReadOnlyFluidNc.cpp: expected exactly 2 ordinary ::send call sites ('?' and the allowlisted query)")
if len(re.findall(r"::send\s*\(", wifi_live)) != 2:
    fail("JcReadOnlyFluidNc.cpp: expected exactly 2 ::send call sites in the experimental blocks (live jog, full control)")
live_fn = wifi_live
sends = [m.start() for m in re.finditer(r"::send\s*\(", live_fn)]
checks = [live_fn.find("jc_jog_line_allowed("), live_fn.find("jc_command_bytes_allowed(")]
if len(sends) == 2 and (min(checks) < 0 or sorted(checks) != checks or checks[0] > sends[0] or checks[1] > sends[1] or checks[1] < sends[0]):
    fail("JcReadOnlyFluidNc.cpp: each experimental send must check jc_jog_line_allowed() / jc_command_bytes_allowed() before its ::send")
if "0x85" not in live_fn:
    fail("JcReadOnlyFluidNc.cpp: the live-jog send does not special-case only the jog-cancel byte 0x85")
uart_ord, uart_live = split_live(uart)
if len(re.findall(r"Serial1\s*\.\s*(?:write|print)\s*\(", uart_ord)) != 3:
    fail("JcUartLink.cpp: expected exactly 3 ordinary Serial1 write sites ('?', query text, newline)")
if len(re.findall(r"Serial1\s*\.\s*(?:write|print)\s*\(", uart_live)) != 1 or "jc_command_bytes_allowed(" not in uart_live:
    fail("JcUartLink.cpp: expected exactly 1 Serial1 write in the full-control block, behind jc_command_bytes_allowed()")
if "jog" in uart.lower():
    fail("JcUartLink.cpp: the wired link must have no jog-specific path")

# ---- 4. every query write is behind the allowlist ----------------------------------------------
for label, text in (("JcReadOnlyFluidNc.cpp", wifi), ("JcUartLink.cpp", uart)):
    m = re.search(r"::send_query_line\(const char\* line\) \{(.*?)\n\}\n", text, re.S)
    if not m:
        fail(f"{label}: send_query_line not found")
    elif "jc_query_allowed(line)" not in m.group(1)[:200]:
        fail(f"{label}: send_query_line does not check jc_query_allowed() before sending")
# the '?' path stays guarded by the byte rule
if "jc_tx_byte_allowed(byte)" not in wifi or "jc_tx_byte_allowed(byte)" not in uart:
    fail("status-query byte rule jc_tx_byte_allowed() missing from a link")

# ---- 5. the ordinary build has an inert jog session ---------------------------------------------------
app_ord, app_live = split_live(read(os.path.join(SRC, "JcApp.cpp")))
if "_jogs(nullptr, nullptr)" not in app_ord:
    fail("JcApp.cpp: the ordinary build must create its jog session without a transport")
if "jog_tx_thunk" in app_ord:
    fail("JcApp.cpp: the ordinary build must not reference the jog transmit thunk")

# ---- 6. non-motion test mode ----------------------------------------------------------------------------
app_b = split_buckets(read(os.path.join(SRC, "JcApp.cpp")))
pv = app_b["preview"]
if "set_preview(true)" not in pv or pv.find("set_preview(true)") > pv.find("set_sink_ex"):
    fail("JcApp.cpp: the preview block must put the dispatcher in preview mode BEFORE installing the sink")
if "&JcPreviewController::tx" not in pv or "&_preview.controller()" not in pv:
    fail("JcApp.cpp: the preview command system must use the simulated controller as its only transport")
if "&JcCommandSystem::sink" not in pv:
    fail("JcApp.cpp: the preview sink must be the unified command system")
live_app = app_b["live"]
if "cmd_tx_thunk" not in live_app or "&JcCommandSystem::sink" not in live_app:
    fail("JcApp.cpp: the full-control block must install the unified command system with the link thunk")
if "jog_tx_thunk" in pv or "send_jog" in pv:
    fail("JcApp.cpp: the preview block must not reference the real jog transport")
if "_jogs(nullptr, nullptr)" not in app_b["ordinary"]:
    fail("JcApp.cpp: neither-flag builds must keep a jog session without a transport")
for pure in ("JcCommandSystem.hpp", "JcWorkflow.hpp", "JcWorkflowPlans.hpp", "JcSimNc.hpp", "JcCommandPolicy.hpp", "JcHomingState.hpp", "JcProbeCalc.hpp",
             "JcBleUart.hpp", "JcSdBrowser.hpp", "JcJobState.hpp", "JcToolInfo.hpp", "JcTransportManager.hpp", "JcStatusApply.hpp"):
    code_p = "\n".join(l.split("//")[0] for l in read(os.path.join(SRC, pure)).splitlines())
    for word in ("Serial", "WiFi", "Arduino", "::send", "socket", "Serial1", "millis("):
        if word in code_p:
            fail(f"{pure}: the pure command/workflow code must not reference '{word}'")
hdr = read(os.path.join(SRC, "JcJogPreview.hpp"))
code = "\n".join(l.split("//")[0] for l in hdr.splitlines())
for word in ("Serial", "WiFi", "::send", "send_", "JcReadOnlyFluidNc", "JcUartLink", "JcTransport", "Arduino", "socket",
             "write(", "connect(", "send_command", "send_jog"):
    if word in code:
        fail(f"JcJogPreview.hpp: the simulated controller must not reference '{word}'")
for inc in re.findall(r'#include\s+[<"]([^>"]+)[>"]', code):
    if inc not in ("math.h", "stddef.h", "stdint.h", "stdio.h", "string.h", "JcJogTest.hpp", "JcSimNc.hpp"):
        fail(f"JcJogPreview.hpp: unexpected include {inc}")
for name in ("JcReadOnlyFluidNc.cpp", "JcUartLink.cpp"):
    b = split_buckets(read(os.path.join(SRC, name)))
    if re.search(r"::send\s*\(|Serial1\s*\.", b["preview"] + b["ui"]):
        fail(f"{name}: a transmit call sits inside a JC_JOG_PREVIEW / JC_JOG_UI block")
for name in ("JcScreenJog.cpp", "JcScreenJogPreview.cpp", "JcScreen.cpp"):
    text = read(os.path.join(SRC, name))
    if re.search(r"send_jog|send_query_line|send_status|_link\.|sv\.link\.send", text):
        fail(f"{name}: jog UI code refers to a link transmit function")

# ---- 7. BLE UART (Instruction 010): no independent command path -------------------------------------------------
ble_hdr = read(os.path.join(SRC, "JcBleUart.hpp"))
ble_b = split_buckets(ble_hdr)
if "send_command_bytes" in ble_b["ordinary"] or "send_command_bytes" in ble_b["ui"] or "send_command_bytes" in ble_b["preview"]:
    fail("JcBleUart.hpp: send_command_bytes must exist only inside a JC_FULL_CONTROL_TEST block (the ordinary firmware has no BLE command path)")
m = re.search(r"bool send_command_bytes\(const char\* bytes, size_t length\) \{(.*?)\n    \}\n", ble_b["live"], re.S)
if not m or "jc_command_bytes_allowed(bytes, length)" not in m.group(1) or m.group(1).find("jc_command_bytes_allowed") > m.group(1).find("enqueue"):
    fail("JcBleUart.hpp: send_command_bytes must apply jc_command_bytes_allowed() before anything is queued")
m = re.search(r"bool send_query_line\(const char\* line\) \{(.*?)\n    \}\n", ble_hdr, re.S)
if not m or "jc_query_allowed(line)" not in m.group(1)[:120]:
    fail("JcBleUart.hpp: send_query_line must check jc_query_allowed() first")
code_ble = "\n".join(l.split("//")[0] for l in ble_hdr.splitlines())
if len(re.findall(r"\benqueue\(", code_ble)) != 3:      # the definition and exactly the two policy-checked callers
    fail("JcBleUart.hpp: enqueue() must be called only from send_query_line and send_command_bytes")
if len(re.findall(r"\bpriority\(", code_ble)) != 3:     # the definition, the '?' keep-alive and the checked real-time byte
    fail("JcBleUart.hpp: priority() must be called only for the '?' keep-alive and the policy-checked real-time byte")
if len(re.findall(r"_radio->write\(", code_ble)) != 1:
    fail("JcBleUart.hpp: the radio must be written from exactly one place (the paced pump)")
blelink = read(os.path.join(SRC, "JcBleLink.cpp"))
if len(re.findall(r"writeValue\s*\(", blelink)) != 1:
    fail("JcBleLink.cpp: expected exactly one GATT write site")
for name in sorted(os.listdir(SRC)):
    if name.endswith((".cpp", ".hpp")) and name not in ("JcBleUart.hpp", "JcBleLink.cpp", "JcBleLink.hpp"):
        t = "\n".join(l.split("//")[0] for l in read(os.path.join(SRC, name)).splitlines())
        if re.search(r"writeValue\s*\(|_radio->write\(|_ble_radio\.write\(", t):
            fail(f"{name}: a BLE write outside JcBleUart / JcBleLink")
# read-only queries: the SD allowlist names no write operation
q = read(os.path.join(SRC, "JcQuery.hpp"))
q_code = "\n".join(l.split("//")[0] for l in q.splitlines())
for word in ("$SD/Delete", "$SD/Rename", "$SD/Run", "$Localfs", "$LocalFS", "Format", "Remove"):
    if word in q_code:
        fail(f"JcQuery.hpp: the read-only allowlist must not contain {word}")
# the SD run string exists only where commands are built / checked and in the simulator
for name in sorted(os.listdir(SRC)):
    if name.endswith((".cpp", ".hpp")) and name not in ("JcCommandBuilder.hpp", "JcCommandPolicy.hpp", "JcSimNc.hpp"):
        t = "\n".join(l.split("//")[0] for l in read(os.path.join(SRC, name)).splitlines())
        if '"$SD/Run' in t:
            fail(f"{name}: builds a $SD/Run line outside the command builder")
# no M6 / T word anywhere in the command builder or the policy (no automatic tool changer is assumed)
for name in ("JcCommandBuilder.hpp", "JcCommandPolicy.hpp"):
    t = "\n".join(l.split("//")[0] for l in read(os.path.join(SRC, name)).splitlines())
    if re.search(r'"M6|"T%|"T[0-9]', t):
        fail(f"{name}: an M6 / T tool-change word must never be generated")

print("check_no_sink:", "FAIL" if failures else "PASS")
sys.exit(1 if failures else 0)
