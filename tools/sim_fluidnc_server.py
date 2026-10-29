#!/usr/bin/env python3
"""Simulated FluidNC Telnet-style status server for the JC read-only client.

Never talks to a real controller. Point the JC "FluidNC host" at this PC's LAN
address to exercise parsing, reconnect and stale handling safely.

Every byte received from the device is audited: the only permitted byte is '?'.
Any other byte is reported as a VIOLATION and makes the process exit non-zero.

Usage:
  python tools/sim_fluidnc_server.py --scenario normal [--port 23]
Scenarios:
  normal      4-axis MPos frames, including negative values and A in degrees
  work        4-axis WPos frames with WCO
  fragmented  frames split into 1-3 byte TCP segments with delays
  malformed   good frames interleaved with bad frames (JC must count errors)
  three-axis  3-axis frames (JC must reject: it requires four axes)
  close       closes the socket after a few frames (tests reconnect)
  silent      accepts and answers nothing (tests stale / response timeout)
"""
import argparse
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

violations = 0


def audit(data: bytes, peer):
    global violations
    for b in data:
        if b != ord("?"):
            violations += 1
            print(f"VIOLATION: {peer} sent byte 0x{b:02x}", flush=True)


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


def serve(conn, peer, scenario):
    conn.settimeout(0.2)
    frames = frames_for(scenario)
    index = sent = queries = 0
    while True:
        try:
            data = conn.recv(64)
            if not data:
                print(f"{peer}: device closed connection; queries={queries}", flush=True)
                return
            audit(data, peer)
            queries += data.count(b"?")
        except socket.timeout:
            data = b""
        except OSError:
            return
        if scenario == "silent" or not data:
            continue
        frame = (frames[index % len(frames)] + "\r\n").encode()
        index += 1
        try:
            if scenario == "fragmented":
                for i in range(0, len(frame), 2):
                    conn.sendall(frame[i:i + 2])
                    time.sleep(0.05)
            else:
                conn.sendall(frame)
        except OSError:
            return
        sent += 1
        print(f"{peer}: query #{queries} -> {frame.decode().strip()}", flush=True)
        if scenario == "close" and sent >= 3:
            print(f"{peer}: closing after {sent} frames", flush=True)
            conn.close()
            return


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--scenario", default="normal",
                   choices=["normal", "work", "fragmented", "malformed",
                            "three-axis", "close", "silent"])
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
    print(f"audit: {'FAIL' if violations else 'PASS'} ({violations} non-'?' bytes)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
