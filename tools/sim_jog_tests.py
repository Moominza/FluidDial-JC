"""First-live-jog scenarios for tools/run_sim_integration.py (Instruction 006).

REAL: the TCP session with tools/sim_fluidnc_server.py, and the JC C++ stream
splitter, response classifier, command executor, jog session, authorization gate and
dispatcher (test/host/jc_link_tool.cpp built with the live-jog policy). Bytes the C++
session wants to transmit are forwarded over the socket exactly as the experimental
firmware's JcApp::send_jog() would.

NOT real: the ESP32 socket layer and lwIP; the controller (the simulator's motion model
is a few lines of Python with invented timing); and NOTHING here touches a real machine.
The simulator audits every byte with its own, independent copy of the test-jog rule.
"""
import socket
import time


def kv(line):
    return dict(x.split("=", 1) for x in line.split())


class Run:
    """One pendant-like client session against the simulator."""

    def __init__(self, port, tool, clock):
        self.port, self.tool, self.clock = port, tool, clock
        self.s = None
        self.closed = False
        self.last = time.time()
        self.last_poll = 0.0
        self.forwarded = []   # raw byte strings forwarded to the simulator
        self.phases = []      # distinct phases seen, in order

    def connect(self):
        self.s = socket.create_connection(("127.0.0.1", self.port), timeout=1)
        self.s.settimeout(0.02)
        self.closed = False
        self.last = time.time()
        self.last_poll = 0.0
        self.t()
        self.tool.cmd("CONNECT")

    def t(self):
        now = time.time()
        self.clock[0] += max(1, int((now - self.last) * 1000))
        self.last = now
        self.tool.cmd(f"T {self.clock[0]}")

    def pump(self):
        try:
            data = self.s.recv(2048)
        except socket.timeout:
            return
        except OSError:
            data = b""
        if not data:
            self.mark_closed()
            return
        self.t()
        self.tool.cmd("DATA " + data.hex())

    def mark_closed(self):
        if not self.closed:
            self.closed = True
            self.t()
            self.tool.cmd("CLOSE")

    def flush(self):
        hx = self.tool.cmd("JTX", reply=True)
        if hx and hx != "-":
            data = bytes.fromhex(hx)
            if not self.closed:
                try:
                    self.s.sendall(data)
                    self.forwarded.append(data)
                except OSError:
                    self.mark_closed()

    def summary(self):
        return kv(self.tool.cmd("JSUM", reply=True))

    def step(self, poll=True):
        self.t()
        if poll and not self.closed and time.time() - self.last_poll >= 0.3:
            self.last_poll = time.time()
            try:
                self.s.send(b"?")
            except OSError:
                self.mark_closed()
        if not self.closed:
            self.pump()
        self.t()
        self.tool.cmd("TICK")
        self.flush()
        ph = self.summary()["phase"]
        if not self.phases or self.phases[-1] != ph:
            self.phases.append(ph)

    def run_for(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.step()

    def run_until(self, pred, timeout=8.0):
        end = time.time() + timeout
        s = self.summary()
        while time.time() < end:
            self.step()
            s = self.summary()
            if pred(s):
                return s
        return s

    def settle(self, seconds=1.0):
        self.run_for(seconds)

    def jog(self, axis, dist, feed):
        self.t()
        reply = self.tool.cmd(f"JOG {axis} {dist} {feed}", reply=True)
        self.flush()
        return reply

    def cancel(self):
        self.t()
        reply = self.tool.cmd("JCANCEL", reply=True)
        self.flush()
        return reply

    def position(self):
        q = kv(self.tool.cmd("Q", reply=True))
        return q.get("M", "n/a"), q

    def close(self):
        try:
            if self.s:
                self.s.close()
        except OSError:
            pass


def run_all(exe, check, Sim, Tool, port):
    def scenario(name):
        print(f"[{name}]")
        sim = Sim(name)
        tool = Tool(exe)
        clock = [1000]
        run = Run(port, tool, clock)
        run.connect()
        return sim, tool, run

    def finish(sim, tool, run, expect_jog_lines=None, expect_cancels=None):
        run.close()
        out = sim.stop()
        tool.close()
        check("VIOLATION" not in out, "simulator audit: only strictly formatted test jogs and the jog-cancel byte received")
        if expect_jog_lines is not None:
            n = out.count(": JOG $J=")
            check(n == expect_jog_lines, f"simulator saw exactly {expect_jog_lines} jog line(s) (saw {n})")
        if expect_cancels is not None:
            n = out.count("CANCEL 0x85")
            check(n == expect_cancels, f"simulator saw exactly {expect_cancels} jog-cancel byte(s) (saw {n})")
        return out

    # ---- the policy build really is the live-jog policy ----------------------------------
    print("[policy]")
    tool = Tool(exe)
    tool.cmd("T 1000")
    tool.cmd("JARM")
    r = tool.cmd("JOG 0 1.0 100", reply=True)
    check(r == "decision=blocked:_stale_or_missing_machine_status sent=0",
          "a jog without any machine status is blocked by the gate (live-jog policy compiled in)")
    check(tool.cmd("JTX", reply=True) == "-", "...and nothing was queued for transmission")
    for args, want in (("0 10 100", "rejected:TestLimit"), ("1 50 100", "rejected:TestLimit"),
                       ("0 100 100", "rejected:TestLimit"), ("0 1.001 100", "rejected:TestLimit"),
                       ("2 0.6 50", "rejected:TestLimit"), ("0 1 101", "rejected:TestLimit"),
                       ("2 0.5 51", "rejected:TestLimit"), ("0 1 3000", "rejected:TestLimit"),
                       ("3 1 100", "rejected:JogDisabled"), ("3 -45 3600", "rejected:JogDisabled"),
                       ("3 0.001 1", "rejected:JogDisabled"), ("0 0 100", "rejected:BadValue"),
                       ("7 1 100", "rejected:BadAxis")):
        check(tool.cmd("JOG " + args, reply=True) == want, f"JOG {args} -> {want}")
    check(tool.cmd("JTX", reply=True) == "-", "no rejected request produced any transmission")
    tool.close()

    # ---- first jog: ack, motion, completion, position ------------------------------------
    sim, tool, run = scenario("jog")
    run.settle(1.0)
    q = run.position()[1]
    check(q["state"] == "Connected" and q["fresh"] == "1" and q["machine"] == "Idle", "fresh Idle status before the first jog")
    check(run.summary()["jogs"] == "0", "no jog sent yet")
    r = run.jog(0, 1.0, 100)
    check(r.startswith("decision=blocked:_machine_control_not_armed") and r.endswith("sent=0"), "not armed: the first request is refused by the gate")
    tool.cmd("JARM")
    r = run.jog(0, 1.0, 100)
    check(r == "decision=allowed sent=1", "armed, fresh, Idle: the jog is authorized and transmitted")
    s = run.run_until(lambda s: s["result"] == "COMPLETED", 8.0)
    check(s["result"] == "COMPLETED", "controller ok, motion, then Idle: COMPLETED")
    check(any("moving" in p or "accepted" in p for p in run.phases), "acknowledgement / motion were observed as separate phases before completion")
    check(s["poscheck"] == "1" and abs(float(s["delta"]) - 1.0) < 0.02, "reported position moved by exactly the commanded +1.000 mm")
    check(s["ack"] == "0" and s["unresolved"] == "0", "nothing left unresolved after a clean jog")
    check(run.position()[0].startswith("1.000,0.000,0.000,0.000"), "status position: X +1.000, others unchanged")
    r = run.jog(0, 1.0, 100)
    # Authorized by the gate but refused by the session itself (settle window): not sent.
    check(r.endswith("sent=0"), "an immediate second jog is not sent (settle window)")
    run.run_for(1.0)
    r = run.jog(0, -1.0, 100)
    check(r == "decision=allowed sent=1", "after the settle window a second jog is accepted")
    s = run.run_until(lambda s: s["result"] == "COMPLETED" and s["jogs"] == "2", 8.0)
    run.run_for(0.5)
    check(run.position()[0].startswith("0.000,0.000,0.000,0.000"), "position back at X 0.000 after the reverse jog")
    check(s["jogs"] == "2", "two jogs in total")
    finish(sim, tool, run, expect_jog_lines=2, expect_cancels=0)

    # ---- touch bounce: a burst of identical requests transmits one -------------------------
    sim, tool, run = scenario("jog")
    run.settle(1.0)
    tool.cmd("JARM")
    replies = []
    for _ in range(6):
        run.t()
        replies.append(tool.cmd("JOG 1 -1.0 100", reply=True))
    hx = tool.cmd("JTX", reply=True)
    run.flush()
    check(replies[0] == "decision=allowed sent=1", "bounce: the first press is accepted")
    check(all(r.endswith("sent=0") for r in replies[1:]), "bounce: the five repeats are refused")
    check(hx.count("244a3d") == 1 if hx != "-" else False, "bounce: exactly one jog line was queued for transmission")
    # we consumed the transmit buffer above, so forward it by hand
    if hx != "-":
        run.s.sendall(bytes.fromhex(hx))
    run.run_until(lambda s: s["result"] == "COMPLETED", 8.0)
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=0)

    # ---- controller rejects the jog ---------------------------------------------------------
    sim, tool, run = scenario("jog-reject")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    s = run.run_until(lambda s: s["result"].startswith("REJECTED"), 5.0)
    check(s["result"] == "REJECTED_by_controller" and s["code"] == "15", "error:15 is reported as a rejection with its code")
    check(s["ack"] == "0" and s["unresolved"] == "0", "a rejection moved nothing, so nothing is latched")
    check(run.position()[0].startswith("0.000,0.000,0.000,0.000"), "position unchanged after a rejected jog")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=0)

    # ---- alarm -------------------------------------------------------------------------------
    sim, tool, run = scenario("jog-alarm")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    s = run.run_until(lambda s: s["result"] == "ALARM", 5.0)
    check(s["result"] == "ALARM" and s["code"] == "1" and s["ack"] == "1", "ALARM:1 is reported and latched")
    run.run_for(1.0)
    tool.cmd("JARM")
    r = run.jog(0, 1.0, 100)
    check(r.endswith("sent=0") and r.startswith("decision=blocked"), "a new jog is refused while the alarm is unresolved")
    check(tool.cmd("JACK", reply=True) == "1", "the operator can acknowledge once nothing is in flight")
    r = run.jog(0, 1.0, 100)
    check(r.startswith("decision=blocked:_machine_in_alarm") and r.endswith("sent=0"), "after acknowledging, the machine is still in Alarm: still blocked")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=0)

    # ---- no reply: timeout, cancel, never resent ---------------------------------------------------
    sim, tool, run = scenario("jog-silent")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    s = run.run_until(lambda s: s["result"].startswith("NO_REPLY") and s["phase"] == "idle", 8.0)
    check(s["result"].startswith("NO_REPLY") and s["timeouts"] == "1", "no ok within 2 s: timed out")
    check(s["cancels"] == "1" and s["jogs"] == "1", "a cancel was sent and the jog line was NOT resent")
    check(s["ack"] == "1" and s["unresolved"] == "1", "the unknown outcome is latched until the operator acknowledges")
    run.run_for(1.5)
    tool.cmd("JARM")
    r = run.jog(0, 1.0, 100)
    check(r.endswith("sent=0") and r.startswith("decision=blocked"), "no new jog while the timeout is unresolved")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=1)

    # ---- connection lost during a jog, reconnect, no replay ------------------------------------------
    sim, tool, run = scenario("jog-close")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    s = run.run_until(lambda s: s["result"] == "CONNECTION_LOST", 6.0)
    check(run.closed, "the device saw the connection close mid-jog")
    check(s["result"] == "CONNECTION_LOST" and s["phase"] == "recovering_connection", "loss during a jog: recovering, never 'completed'")
    check(s["ack"] == "1" and s["cancels"] == "0", "latched; no cancel can be sent on a dead link")
    time.sleep(1.5)          # the simulated machine finishes the move while the pendant is blind
    run.close()
    run.connect()
    run.settle(2.5)          # reconnect: fresh Idle frames and the quiet period
    s = run.summary()
    check(s["phase"] == "idle" and s["ack"] == "1", "after reconnecting the session is Idle but the failure still needs acknowledging")
    check(s["jogs"] == "1", "reconnecting transmitted nothing")
    tool.cmd("JARM")
    r = run.jog(0, 1.0, 100)
    check(r.endswith("sent=0") and r.startswith("decision=blocked"), "no jog after reconnect until acknowledged")
    check(run.position()[0].startswith("1.000,0.000,0.000,0.000"), "the pendant reports the machine's real position (+1.000) after reconnecting")
    check(tool.cmd("JACK", reply=True) == "1", "operator acknowledges")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=0)

    # ---- stale status while moving: cancel attempted -------------------------------------------------------
    sim, tool, run = scenario("jog-stale")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    stale_sent = False
    end = time.time() + 12.0
    s = run.summary()
    while time.time() < end:
        run.step()
        if not stale_sent and kv(tool.cmd("Q", reply=True))["fresh"] == "0":
            tool.cmd("JSTALE")      # what JcApp::note_link_fresh() does when the data goes stale
            run.flush()
            stale_sent = True
        s = run.summary()
        if stale_sent and s["phase"] == "idle":
            break
    check(stale_sent, "the status stream went stale while the jog was unresolved")
    check(s["result"].startswith("STATUS_STALE") and s["cancels"] == "1", "stale status: a cancel was attempted and recorded")
    check(s["ack"] == "1", "latched until acknowledged")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=1)

    # ---- cancel before the ok; the jog still starts afterwards ---------------------------------------------------
    sim, tool, run = scenario("jog-late-start")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    run.run_for(0.3)
    r = run.cancel()
    check(r == "decision=allowed sent=1", "the operator cancels before the controller answered (no arming or confirmation needed)")
    s = run.run_until(lambda s: s["result"] == "CANCELLED", 3.0)
    check(s["result"] == "CANCELLED", "cancel resolved once the controller reported Idle")
    s = run.run_until(lambda s: s["unexpected"] == "1" and s["phase"] == "idle", 10.0)
    check(s["unexpected"] == "1" and s["result"].startswith("UNEXPECTED_MOTION"), "the late-starting jog was detected as unexpected motion")
    check(s["cancels"] == "2" and s["ack"] == "1" and s["jogs"] == "1", "it was cancelled again, latched, and nothing was resent")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=2)

    # ---- the controller ignores the cancel --------------------------------------------------------------------------
    sim, tool, run = scenario("jog-nocancel")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    run.run_until(lambda s: "moving" in s["phase"], 4.0)
    run.cancel()
    s = run.run_until(lambda s: s["unconfirmed"] == "1", 8.0)
    check(s["unconfirmed"] == "1" and s["phase"] == "cancelling", "cancel not confirmed within 3 s: reported, still unresolved")
    check(tool.cmd("JACK", reply=True) == "0", "cannot acknowledge while the controller is still moving")
    tool.cmd("JARM")
    r = run.jog(1, 1.0, 100)
    check(r.endswith("sent=0") and r.startswith("decision=blocked"), "no new jog while the cancel is unconfirmed")
    run.cancel()
    s = run.summary()
    check(int(s["cancels"]) >= 2, "the operator can send the cancel again")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=None)

    # ---- a move that ends in the wrong place -------------------------------------------------------------------------
    sim, tool, run = scenario("jog-short")
    run.settle(1.0)
    tool.cmd("JARM")
    run.jog(0, 1.0, 100)
    s = run.run_until(lambda s: s["result"] == "POSITION_MISMATCH", 8.0)
    check(s["result"] == "POSITION_MISMATCH" and s["poscheck"] == "2", "position mismatch detected after completion")
    check(abs(float(s["delta"]) - 0.6) < 0.02 and s["ack"] == "1", "reported move was 0.600 mm instead of 1.000; latched")
    finish(sim, tool, run, expect_jog_lines=1, expect_cancels=0)

    # ---- the audit itself rejects anything outside the rule ---------------------------------------------------------------
    print("[audit self-test: jog]")
    sim = Sim("jog")
    s = socket.create_connection(("127.0.0.1", port), timeout=1)
    for payload in (b"$J=G91G21F100X10.000\n",     # oversized step
                    b"$J=G91G21F100A1.000\n",      # rotary axis
                    b"$J=G91G21F100X1.000 Y1.000\n",  # extra word
                    b"$J=G91G21F101X1.000\n",      # feed above the limit
                    b"$H\n", b"G0 X1\n",
                    b"!", b"~", bytes([0x18])):    # other real-time bytes
        s.sendall(payload)
        time.sleep(0.08)
    s.sendall(b"$J=G91G21F100X1.000\n")            # one legitimate jog...
    time.sleep(0.15)
    s.sendall(b"$J=G91G21F100X1.000\n")            # ...and a second while it is unresolved
    time.sleep(0.3)
    s.sendall(bytes([0x85]))                       # the cancel byte is permitted
    time.sleep(0.2)
    s.close()
    out = sim.stop()
    check(out.count("VIOLATION") == 10, "audit flags oversize, rotary, extra word, feed, $H, G-code, ! ~ 0x18 and a jog while unresolved (%d)" % out.count("VIOLATION"))
    check(out.count(": JOG $J=") == 1 and out.count("CANCEL 0x85") == 1, "the one legitimate jog and the cancel were accepted")
