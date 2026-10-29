#!/usr/bin/env python3
"""Host-side integration test: real TCP <-> simulated FluidNC <-> real JC logic.

What is REAL here: the TCP session with tools/sim_fluidnc_server.py, and the
JC C++ frame assembler, status parser, machine model and link tracker
(test/host/jc_link_tool.cpp). What is NOT: this script stands in for the
ESP32 socket layer (it opens the connection, sends '?', detects closure), so
the Wi-Fi / lwIP firmware itself is NOT exercised. See docs/development.

Usage: python tools/run_sim_integration.py
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "tools", "sim_fluidnc_server.py")
TOOL_SRC = os.path.join(ROOT, "test", "host", "jc_link_tool.cpp")
PORT = 23823

failures = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        failures.append(msg)


def find_cxx():
    if os.environ.get("CXX"):
        return os.environ["CXX"].split()
    for n in ("g++", "clang++"):
        if shutil.which(n):
            return [n]
    import ziglang  # noqa: F401
    return [sys.executable, "-m", "ziglang", "c++"]


class Tool:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(self, line, reply=False):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip() if reply else None

    def close(self):
        self.p.stdin.close()
        self.p.wait()


class Sim:
    def __init__(self, scenario):
        self.p = subprocess.Popen([sys.executable, SIM, "--scenario", scenario, "--port", str(PORT), "--bind", "127.0.0.1"],
                                  stdout=subprocess.PIPE, text=True)
        self.p.stdout.readline()  # "listening"
        time.sleep(0.2)

    def stop(self):
        self.p.terminate()
        try:
            out = self.p.communicate(timeout=3)[0]
        except subprocess.TimeoutExpired:
            self.p.kill()
            out = ""
        return out


def session(tool, clock, queries, sent_log, per_query_wait=0.25, drain_slow=False):
    """One TCP session: send '?' `queries` times, forward bytes to the tool."""
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
    except OSError:
        return "refused"
    tool.cmd(f"T {clock[0]}")
    tool.cmd("CONNECT")
    s.settimeout(per_query_wait)
    closed = False
    for _ in range(queries):
        try:
            s.send(b"?")
            sent_log.append(b"?")
        except OSError:
            closed = True
            break
        deadline = time.time() + per_query_wait
        while time.time() < deadline:
            try:
                data = s.recv(256)
            except socket.timeout:
                break
            except OSError:
                data = b""
            if not data:
                closed = True
                break
            clock[0] += 200
            tool.cmd(f"T {clock[0]}")
            tool.cmd("DATA " + data.hex())
        if closed:
            break
    # Drain: keep reading until the stream has been quiet for a while.
    quiet_until = time.time() + (1.5 if drain_slow else 0.3)
    while not closed and time.time() < quiet_until:
        try:
            data = s.recv(256)
        except socket.timeout:
            continue
        except OSError:
            data = b""
        if not data:
            closed = True
            break
        quiet_until = time.time() + (1.5 if drain_slow else 0.3)
        clock[0] += 200
        tool.cmd(f"T {clock[0]}")
        tool.cmd("DATA " + data.hex())
    if closed:
        tool.cmd(f"T {clock[0]}")
        tool.cmd("CLOSE")
    s.close()
    return "closed" if closed else "open"


def fields(q):
    return dict(kv.split("=", 1) for kv in q.split())


def scenario_run(exe, name, queries, verify):
    print(f"[{name}]")
    sim = Sim(name)
    tool = Tool(exe)
    clock = [1000]
    sent = []
    result = session(tool, clock, queries, sent, per_query_wait=0.2, drain_slow=(name == "fragmented"))
    clock[0] += 100
    tool.cmd(f"T {clock[0]}")
    q = fields(tool.cmd("Q", reply=True))
    verify(q, tool, clock, result)
    check(all(b == b"?" for b in sent), "client transmitted only '?' bytes")
    tool.close()
    out = sim.stop()
    check("VIOLATION" not in out, "simulator audit: no non-'?' byte received")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "jc_link_tool.exe")
        subprocess.run(find_cxx() + ["-std=c++17", TOOL_SRC, "-o", exe], check=True,
                       stderr=subprocess.DEVNULL)

        def normal(q, tool, clock, result):
            check(int(q["frames"]) == 3 and q["invalid"] == "0", "3 valid four-axis frames, 0 errors")
            check(q["state"] == "Connected" and q["fresh"] == "1", "link Connected and fresh")
            check(q["M"] == "-250.000,-0.001,12.000,-359.990", "X/Y/Z mm and A degrees (negative, decimals) exact")
            check(q["W"] == "n/a", "work position NOT invented when no WCO has been reported")
            check(q["machine"] == "Idle", "machine state Idle")

        scenario_run(exe, "normal", 3, normal)

        def work(q, tool, clock, result):
            check(q["state"] == "Connected" and int(q["frames"]) == 2, "work-position frames accepted")
            check(q["W"] == "-1.500,2.250,0.000,-10.000", "WPos values exact")
            check(q["M"] == "8.500,22.250,0.000,-10.000", "machine = work + WCO(10,20,0,0), A unchanged")
            check(q["machine"] == "Jog", "state Jog")

        scenario_run(exe, "work", 2, work)

        def fragmented(q, tool, clock, result):
            check(int(q["frames"]) == 3 and q["invalid"] == "0", "fragmented 2-byte segments reassembled (3 frames)")
            check(q["M"] == "-250.000,-0.001,12.000,-359.990", "fragmented frames parse to exact values")

        scenario_run(exe, "fragmented", 3, fragmented)

        def malformed(q, tool, clock, result):
            check(int(q["frames"]) == 6, "6 good frames accepted among bad ones")
            check(int(q["invalid"]) == 6, "6 malformed frames rejected and counted (incl. one cut by CRLF)")
            check(q["M"] == "0.000,0.000,0.000,0.000", "bad frames never altered the displayed position")

        scenario_run(exe, "malformed", 12, malformed)

        def three_axis(q, tool, clock, result):
            check(int(q["frames"]) == 0 and int(q["invalid"]) == 3, "3-axis frames rejected (4 axes required)")
            check(q["state"] != "Connected" and q["fresh"] == "0", "never presented as live data")

        scenario_run(exe, "three-axis", 3, three_axis)

        def close(q, tool, clock, result):
            check(result == "closed", "device observed unexpected closure")
            check(int(q["lost"]) == 1 and q["state"] == "Lost" and q["fresh"] == "0", "state Lost, data not fresh")
            check(q["M"] != "n/a", "last values retained (shown stale, not cleared)")
            # Reconnect: retry is only due after the retry delay.
            check(tool.cmd("RETRY", reply=True) == "0", "no immediate reconnect storm")
            clock[0] += 2100
            tool.cmd(f"T {clock[0]}")
            check(tool.cmd("RETRY", reply=True) == "1", "reconnect attempt due after delay")
            sent = []
            sim2_result = session(tool, clock, 2, sent)
            clock[0] += 100
            tool.cmd(f"T {clock[0]}")
            q2 = fields(tool.cmd("Q", reply=True))
            check(q2["state"] == "Connected" and q2["fresh"] == "1", "Wi-Fi-style reconnect: Connected again with fresh data")
            check(int(q2["lost"]) == 1, "loss counter preserved")

        scenario_run(exe, "close", 6, close)

        def silent(q, tool, clock, result):
            check(int(q["frames"]) == 0 and q["fresh"] == "0", "silent controller: no data, never fresh")
            check(q["state"] == "Connecting", "state stays Connecting (no frames)")

        scenario_run(exe, "silent", 3, silent)

        # Stale handling on a live link.
        print("[stale]")
        tool = Tool(exe)
        tool.cmd("T 1000"); tool.cmd("CONNECT")
        tool.cmd("T 1100")
        tool.cmd("DATA " + b"<Idle|MPos:1.000,2.000,3.000,4.000|FS:0,0>\r\n".hex())
        check(fields(tool.cmd("T 2000") or tool.cmd("Q", reply=True))["fresh"] == "1", "fresh within 3 s")
        tool.cmd("T 4200")
        q = fields(tool.cmd("Q", reply=True))
        check(q["state"] == "Stale" and q["fresh"] == "0", "stale after 3 s without frames")
        check(q["M"] == "1.000,2.000,3.000,4.000", "stale values retained for display as stale")
        # Transmit restrictions (rule shared with firmware)
        check(tool.cmd("TX 3f", reply=True) == "1", "'?' permitted")
        for label, hx in (("jog", "244a3d"), ("home", "2448"), ("hold", "21"), ("resume", "7e"),
                          ("reset", "18"), ("jog-cancel", "85"), ("unlock", "2458"), ("G-code", "4730")):
            check(tool.cmd("TX " + hx, reply=True) == "0", f"{label} bytes blocked")
        tool.close()

    print("\nRESULT:", "FAIL (%d)" % len(failures) if failures else "PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
