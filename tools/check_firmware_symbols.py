#!/usr/bin/env python3
"""Symbol-level proof of the transmit path in the BUILT firmware (Instruction 013).

Run after `pio run` for the environments. For each ELF it lists whether the machine-command transmit functions exist:
  * ordinary image (jc8048w550):                    NO send_command_bytes anywhere (no machine-changing sink)
  * live-jog test image:                            checked only for the absence of the full-control string
  * full-control image (jc8048w550_fullcontrol_test): Wi-Fi AND UART send_command_bytes present, all behind the shared
                                                    jc_command_bytes_allowed() policy
Also greps the binaries for program-start strings, which must never be present. Exit status 1 on any violation.
"""
import os
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
home = os.path.expanduser("~")
nm = os.path.join(home, ".platformio", "packages", "toolchain-xtensa-esp32s3", "bin", "xtensa-esp32s3-elf-nm")
if os.name == "nt":
    nm += ".exe"
fail = []


def symbols(env):
    elf = os.path.join(root, ".pio", "build", env, "firmware.elf")
    if not os.path.exists(elf):
        fail.append(f"{env}: firmware.elf missing (build it first)")
        return None
    return subprocess.run([nm, "-C", elf], capture_output=True, text=True, check=True).stdout


def binary(env):
    with open(os.path.join(root, ".pio", "build", env, "firmware.bin"), "rb") as f:
        return f.read()


ordinary = symbols("jc8048w550")
full = symbols("jc8048w550_fullcontrol_test")
if ordinary is not None:
    if "send_command_bytes" in ordinary:
        fail.append("ordinary image contains a machine-command transmit function")
    if "jc_command_bytes_allowed" not in ordinary:
        print("note: ordinary image has no policy symbol (inlined)")
if full is not None:
    for need in ("JcReadOnlyFluidNc::send_command_bytes", "JcUartLink::send_command_bytes", "JcCommandSystem::checked_tx", "jc_command_bytes_allowed"):
        if need not in full:
            fail.append(f"full-control image lacks {need}")
for env in ("jc8048w550", "jc8048w550_fullcontrol_test", "jc8048w550_livejog_test"):
    try:
        data = binary(env)
    except OSError:
        fail.append(f"{env}: firmware.bin missing")
        continue
    for bad in (b"SD/Run", b"Localfs/Run", b"SD/Resume"):
        if bad in data:
            fail.append(f"{env}: contains the program-start string {bad.decode()}")
    banner = b"FULL CONTROL TEST BUILD" in data
    print(f"{env}: full-control banner={'yes' if banner else 'no'}")
    if env != "jc8048w550_fullcontrol_test" and banner:
        fail.append(f"{env}: carries the full-control banner")
    if env == "jc8048w550_fullcontrol_test" and not banner:
        fail.append("full-control image lacks its banner")
# Wi-Fi OTA (Instruction 020): every JC image carries the image-target marker and the OTA service; the M5Dial / CYD images
# carry neither (OTA is isolated to the JC targets), so an OTA image of theirs can never pass the JC target check.
MARKER = b"FDJC-IMAGE-TARGET:JC8048W550C_I:ESP32-S3:V1"
for env in ("jc8048w550", "jc8048w550_fullcontrol_test", "jc8048w550_livejog_test"):
    try:
        data = binary(env)
    except OSError:
        continue
    if MARKER not in data:
        fail.append(f"{env}: lacks the OTA image-target marker")
    else:
        # The pendant checks the marker when the last byte arrives, but the final partial 4 KB block is only
        # flushed afterwards, so the marker must lie inside the whole-block prefix (with margin) of the image.
        off = data.index(MARKER)
        prefix = len(data) - (len(data) % 4096)
        if off + len(MARKER) + 64 > prefix:
            fail.append(f"{env}: OTA marker at {off} lies in the final partial flash block (image {len(data)} bytes)")
    if b"ArduinoOTA" not in data and b"OTA server at" not in data:
        fail.append(f"{env}: lacks the OTA service")
    if b"verifyRollbackLater" not in data and b"cancel_rollback" not in data and b"rollback" not in data.lower():
        print(f"note: {env}: no rollback string found")
for env in ("m5dial", "cyddial"):
    try:
        data = binary(env)
    except OSError:
        continue
    if MARKER in data or b"OTA server at" in data:
        fail.append(f"{env}: contains JC OTA code (OTA must stay isolated to the JC targets)")
print("OTA: marker present in the JC images, absent from m5dial/cyddial" if not [m for m in fail if "OTA" in m or "marker" in m] else "OTA: FAILED")
print("ordinary: send_command_bytes", "ABSENT" if ordinary is not None and "send_command_bytes" not in ordinary else "PRESENT/unknown")
print("full-control: Wi-Fi send_command_bytes", "present" if full and "JcReadOnlyFluidNc::send_command_bytes" in full else "MISSING",
      "| UART send_command_bytes", "present" if full and "JcUartLink::send_command_bytes" in full else "MISSING")
for m in fail:
    print("FAIL:", m)
print("check_firmware_symbols:", "FAIL" if fail else "PASS")
sys.exit(1 if fail else 0)
