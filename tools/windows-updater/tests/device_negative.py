#!/usr/bin/env python3
"""Negative tests against a REAL pendant (writes only to the inactive flash slot; never touches the running image).

  python device_negative.py <ip> interrupt <good.bin>   send ~25 % of a good image, then drop the connection
  python device_negative.py <ip> corrupt   <good.bin>   send a full image with one byte changed (fails image verification)
  python device_negative.py <ip> foreign   <other.bin>  send a complete, valid image built for another device
                                                        (for example the M5Dial build): the pendant must refuse it

The OTA password is read from the environment variable FD_PW and is never printed. The pendant must be READY
(SETTINGS > FIRMWARE UPDATE). After each run check that the boot slot did not change (otadata) and that the
pendant is offered again (probe).
"""
import hashlib, os, random, socket, sys, time


def md5(b):
    return hashlib.md5(b).hexdigest()


def main():
    ip, mode, path = sys.argv[1], sys.argv[2], sys.argv[3]
    pw = os.environ.get("FD_PW", "")
    if not pw:
        print("set FD_PW"); return 2
    data = bytearray(open(path, "rb").read())
    if mode == "corrupt":
        data[len(data) // 2] ^= 0xFF
    data = bytes(data)
    fm = md5(data)
    srv = socket.socket(); port = random.randint(10000, 60000); srv.bind(("0.0.0.0", port)); srv.listen(1)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.settimeout(2)
    reply = None
    for _ in range(8):
        u.sendto(("0 %d %d %s\n" % (port, len(data), fm)).encode(), (ip, 3232))
        try:
            reply = u.recv(64).decode(); break
        except socket.timeout:
            pass
    if reply is None:
        print("RESULT no answer from the pendant (not offered)"); return 1
    if reply.startswith("AUTH"):
        nonce = reply.split()[1]
        cnonce = md5(os.urandom(8))
        res = md5((md5(pw.encode()) + ":" + nonce + ":" + cnonce).encode())
        u.sendto(("200 %s %s\n" % (cnonce, res)).encode(), (ip, 3232))
        u.settimeout(10)
        a = u.recv(64).decode()
        if a != "OK":
            print("RESULT authentication refused:", a); return 1
    srv.settimeout(15)
    try:
        c, _ = srv.accept()
    except socket.timeout:
        print("RESULT the pendant did not connect back (it refused the image before the transfer)"); return 0
    c.settimeout(15)
    sent = 0
    limit = len(data) // 4 if mode == "interrupt" else len(data)
    ok = False
    try:
        while sent < limit:
            chunk = data[sent:sent + 1024]
            c.sendall(chunk); sent += len(chunk)
            r = c.recv(32).decode(errors="replace")
            ok = "OK" in r
        if mode == "interrupt":
            c.close()
            print("RESULT interrupted at %d%% (connection dropped by the client)" % (sent * 100 // len(data))); return 0
        if not ok:
            c.settimeout(70)
            for _ in range(5):
                r = c.recv(64).decode(errors="replace")
                print("device said:", r.strip()[:60])
                if "OK" in r:
                    ok = True; break
                if not r:
                    break
    except Exception as e:
        print("RESULT transfer ended with", type(e).__name__, "at %d%%" % (sent * 100 // len(data))); return 0
    print("RESULT transfer finished; device answered OK = %s" % ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
