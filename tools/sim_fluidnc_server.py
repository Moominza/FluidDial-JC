#!/usr/bin/env python3
"""Simulated FluidNC Telnet-style server for the JC client.

Never talks to a real controller. Point the JC "FluidNC host" at this PC's LAN
address to exercise parsing, reconnect, stale handling and the read-only
capability queries safely. The configuration values it reports are INVENTED test
data, not any real machine.

Everything the device sends is audited:
  * the single byte '?' (status query) is permitted;
  * a text line is permitted only if it is one of the closed set of read-only
    queries ($I, $G, $/axes/<x|y|z|a>/<item>, $/probe/pin, $/probe/toolsetter_pin);
  * anything else (jog, home, unlock, G-code, assignments, real-time bytes) is
    reported as a VIOLATION and makes the process exit non-zero.

Usage:
  python tools/sim_fluidnc_server.py --scenario normal [--port 23]
Status scenarios:
  normal      4-axis MPos frames, including negative values and A in degrees
  work        4-axis WPos frames with WCO
  fragmented  frames split into 1-3 byte TCP segments with delays
  malformed   good frames interleaved with bad frames (JC must count errors)
  three-axis  3-axis frames (JC must reject: it requires four axes)
  close       closes the socket after a few frames (tests reconnect)
  silent      accepts and answers nothing (tests stale / response timeout)
Capability scenarios (normal frames plus answers to the read-only queries):
  caps             every query answered with plausible values
  caps-fragmented  same, replies split into 2-3 byte segments
  caps-refuse-a    the A-axis queries are refused with error:3
  caps-bad         some values are implausible (nan, negative, text)
  caps-silent      frames are answered but queries never are (timeout path)
  caps-close       the socket is closed after a few queries (loss mid-read)
  caps-alarm       ALARM:1 is raised in reply to the 5th query
  caps-late-ok     the 3rd query is answered only after a long delay (late reply)
"""
import argparse
import re
import socket
import sys
import time

GOOD_MPOS = [
    "<Idle|MPos:0.000,0.000,0.000,0.000|FS:0,0>",
    "<Run|MPos:-12.345,100.500,-0.250,45.000|FS:500,0>",
    "<Idle|MPos:-250.000,-0.001,12.000,-359.990|FS:0,0>",
]
GOOD_WPOS = [
    "<Idle|WPos:1.000,-2.000,3.500,90.000|FS:0,0|WCO:10.000,20.000,0.000,0.000>",
    "<Jog|WPos:-1.500,2.250,0.000,-10.000|FS:200,0>",
]
MALFORMED = [
    "<Idle|MPos:1.0,2.0,3.0|FS:0,0>",                 # three axes
    "<Bogus|MPos:1,2,3,4|FS:0,0>",                    # unknown state
    "<Idle|MPos:1,2,x,4|FS:0,0>",                     # non-numeric
    "<Idle|MPos:nan,0,0,0|FS:0,0>",                   # non-finite
    "<Idle|MPos:1,2,3,4,5|FS:0,0>",                   # five axes
    "<Idle|MPos:1,2,3,4",                             # unterminated
]
THREE_AXIS = ["<Idle|MPos:1.000,2.000,3.000|FS:0,0>"]

# Invented configuration: steps/mm, travel mm, max rate, accel, homing cycle, +dir
CONFIG = {
    "x": ["800.000", "300.000", "5000.000", "200.000", "2", "false"],
    "y": ["800.000", "250.000", "5000.000", "200.000", "3", "false"],
    "z": ["800.000", "100.000", "2000.000", "100.000", "1", "true"],
    "a": ["17.778", "360.000", "3600.000", "50.000", "0", "false"],
}
ITEMS = ["steps_per_mm", "max_travel_mm", "max_rate_mm_per_min", "acceleration_mm_per_sec2",
         "homing/cycle", "homing/positive_direction"]

ALLOWED = [re.compile(p) for p in (
    r"^\$I$",
    r"^\$G$",
    r"^\$/probe/pin$",
    r"^\$/probe/toolsetter_pin$",
    r"^\$/axes/[xyza]/(steps_per_mm|max_travel_mm|max_rate_mm_per_min|acceleration_mm_per_sec2|"
    r"homing/cycle|homing/positive_direction)$",
)]
REALTIME_BYTES = {0x18, 0x85, ord("!"), ord("~")}

violations = 0
queries_seen = []
late_reply_used = [False]  # caps-late-ok delays one reply once per server process


def violation(peer, what):
    global violations
    violations += 1
    print(f"VIOLATION: {peer} sent {what}", flush=True)


