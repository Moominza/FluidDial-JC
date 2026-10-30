#!/usr/bin/env python3
"""Render the real JC pages on the host and check their layout (Instructions 006B / 006C).

Builds test/host/ui_render.cpp together with the real page code and fonts, against host stubs
of the display and Arduino (test/host/ui_stubs), for BOTH policies:
  ordinary  JC_MACHINE_CONTROL_ENABLED=0
  live      JC_MACHINE_CONTROL_ENABLED=1 JC_LIVE_JOG_TEST=1   (the experimental build's UI)
runs the simulated scenarios, fails on any layout problem (text outside the screen, text over
other text, text wiped by a later fill, dialog text leaving its box or touching a button), and
with --png DIR writes PNG screenshots (standard library only).

This renders the page CODE, not the panel: it cannot show colour accuracy, touch response or
what the physical display looks like. Operator photographs remain the real visual check.

Usage: python tools/run_ui_preview.py [--png DIR]
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "jc8048w550")
HOST = os.path.join(ROOT, "test", "host")
SOURCES = [os.path.join(HOST, "ui_render.cpp")] + [
    os.path.join(SRC, n) for n in (
        "JcFontRenderer.cpp", "JcUiKit.cpp", "JcScreen.cpp", "JcScreenUtil.cpp", "JcScreenJog.cpp",
        "JcScreenLimits.cpp", "JcScreenCapabilities.cpp", "JcScreenMenu.cpp", "JcScreenSettings.cpp",
        "JcScreenDryRun.cpp", "JcScreenJogPreview.cpp")]


def find_cxx():
    if os.environ.get("CXX"):
        return os.environ["CXX"].split()
    for n in ("g++", "clang++"):
        if shutil.which(n):
            return [n]
    import ziglang  # noqa: F401
    return [sys.executable, "-m", "ziglang", "c++"]


def ppm_to_png(ppm, png):
    with open(ppm, "rb") as f:
        data = f.read()
    # P6\nW H\n255\n
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pixels = parts[3]
    raw = b"".join(b"\x00" + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    png_dir = None
    if "--png" in sys.argv:
        png_dir = os.path.abspath(sys.argv[sys.argv.index("--png") + 1])
        os.makedirs(png_dir, exist_ok=True)
    rc = 0
    with tempfile.TemporaryDirectory() as tmp:
        for tag, flags in (("ordinary", ["-DJC_MACHINE_CONTROL_ENABLED=0", "-DJC_LIVE_JOG_TEST=0"]),
                           ("preview", ["-DJC_MACHINE_CONTROL_ENABLED=0", "-DJC_LIVE_JOG_TEST=0", "-DJC_JOG_PREVIEW=1"]),
                           ("live", ["-DJC_MACHINE_CONTROL_ENABLED=1", "-DJC_LIVE_JOG_TEST=1"])):
            exe = os.path.join(tmp, f"ui_{tag}.exe")
            cmd = find_cxx() + ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
                                "-I", os.path.join(HOST, "ui_stubs")] + flags + SOURCES + ["-o", exe]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"[{tag}] COMPILE FAILED\n{r.stderr[-3000:]}")
                return 1
            out = os.path.join(tmp, f"frames_{tag}")
            os.makedirs(out)
            run = subprocess.run([exe, out], capture_output=True, text=True)
            print(run.stdout, end="")
            if run.stderr:
                print(run.stderr, end="")
            rc |= run.returncode
            if png_dir:
                for name in sorted(os.listdir(out)):
                    if name.endswith(".ppm"):
                        ppm_to_png(os.path.join(out, name), os.path.join(png_dir, name[:-4] + ".png"))
        if png_dir:
            print("PNG screenshots written to", png_dir)
    print("RESULT:", "PASS" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
