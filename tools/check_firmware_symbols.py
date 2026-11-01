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
print("ordinary: send_command_bytes", "ABSENT" if ordinary is not None and "send_command_bytes" not in ordinary else "PRESENT/unknown")
print("full-control: Wi-Fi send_command_bytes", "present" if full and "JcReadOnlyFluidNc::send_command_bytes" in full else "MISSING",
      "| UART send_command_bytes", "present" if full and "JcUartLink::send_command_bytes" in full else "MISSING")
for m in fail:
    print("FAIL:", m)
print("check_firmware_symbols:", "FAIL" if fail else "PASS")
sys.exit(1 if fail else 0)
