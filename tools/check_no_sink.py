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
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "jc8048w550")
LIVE_ENV = "env:jc8048w550_livejog_test"
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL " + msg)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def split_live(text):
    """Returns (ordinary_text, live_text): lines inside '#if JC_LIVE_JOG_TEST' (the
    true branch) go to live_text, everything else to ordinary_text."""
    ordinary, live = [], []
    stack = []  # items: ["live", branch_is_live] or ["other", None]
    for line in text.splitlines():
        s = line.strip()
        is_directive = s.startswith("#")
        if is_directive and re.match(r"#\s*if(def|ndef)?\b", s):
            if "JC_LIVE_JOG_TEST" in s:
                negative = "!" in s or s.startswith("#ifndef") or re.match(r"#\s*ifndef", s) is not None
                stack.append(["live", not negative])
            else:
                stack.append(["other", None])
            ordinary.append("")
            live.append("")
            continue
        if is_directive and re.match(r"#\s*else\b", s) and stack:
            if stack[-1][0] == "live":
                stack[-1][1] = not stack[-1][1]
            ordinary.append("")
            live.append("")
            continue
        if is_directive and re.match(r"#\s*endif\b", s) and stack:
            stack.pop()
            ordinary.append("")
            live.append("")
            continue
        in_live = any(kind == "live" and branch for kind, branch in stack)
        ordinary.append("" if in_live else line)
        live.append(line if in_live else "")
    return "\n".join(ordinary), "\n".join(live)


# ---- 1. policy macros -------------------------------------------------------------
ini = read(os.path.join(ROOT, "platformio.ini"))
section = None
live_flags = {}
for raw in ini.splitlines():
    line = raw.strip()
    m = re.match(r"\[(.+)\]", line)
    if m:
        section = m.group(1)
        continue
    for macro in ("JC_MACHINE_CONTROL_ENABLED", "JC_LIVE_JOG_TEST"):
        mm = re.search(r"-D\s*" + macro + r"\s*=?\s*(\S*)", line)
        if mm:
            value = mm.group(1).strip("'\"") or "1"
            if section == LIVE_ENV:
                live_flags[macro] = value
            elif value != "0":
                fail(f"platformio.ini [{section}]: {macro}={value} outside [{LIVE_ENV}]")
            else:
                pass
        elif macro in line and not line.startswith(";") and "-D" not in line:
            if section != LIVE_ENV:
                fail(f"platformio.ini [{section}]: unexpected reference to {macro}")
for macro in ("JC_MACHINE_CONTROL_ENABLED", "JC_LIVE_JOG_TEST"):
    if live_flags.get(macro) not in (None, "1"):
        fail(f"platformio.ini [{LIVE_ENV}]: {macro} must be 1 (got {live_flags.get(macro)!r})")
if bool(live_flags.get("JC_MACHINE_CONTROL_ENABLED")) != bool(live_flags.get("JC_LIVE_JOG_TEST")):
    fail(f"platformio.ini [{LIVE_ENV}]: both macros must be set together")
dm = re.search(r"default_envs\s*=\s*(.+)", ini)
if dm and "livejog" in dm.group(1):
    fail("platformio.ini: the live-jog environment must not be a default environment")

for name in sorted(os.listdir(SRC)):
    if not name.endswith((".cpp", ".hpp", ".h")):
        continue
    text = read(os.path.join(SRC, name))
    for macro in ("JC_MACHINE_CONTROL_ENABLED", "JC_LIVE_JOG_TEST"):
        for m in re.finditer(r"#\s*define\s+" + macro + r"\s+(\S+)", text):
            if m.group(1) != "0":
                fail(f"{name}: defines {macro} as {m.group(1)}")

# ---- 2. no sink in the ordinary text of firmware translation units ----------------------
SINK_WORDS = ("set_sink_ex", "JcCommandLink", "JcCommandSink", "JcCommandGate(", "JcJogSession::sink",
              "send_jog_bytes", "jog_tx_thunk", "send_jog(")
ordinary_cpp = {}
live_cpp = {}
for name in sorted(os.listdir(SRC)):
    if name.endswith(".cpp"):
        o, l = split_live(read(os.path.join(SRC, name)))
        ordinary_cpp[name], live_cpp[name] = o, l
        for word in SINK_WORDS:
            if word in o:
                fail(f"{name}: references {word} outside a JC_LIVE_JOG_TEST block (ordinary firmware must not install a sink)")
sink_sites = [n for n, t in live_cpp.items() if "set_sink_ex" in t]
if sink_sites != ["JcApp.cpp"]:
    fail(f"the command sink must be installed only in JcApp.cpp inside a live-jog block (found in {sink_sites})")

# headers: the sink wiring (JcApp.hpp) and the transmit method (link header) are live-only too
for name in ("JcApp.hpp", "JcReadOnlyFluidNc.hpp"):
    o, _ = split_live(read(os.path.join(SRC, name)))
    for word in ("jog_tx_thunk", "send_jog", "send_jog_bytes"):
        if word in o:
            fail(f"{name}: declares {word} outside a JC_LIVE_JOG_TEST block")

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
if len(re.findall(r"::send\s*\(", wifi_live)) != 1:
    fail("JcReadOnlyFluidNc.cpp: expected exactly 1 ::send call site inside the live-jog block")
live_fn = wifi_live
send_call = re.search(r"::send\s*\(", live_fn)
if "jc_jog_line_allowed(" not in live_fn or send_call is None or live_fn.find("jc_jog_line_allowed(") > send_call.start():
    fail("JcReadOnlyFluidNc.cpp: the live-jog send does not check jc_jog_line_allowed() before ::send")
if "0x85" not in live_fn:
    fail("JcReadOnlyFluidNc.cpp: the live-jog send does not special-case only the jog-cancel byte 0x85")
if len(re.findall(r"Serial1\s*\.\s*(?:write|print)\s*\(", uart)) != 3:
    fail("JcUartLink.cpp: expected exactly 3 Serial1 write sites ('?', query text, newline)")
if "jog" in uart.lower():
    fail("JcUartLink.cpp: the wired link must have no jog path in this build")

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

print("check_no_sink:", "FAIL" if failures else "PASS")
sys.exit(1 if failures else 0)
