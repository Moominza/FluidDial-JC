#!/usr/bin/env python3
"""Static guard for the JC firmware: machine control must stay disabled and no
transmit sink may be installed.

Checks (exit status 1 on any failure):
  1. JC_MACHINE_CONTROL_ENABLED is not turned on in platformio.ini or any source.
  2. No firmware .cpp installs or builds a command sink (set_sink_ex, JcCommandLink,
     JcCommandSink); those exist only in headers and host tests.
  3. Raw transmit calls (::send, Serial1.) appear only in the two read-only links.
  4. Those links route every non-'?' write through jc_query_allowed().
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "jc8048w550")
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL " + msg)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# 1. the policy macro is never switched on
for rel in ("platformio.ini",):
    text = read(os.path.join(ROOT, rel))
    for m in re.finditer(r"JC_MACHINE_CONTROL_ENABLED\s*=?\s*(\S*)", text):
        if m.group(1).strip("'\"") not in ("0", ""):
            fail(f"{rel}: JC_MACHINE_CONTROL_ENABLED set to {m.group(1)!r}")
for name in sorted(os.listdir(SRC)):
    if not name.endswith((".cpp", ".hpp", ".h")):
        continue
    for m in re.finditer(r"#\s*define\s+JC_MACHINE_CONTROL_ENABLED\s+(\S+)", read(os.path.join(SRC, name))):
        if m.group(1) != "0":
            fail(f"{name}: defines JC_MACHINE_CONTROL_ENABLED as {m.group(1)}")

# 2. no sink in firmware translation units
SINK_WORDS = ("set_sink_ex", "JcCommandLink", "JcCommandSink", "JcCommandGate(")
for name in sorted(os.listdir(SRC)):
    if name.endswith(".cpp"):
        text = read(os.path.join(SRC, name))
        for word in SINK_WORDS:
            if word in text:
                fail(f"{name}: references {word} (firmware must not install a command sink)")

# 3. raw transmit sites only in the read-only links
ALLOWED_TX = {"JcReadOnlyFluidNc.cpp", "JcUartLink.cpp"}
TX_PATTERN = re.compile(r"::send\s*\(|Serial1\s*\.\s*(write|print|println)\s*\(|Serial2\s*\.")
for name in sorted(os.listdir(SRC)):
    if name.endswith(".cpp") and name not in ALLOWED_TX and TX_PATTERN.search(read(os.path.join(SRC, name))):
        fail(f"{name}: contains a raw transmit call outside the read-only links")

# 4. every query write is behind the allowlist
wifi = read(os.path.join(SRC, "JcReadOnlyFluidNc.cpp"))
uart = read(os.path.join(SRC, "JcUartLink.cpp"))
for label, text, fn in (("JcReadOnlyFluidNc.cpp", wifi, "send_query_line"), ("JcUartLink.cpp", uart, "send_query_line")):
    m = re.search(r"::" + fn + r"\(const char\* line\) \{(.*?)\n\}\n", text, re.S)
    if not m:
        fail(f"{label}: {fn} not found")
    elif "jc_query_allowed(line)" not in m.group(1).split("send", 1)[0] and "jc_query_allowed(line)" not in m.group(1)[:200]:
        fail(f"{label}: {fn} does not check jc_query_allowed() before sending")
# the '?' path stays guarded by the byte rule
if "jc_tx_byte_allowed(byte)" not in wifi or "jc_tx_byte_allowed(byte)" not in uart:
    fail("status-query byte rule jc_tx_byte_allowed() missing from a link")
# exactly the expected number of ::send / Serial1 write sites
if len(re.findall(r"::send\s*\(", wifi)) != 2:
    fail("JcReadOnlyFluidNc.cpp: expected exactly 2 ::send call sites ('?' and the allowlisted query)")
if len(re.findall(r"Serial1\s*\.\s*(?:write|print)\s*\(", uart)) != 3:
    fail("JcUartLink.cpp: expected exactly 3 Serial1 write sites ('?', query text, newline)")

print("check_no_sink:", "FAIL" if failures else "PASS")
sys.exit(1 if failures else 0)