def reply_for(line, scenario, query_index):
    """Reply text for one allowed query line."""
    if scenario == "caps-alarm" and query_index == 4:
        return "ALARM:1\r\n"
    if line == "$I":
        return "[VER:4.1.1 FluidNC v4.1.1:]\r\n[OPT:PH]\r\nok\r\n"
    if line == "$G":
        return "[GC:G0 G55 G17 G21 G90 G94 M5 M9 T2 F0 S0]\r\nok\r\n"
    if line == "$/probe/pin":
        return "$/probe/pin=gpio.36:low\r\nok\r\n"
    if line == "$/probe/toolsetter_pin":
        return "$/probe/toolsetter_pin=NO_PIN\r\nok\r\n"
    m = re.match(r"^\$/axes/([xyza])/(.+)$", line)
    axis, item = m.group(1), m.group(2)
    if scenario == "caps-refuse-a" and axis == "a":
        return "error:3\r\n"
    value = CONFIG[axis][ITEMS.index(item)]
    if scenario == "caps-bad":
        if (axis, item) == ("x", "steps_per_mm"):
            value = "nan"
        elif (axis, item) == ("y", "max_travel_mm"):
            value = "-5"
        elif (axis, item) == ("z", "max_rate_mm_per_min"):
            value = "abc"
    return f"{line}={value}\r\nok\r\n"


def frames_for(scenario):
    if scenario == "work":
        return GOOD_WPOS
    if scenario == "three-axis":
        return THREE_AXIS
    if scenario == "malformed":
        out = []
        for bad in MALFORMED:
            out += [GOOD_MPOS[0], bad]
        return out
    return GOOD_MPOS


def send(conn, data, scenario):
    if scenario in ("fragmented", "caps-fragmented"):
        step = 2 if scenario == "fragmented" else 3
        for i in range(0, len(data), step):
            conn.sendall(data[i:i + step])
            time.sleep(0.02)
    else:
        conn.sendall(data)


def serve(conn, peer, scenario):
    conn.settimeout(0.2)
    frames = frames_for(scenario)
    is_caps = scenario.startswith("caps")
    index = sent = queries = 0
    buffer = b""
    query_index = 0
    delayed = []  # (due_time, bytes) for caps-late-ok
    while True:
        now = time.time()
        for item in list(delayed):
            if now >= item[0]:
                delayed.remove(item)
                try:
                    conn.sendall(item[1])
                except OSError:
                    return
        try:
            data = conn.recv(256)
            if not data:
                print(f"{peer}: device closed connection; status queries={queries}", flush=True)
                return
        except socket.timeout:
            data = b""
        except OSError:
            return

        status_queries = 0
        for b in data:
            if b == ord("?"):
                status_queries += 1
                queries += 1
            elif b in REALTIME_BYTES:
                violation(peer, f"real-time byte 0x{b:02x}")
            elif b == 0x0A:
                line = buffer.decode("latin-1").strip("\r")
                buffer = b""
                if not line:
                    continue
                if not any(p.match(line) for p in ALLOWED):
                    violation(peer, f"line {line!r}")
                    continue
                queries_seen.append(line)
                print(f"{peer}: QUERY {line}", flush=True)
                if not is_caps or scenario == "caps-silent":
                    continue
                reply = reply_for(line, scenario, query_index).encode()
                query_index += 1
                if scenario == "caps-late-ok" and query_index == 3 and not late_reply_used[0]:
                    late_reply_used[0] = True
                    delayed.append((time.time() + 3.0, reply))
                    continue
                try:
                    send(conn, reply, scenario)
                except OSError:
                    return
                if scenario == "caps-close" and query_index >= 5:
                    print(f"{peer}: closing after {query_index} queries", flush=True)
                    conn.close()
                    return
            else:
                buffer += bytes([b])
                if len(buffer) > 200:
                    violation(peer, f"over-long line start {buffer[:20]!r}")
                    buffer = b""

        if scenario == "silent":
            continue
        if is_caps and status_queries > 1:
            status_queries = 1  # coalesce: the capability scenarios care about the query replies
        for _ in range(status_queries):  # a real controller answers every '?'
            frame = (frames[index % len(frames)] + "\r\n").encode()
            index += 1
            try:
                send(conn, frame, "fragmented" if scenario == "fragmented" else "normal")
            except OSError:
                return
            sent += 1
            print(f"{peer}: query #{queries} -> {frame.decode().strip()}", flush=True)
            if scenario == "close" and sent >= 3:
                print(f"{peer}: closing after {sent} frames", flush=True)
                conn.close()
                return


def main():
    choices = ["normal", "work", "fragmented", "malformed", "three-axis", "close", "silent",
               "caps", "caps-fragmented", "caps-refuse-a", "caps-bad", "caps-silent", "caps-close",
               "caps-alarm", "caps-late-ok"]
    p = argparse.ArgumentParser()
    p.add_argument("--scenario", default="normal", choices=choices)
    p.add_argument("--port", type=int, default=23)
    p.add_argument("--bind", default="0.0.0.0")
    args = p.parse_args()

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(1)
    print(f"sim FluidNC '{args.scenario}' listening on {args.bind}:{args.port}", flush=True)
    try:
        while True:
            conn, addr = srv.accept()
            print(f"{addr}: connected", flush=True)
            with conn:
                serve(conn, addr, args.scenario)
    except KeyboardInterrupt:
        pass
    print(f"audit: {'FAIL' if violations else 'PASS'} ({violations} non-permitted transmissions)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
