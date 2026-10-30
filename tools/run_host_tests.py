#!/usr/bin/env python3
"""Build and run the host unit tests with any available C++ compiler.

Compilers tried: $CXX, g++, clang++, then `python -m ziglang c++`.
Runs two configurations: the shipping policy (machine control disabled) and a
test-only build with the authorization rules compiled in. The first-live-jog
tests (test_jc_jog.cpp) run in three: shipping, rules compiled in, and the
experimental live-jog policy (JC_LIVE_JOG_TEST=1). The other suites are not run
with JC_LIVE_JOG_TEST because that policy deliberately authorizes only jogs.
"""
import os
import shutil
import subprocess
import sys
import tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sources = [os.path.join(root, "test", "host", n) for n in ("test_jc_core.cpp", "test_jc_app.cpp", "test_jc_control.cpp", "test_jc_conn.cpp", "test_jc_capsflow.cpp")]


def find_cxx():
    if os.environ.get("CXX"):
        return os.environ["CXX"].split()
    for name in ("g++", "clang++"):
        if shutil.which(name):
            return [name]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        sys.exit("No C++ compiler found (set CXX, install g++/clang++, or pip install ziglang)")


cxx = find_cxx()
rc = 0
with tempfile.TemporaryDirectory() as tmp:
    for src in sources:
        for enabled in (0, 1):
            exe = os.path.join(tmp, f"t{os.path.basename(src)}{enabled}.exe")
            cmd = cxx + ["-std=c++17", "-Wall", "-Wextra", f"-DJC_MACHINE_CONTROL_ENABLED={enabled}", src, "-o", exe]
            subprocess.run(cmd, check=True)
            rc |= subprocess.run([exe]).returncode
    jog = os.path.join(root, "test", "host", "test_jc_jog.cpp")
    for enabled, live in ((0, 0), (1, 0), (1, 1)):
        exe = os.path.join(tmp, f"tjog{enabled}{live}.exe")
        cmd = cxx + ["-std=c++17", "-Wall", "-Wextra", f"-DJC_MACHINE_CONTROL_ENABLED={enabled}",
                     f"-DJC_LIVE_JOG_TEST={live}", jog, "-o", exe]
        subprocess.run(cmd, check=True)
        rc |= subprocess.run([exe]).returncode
    # Unified command interface and guided routines (Instructions 009 / 009A): the controller-authoritative policy.
    for name in ("test_jc_commands.cpp", "test_jc_workflow.cpp"):
        exe = os.path.join(tmp, "t" + name + ".exe")
        subprocess.run(cxx + ["-std=c++17", "-Wall", "-Wextra", "-DJC_MACHINE_CONTROL_ENABLED=1", "-DJC_FULL_CONTROL_TEST=1",
                              os.path.join(root, "test", "host", name), "-o", exe], check=True)
        rc |= subprocess.run([exe]).returncode
    # Non-motion jog test mode (Instruction 007): the real gate against the simulated controller.
    prev = os.path.join(root, "test", "host", "test_jc_preview.cpp")
    # The preview build (JC_JOG_PREVIEW=1, machine control disabled) and the full-control build run the same rules.
    for tag, flags in (("preview", ["-DJC_MACHINE_CONTROL_ENABLED=0", "-DJC_JOG_PREVIEW=1"]),
                       ("full", ["-DJC_MACHINE_CONTROL_ENABLED=1", "-DJC_FULL_CONTROL_TEST=1"])):
        exe = os.path.join(tmp, f"tpreview_{tag}.exe")
        subprocess.run(cxx + ["-std=c++17", "-Wall", "-Wextra"] + flags + [prev, "-o", exe], check=True)
        rc |= subprocess.run([exe]).returncode
rc |= subprocess.run([sys.executable, os.path.join(root, "tools", "check_no_sink.py")]).returncode
# Renders the real page code on the host (ordinary and live-jog builds) and checks the layout.
rc |= subprocess.run([sys.executable, os.path.join(root, "tools", "run_ui_preview.py")]).returncode
sys.exit(rc)
