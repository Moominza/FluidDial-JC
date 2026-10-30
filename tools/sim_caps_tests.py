"""Capability-read scenarios for tools/run_sim_integration.py.

REAL: the TCP session with tools/sim_fluidnc_server.py and the JC C++ stream
splitter, response classifier, command executor and capability loader
(test/host/jc_link_tool.cpp). NOT real: the ESP32 socket layer (this script
sends the lines the C++ loader asks for, exactly as JcApp::service() does) and
the controller (the simulator reports invented values).
"""
import re
import socket
import time

ALLOWED = [re.compile(p) for p in (
    r"^\$I$", r"^\$G$", r"^\$/probe/pin$", r"^\$/probe/toolsetter_pin$", r"^\$/start/must_home$",
    r"^\$/axes/[xyza]/(steps_per_mm|max_travel_mm|max_rate_mm_per_min|acceleration_mm_per_sec2|"
    r"homing/cycle|homing/positive_direction|soft_limits|motor0/hard_limits|motor0/limit_neg_pin|"
    r"motor0/limit_pos_pin|motor0/limit_all_pin|homing/mpos_mm)$",
)]


def py_allowed(line):
    return any(p.match(line) for p in ALLOWED)


def pump(sock, tool, wait=0.06):
    """Forward whatever the controller sent to the C++ stream logic. -1 = closed."""
    got = 0
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            data = sock.recv(1024)
        except socket.timeout:
            break
        except OSError:
            return -1
        if not data:
            return -1
        got += len(data)
        tool.cmd("DATA " + data.hex())
    return got


def summary(tool):
    return dict(kv.split("=", 1) for kv in tool.cmd("LSUM", reply=True).split())


def session(port, tool, clock, sent, max_loops=600, settle_real=0.0):
    """One connection: get fresh Idle status, operator starts a read, run it to
    completion or failure. The tool's clock follows real time so timeouts behave
    as on a device. Returns (summary, closed, load_reply)."""
    s = socket.create_connection(("127.0.0.1", port), timeout=1)
    s.settimeout(0.05)
    last = [time.time()]

    def advance():
        now = time.time()
        clock[0] += max(1, int((now - last[0]) * 1000))
        last[0] = now

    tool.cmd(f"T {clock[0]}")
    tool.cmd("CONNECT")
    s.send(b"?")
    pump(s, tool, 0.25)
    advance()
    tool.cmd(f"T {clock[0]}")
    load = tool.cmd("LOAD", reply=True)
    closed = False
    last_poll = [time.time()]
    for _ in range(max_loops):
        tool.cmd(f"T {clock[0]}")
        q = tool.cmd("NEXT", reply=True)
        if q != "-":
            try:
                s.sendall((q + "\n").encode())
            except OSError:
                closed = True
                break
            sent.append(q)
            tool.cmd("SENT")
        if time.time() - last_poll[0] >= 0.3:  # like the firmware, poll status at a modest rate
            last_poll[0] = time.time()
            try:
                s.send(b"?")
            except OSError:
                closed = True
                break
        got = pump(s, tool)
        if got < 0:
            closed = True
            break
        advance()
        tool.cmd(f"T {clock[0]}")
        tool.cmd("TICK")
        if summary(tool)["lstate"] in ("Complete", "Failed"):
            break
    if closed:
        tool.cmd(f"T {clock[0]}")
        tool.cmd("CLOSE")
    elif settle_real > 0:
        end = time.time() + settle_real
        while time.time() < end:
            if pump(s, tool, 0.1) < 0:
                break
            advance()
            tool.cmd(f"T {clock[0]}")
            tool.cmd("TICK")
    s.close()
    return summary(tool), closed, load


