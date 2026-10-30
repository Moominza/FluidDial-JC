"""Reconnection scenarios for tools/run_integration (Instruction 006C.1).

REAL: TCP connection attempts to a real listening simulator (tools/sim_fluidnc_server.py) or to a
port with nothing listening (connection refused), and the JC C++ link tracker, status parser and
bounded exponential backoff (test/host/jc_link_tool.cpp). The clock is virtual, so the long retry
intervals cost no waiting time.
NOT real: the ESP32 Wi-Fi association (a refused TCP connection stands in for "Wi-Fi or FluidNC
temporarily unavailable"; the Wi-Fi layer's own retry timing uses the same backoff function but is
only checked on hardware) and the controller (a simulator with invented values).
"""
import socket


def attempt(port):
    """One real TCP connection attempt. Returns the socket, or None when refused / unreachable."""
    try:
        return socket.create_connection(("127.0.0.1", port), timeout=1)
    except OSError:
        return None


def kv(line):
    return dict(x.split("=", 1) for x in line.split())


def run_all(exe, check, Sim, Tool, port):
    # ---- the controller is unreachable: attempts back off 2, 4, 8, 16, 30, 30 s -------------------------
    print("[reconnect: controller unavailable, then it returns]")
    tool = Tool(exe)
    clock = [1000]

    def at(ms):
        clock[0] = ms
        tool.cmd(f"T {ms}")

    at(1000)
    tool.cmd("REQUEST")                                    # boot auto-connect asks for a connection
    check(tool.cmd("RETRY", reply=True) == "1", "the first attempt starts at once (no manual action)")
    attempts = [clock[0]]
    delays = []
    sock = attempt(port)                                   # nothing is listening
    check(sock is None, "the controller is unavailable: the real TCP attempt is refused")
    tool.cmd("CLOSE")                                      # the attempt failed
    for _ in range(7):
        wait = int(tool.cmd("RETRYMS", reply=True))
        delays.append(wait)
        at(clock[0] + wait - 1)
        check(tool.cmd("RETRY", reply=True) == "0", f"not retried 1 ms early (wait {wait} ms)")
        at(clock[0] + 1)
        check(tool.cmd("RETRY", reply=True) == "1", f"retried exactly when due (after {wait} ms)")
        attempts.append(clock[0])
        s = attempt(port)
        if s is not None:
            s.close()
            break
        tool.cmd("CLOSE")
    check(delays == [2000, 4000, 8000, 16000, 30000, 30000, 30000],
          "retry delays double from 2 s and stop growing at 30 s: %s" % delays)
    check(max(delays) <= 30000, "no wait is ever longer than 30 s, and none is shorter than 2 s")

    # The controller comes back: the next due attempt connects and valid status restores CONNECTED.
    sim = Sim("normal")
    wait = int(tool.cmd("RETRYMS", reply=True))
    at(clock[0] + wait)
    check(tool.cmd("RETRY", reply=True) == "1", "the retry after the controller returned is due on schedule")
    s = attempt(port)
    check(s is not None, "the real TCP attempt now succeeds")
    tool.cmd("UP")
    s.settimeout(0.5)
    s.send(b"?")
    clock[0] += 200
    at(clock[0])
    try:
        data = s.recv(256)
    except OSError:
        data = b""
    tool.cmd("DATA " + data.hex())
    q = kv(tool.cmd("Q", reply=True))
    check(q["state"] == "Connected" and q["fresh"] == "1", "valid status arrived: CONNECTED and fresh")
    # A new loss after a success starts again from the base delay (the backoff was reset).
    s.close()
    at(clock[0] + 100)
    tool.cmd("CLOSE")
    check(int(tool.cmd("RETRYMS", reply=True)) == 2000, "after a success the backoff starts over at 2 s")

    # ---- manual disconnect: no automatic reconnection ------------------------------------------------
    print("[manual disconnect suppresses reconnection]")
    tool.cmd("DISCONNECT")
    quiet = True
    for _ in range(12):
        at(clock[0] + 60000)
        quiet = quiet and tool.cmd("RETRY", reply=True) == "0"
    check(quiet, "12 minutes after a manual disconnect nothing tried to reconnect")
    check(int(tool.cmd("RETRYMS", reply=True)) == 0, "no retry is scheduled")
    q = kv(tool.cmd("Q", reply=True))
    check(q["state"] == "Disconnected" and q["fresh"] == "0", "the link reports DISCONNECTED, not fresh")
    tool.cmd("REQUEST")                                    # the operator connects again
    check(tool.cmd("RETRY", reply=True) == "1", "CONNECT again starts a new attempt immediately")
    out = sim.stop()
    check("VIOLATION" not in out, "simulator audit: only '?' was sent during all of this")
    tool.close()

    # ---- stale status: the stream stops while the socket stays open ---------------------------------------
    print("[stale status]")
    sim = Sim("normal")
    tool = Tool(exe)
    at(5000)
    tool.cmd("REQUEST")
    tool.cmd("RETRY", reply=True)
    s = attempt(port)
    tool.cmd("UP")
    s.settimeout(0.5)
    s.send(b"?")
    at(5100)
    tool.cmd("DATA " + s.recv(256).hex())
    check(kv(tool.cmd("Q", reply=True))["fresh"] == "1", "fresh right after a valid frame")
    at(5100 + 2999)
    check(kv(tool.cmd("Q", reply=True))["fresh"] == "1", "still fresh 2.999 s later")
    at(5100 + 3001)
    q = kv(tool.cmd("Q", reply=True))
    check(q["fresh"] == "0" and q["state"] == "Stale", "no frame for 3 s: STALE and not fresh")
    s.close()
    out = sim.stop()
    check("VIOLATION" not in out, "simulator audit: only '?' was sent")
    tool.close()
