#!/usr/bin/env python3
"""Guard: the production (full-control) firmware shows no commissioning / test wording on its screens.

Preprocesses every screen source with the production defines (so the #if branches of the other builds are gone)
and looks for banned phrases in the string literals that remain. Serial log lines are not on the screen and are
ignored. Exits 1 on any hit.

Usage: python tools/check_production_wording.py
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "jc8048w550")
HOST = os.path.join(ROOT, "test", "host")

BANNED = [
    "FULL CONTROL TEST BUILD", "LIVE JOG TEST BUILD", "TEST BUILD", "ARM JOG", "ARM TEST", "ARM PREVIEW", "ARMED",
    "UNPROTECTED", "RESTRICTED", "CONFIRMED COMMANDS", "ONLY RESTRICTED", "PREVIEW ONLY", "MOTION DISABLED",
    "FULL CONTROL", "COMMANDS ARE SENT",
]
SCREEN_EXTRA = ["JcScreens.hpp", "JcScreen.hpp", "JcUiKit.hpp"]
FILES = [n for n in sorted(os.listdir(SRC)) if n.startswith("JcScreen") and n.endswith(".cpp")] + ["JcPendantUi.cpp", "JcUiKit.cpp"]


SCREEN_FILES = set(FILES + SCREEN_EXTRA)


def find_cxx():
    if os.environ.get("CXX"):
        return os.environ["CXX"].split()
    for n in ("g++", "clang++"):
        if shutil.which(n):
            return [n]
    import ziglang  # noqa: F401
    return [sys.executable, "-m", "ziglang", "c++"]


def preprocess(cxx, name, stubs):
    """Runs the preprocessor. An SDK header that is not available on the host (WiFi.h ...) is replaced by an empty
    stub, which is enough for macro expansion and #if evaluation."""
    out = None
    for _ in range(16):
        cmd = cxx + ["-std=c++17", "-E", "-DJC_MACHINE_CONTROL_ENABLED=1", "-DJC_FULL_CONTROL_TEST=1",
                     "-I", os.path.join(HOST, "ui_stubs"), "-I", stubs, "-I", SRC, os.path.join(SRC, name)]
        out = subprocess.run(cmd, capture_output=True, text=True)
        if out.returncode == 0:
            return out
        m = re.search(r"'([^']+)' file not found", out.stderr)
        if not m:
            return out
        path = os.path.join(stubs, m.group(1))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w").close()
    return out


def main():
    cxx = find_cxx()
    stubs = tempfile.mkdtemp(prefix="jcwording")
    hits = []
    for name in FILES:
        out = preprocess(cxx, name, stubs)
        if out.returncode != 0:
            print("preprocess failed for %s:\n%s" % (name, out.stderr[-400:]))
            return 2
        current = ""
        for line in out.stdout.splitlines():
            marker = re.match(r'# \d+ "([^"]+)"', line)
            if marker:
                current = os.path.basename(marker.group(1).replace("\\", "/"))
                continue
            # Screen code only: the page sources and the headers that define screens. Logic headers (command gate,
            # jog session) carry texts for the restricted builds that no production screen shows.
            if current not in SCREEN_FILES:
                continue
            if "Serial." in line:
                continue
            for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
                for b in BANNED:
                    if b in lit:
                        hits.append((name, b, lit))
    seen = []
    for h in hits:
        if h not in seen:
            seen.append(h)
            print("BANNED  %-26s %-24s %s" % h)
    print("production wording: %d banned string literal(s) on screen code" % len(seen))
    return 1 if seen else 0


if __name__ == "__main__":
    sys.exit(main())