def run_all(exe, check, Sim, Tool, port):
    ax = lambda q, n: q[f"ax{n}"].split(",")

    def scenario(name, verify, **kw):
        print(f"[{name}]")
        sim = Sim(name)
        tool = Tool(exe)
        clock = [1000]
        sent = []
        q, closed, load = session(port, tool, clock, sent, **kw)
        verify(q, sent, closed, load, tool, clock)
        out = sim.stop()
        check("VIOLATION" not in out, "simulator audit: only allowlisted read-only queries received")
        seen = [ln.split("QUERY ", 1)[1] for ln in out.splitlines() if "QUERY " in ln]
        check(all(py_allowed(x) for x in sent), "every line the device sent matches the independent allowlist")
        check(len(seen) >= len(sent) - 1, "simulator saw the queries the device sent")
        tool.close()
        return out

    # ---- cross-check: C++ allowlist agrees with the independent Python one --------
    print("[allowlist cross-check]")
    tool = Tool(exe)
    probes = ["$I", "$G", "$/probe/pin", "$/probe/toolsetter_pin", "$/axes/x/steps_per_mm",
              "$/axes/a/homing/positive_direction", "$/axes/b/steps_per_mm", "$/axes/x/steps_per_mm=1",
              "$H", "$HX", "$X", "$J=G91G21F100X1", "G0 X1", "G10 L20 P0 X0", "$SD/Run=/a.nc",
              "$Localfs/Run=/m.nc", "$/axes/x", "$Config/Dump", "$$", "$I ", " $I", "$/axes/x/homing/cycle",
              "$/axes/x/soft_limits", "$/axes/y/motor0/hard_limits", "$/axes/z/motor0/limit_pos_pin",
              "$/axes/a/homing/mpos_mm", "$/start/must_home", "$/start/must_home=false",
              "$/axes/x/soft_limits=false", "$/axes/x/motor0/hard_limits=true", "$/axes/x/motor1/hard_limits",
              "$/axes/x/motor0/limit_all_pin=NO_PIN", "$/start/", "$J=G91G21F100X1.000", "$J=G91G21F100X1.000\n"]
    mismatch = [p for p in probes if (tool.cmd("QOK " + p, reply=True) == "1") != py_allowed(p)]
    check(not mismatch, "C++ and Python allowlists agree on %d probe lines %s" % (len(probes), mismatch or ""))
    tool.close()

    # ---- happy path ---------------------------------------------------------------
    def good(q, sent, closed, load, tool, clock):
        check(load == "started", "operator-started read accepted (fresh, Idle)")
        check(q["lstate"] == "Complete" and q["lfail"] == "none", "read completes")
        check(int(q["ok"]) == 53 and int(q["err"]) == 0 and len(sent) == 53, "53 queries sent, 53 answered, none refused")
        check(sent[0] == "$I" and sent[1] == "$G" and sent[-1] == "$/start/must_home", "read starts with $I then $G, ends with must_home")
        check(q["mask"] == "15", "limits confirmed for all four axes")
        check(q["envmask"] == "15", "soft / hard limit settings read for all four axes")
        check(ax(q, 0)[:6] == ["800", "300", "5000", "200", "2", "0"], "X steps/travel/rate/accel/cycle parsed exactly")
        check(ax(q, 2)[:6] == ["800", "100", "2000", "100", "1", "1"], "Z values and +direction parsed")
        check(ax(q, 3)[:6] == ["17.778", "360", "3600", "50", "0", "0"], "A (rotary) values parsed; homing cycle 0")
        # soft, hard, neg pin, pos pin, all pin (1 = assigned), mpos
        check(ax(q, 0)[6:] == ["1", "0", "0", "0", "1", "0"], "X: soft limits ON, hard OFF, only the ALL pin assigned, mpos 0")
        check(ax(q, 2)[6:] == ["1", "0", "0", "1", "0", "-5"], "Z: soft ON, hard OFF, only the + pin assigned, mpos -5")
        check(ax(q, 3)[6] == "0" and ax(q, 3)[7] == "0", "A: soft limits OFF, hard OFF")
        check(q["musthome"] == "1", "start/must_home read as true")
        check(q["wcs"] == "G55" and q["tool"] == "2", "active WCS and tool read from $G")
        check(q["probe"] == "gpio.36:low", "probe pin read")
        check(int(q["lines"]) >= 53 and q["timeouts"] == "0" and q["losses"] == "0", "no timeouts or losses")

    scenario("caps", good)
    scenario("caps-fragmented", good)

    # ---- controller refuses the A axis ---------------------------------------------
    def refuse_a(q, sent, closed, load, tool, clock):
        check(q["lstate"] == "Complete" and int(q["err"]) == 12, "read completes; 12 A-axis queries refused (error:3)")
        check(q["mask"] == "7", "limits confirmed for X Y Z only; A stays a placeholder")
        check(q["envmask"] == "15", "refused soft / hard limit items still count as 'reached' (shown as error, never as OFF)")
        check(ax(q, 3) == ["E"] * 12, "A fields recorded as controller errors, no values invented")
        check(ax(q, 0)[1] == "300", "other axes unaffected")

    scenario("caps-refuse-a", refuse_a)

    # ---- implausible values --------------------------------------------------------
    def bad(q, sent, closed, load, tool, clock):
        check(q["lstate"] == "Complete", "read completes")
        check(ax(q, 0)[0] == "-" and ax(q, 1)[1] == "-" and ax(q, 2)[2] == "-", "nan / negative / text rejected, not clamped")
        check(q["mask"] == "8", "only the axis with clean values (A) is confirmed")

    scenario("caps-bad", bad)

    # ---- silent controller: timeout, no resend --------------------------------------
    def silent(q, sent, closed, load, tool, clock):
        check(q["lstate"] == "Failed" and q["lfail"] == "controller_did_not_reply_(timeout)", "read fails with a timeout")
        check(q["timeouts"] == "1" and len(sent) == 1, "exactly one query sent; never resent")
        check(q["mask"] == "0", "no limits confirmed")
        tool.cmd(f"T {clock[0] + 5000}")
        check(tool.cmd("NEXT", reply=True) == "-", "loader does not retry by itself")

    scenario("caps-silent", silent)

    # ---- alarm during the read -------------------------------------------------------
    def alarm(q, sent, closed, load, tool, clock):
        check(q["lstate"] == "Failed" and q["lfail"] == "controller_alarm_during_read", "ALARM:1 fails the read")
        check(q["mask"] == "0", "partial data never confirms limits")

    scenario("caps-alarm", alarm)

    # ---- connection lost mid-read, reconnect without replay --------------------------
    print("[caps-close]")
    sim = Sim("caps-close")
    tool = Tool(exe)
    clock = [1000]
    sent = []
    q, closed, load = session(port, tool, clock, sent)
    check(closed, "device observed the connection closing mid-read")
    check(q["lstate"] == "Failed" and q["lfail"] == "connection_lost", "read fails: connection lost")
    check(q["mask"] == "0", "incomplete data confirms nothing")
    first = len(sent)
    tool.cmd(f"T {clock[0] + 100}")
    check(tool.cmd("NEXT", reply=True) == "-", "nothing is replayed after the loss")
    clock[0] += 2500
    tool.cmd(f"T {clock[0]}")
    check(tool.cmd("RETRY", reply=True) == "1", "reconnect attempt due after the retry delay")
    check(tool.cmd("NEXT", reply=True) == "-", "reconnecting does not restart the read")
    q2, closed2, load2 = session(port, tool, clock, sent)
    check(load2 == "started" and sent[first] == "$I", "an explicit new read starts again from the first query ($I)")
    out = sim.stop()
    check("VIOLATION" not in out, "simulator audit: only allowlisted read-only queries received")
    tool.close()

    # ---- late reply after a timeout --------------------------------------------------
    print("[caps-late-ok]")
    sim = Sim("caps-late-ok")
    tool = Tool(exe)
    clock = [1000]
    sent = []
    q, closed, load = session(port, tool, clock, sent, settle_real=3.6)
    check(q["lstate"] == "Failed" and q["lfail"] == "controller_did_not_reply_(timeout)", "slow reply: the read timed out")
    check(int(q["ok"]) == 2, "the late reply was not counted as an answer")
    check(q["mask"] == "0", "nothing confirmed from a failed read")
    clock[0] += 3000
    tool.cmd(f"T {clock[0]}")
    s2 = []
    q3, closed3, load3 = session(port, tool, clock, s2)
    if q3["lstate"] != "Complete":
        print("      diagnostic:", load3, q3)
    check(q3["lstate"] == "Complete" and int(q3["ok"]) == 53, "a fresh read afterwards completes normally")
    out = sim.stop()
    check("VIOLATION" not in out, "simulator audit: only allowlisted read-only queries received")
    tool.close()

    # ---- the audit itself catches machine-changing traffic ----------------------------
    print("[audit self-test]")
    sim = Sim("caps")
    s = socket.create_connection(("127.0.0.1", port), timeout=1)
    s.sendall(b"$H\n")
    s.sendall(b"G0 X1\n")
    s.sendall(b"$J=G91G21F100X1\n")
    s.sendall(bytes([0x85]))
    time.sleep(0.6)
    s.close()
    out = sim.stop()
    check(out.count("VIOLATION") == 4, "simulator flags home, G-code, jog and a real-time byte (this is a test of the audit)")
