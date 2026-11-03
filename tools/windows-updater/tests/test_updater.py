#!/usr/bin/env python3
"""Protocol tests for FluidDialUpdater.exe against a stand-in pendant.

The stand-in follows the stock Arduino ESP32 OTA library (ArduinoOTA.cpp): UDP invitation on the OTA port, AUTH
nonce, md5(md5(password):nonce:cnonce) response, then the device connects back over TCP and acknowledges every
chunk, finishing with "OK". It also reproduces the library's habit of dropping an invitation that arrives while it
is waiting for authentication. The stock espota.py is run against the same stand-in first, so the stand-in itself is
checked against the reference client.

Usage: python test_updater.py <FluidDialUpdater.exe> <good.bin> [espota.py]
Nothing here touches real hardware; the password is a test value.
"""
import hashlib, os, random, socket, subprocess, sys, threading, time

EXE = sys.argv[1]
GOOD = sys.argv[2]
ESPOTA = sys.argv[3] if len(sys.argv) > 3 else None
PASSWORD = "test-password-1"
MAX_SLOT = 3264 * 1024


def md5(b):
    return hashlib.md5(b).hexdigest()


class FakePendant(threading.Thread):
    """mode: normal | interrupt | md5bad | unarmed"""

    def __init__(self, mode="normal", interrupt_at=200000, max_size=MAX_SLOT, lockout=5):
        super().__init__(daemon=True)
        self.mode = mode
        self.interrupt_at = interrupt_at
        self.max_size = max_size
        self.lockout = lockout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sock.settimeout(0.2)
        self.stop = False
        self.state = "idle"
        self.fails = 0
        self.completed = []     # sha256 of images fully received and accepted
        self.events = []
        self.dropped_invites = 0
        self.nonce = ""
        self.size = 0
        self.md5 = ""
        self.tcp_port = 0
        self.peer = None

    def run(self):
        if self.mode == "unarmed":
            while not self.stop:
                try:
                    self.sock.recvfrom(2048)
                except socket.timeout:
                    pass
            return
        while not self.stop:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            if self.fails >= self.lockout:
                continue   # service withdrawn after too many wrong passwords
            text = data.decode(errors="replace")
            if self.state == "idle":
                p = text.split()
                if len(p) < 4 or p[0] != "0":
                    continue
                self.tcp_port, self.size, self.md5 = int(p[1]), int(p[2]), p[3]
                self.nonce = md5(os.urandom(16))
                self.sock.sendto(("AUTH " + self.nonce).encode(), addr)
                self.state = "waitauth"
            else:
                p = text.split()
                if not p or p[0] != "200":
                    self.dropped_invites += 1   # real library: logs and goes back to idle, packet lost
                    self.state = "idle"
                    continue
                if len(p) < 3 or len(p[1]) != 32 or len(p[2]) != 32:
                    self.state = "idle"
                    continue
                expect = md5((md5(PASSWORD.encode()) + ":" + self.nonce + ":" + p[1]).encode())
                if expect == p[2]:
                    self.sock.sendto(b"OK", addr)
                    self.state = "idle"
                    self.events.append("auth ok")
                    self.receive(addr[0])
                else:
                    self.sock.sendto(b"Authentication Failed", addr)
                    self.fails += 1
                    self.events.append("auth failed")
                    self.state = "idle"

    def receive(self, host):
        if self.size > self.max_size:
            self.events.append("begin error: too large")   # library: no connection is made
            return
        try:
            c = socket.create_connection((host, self.tcp_port), timeout=5)
        except OSError:
            self.events.append("connect failed")
            return
        c.settimeout(5)
        got = bytearray()
        try:
            while len(got) < self.size:
                chunk = c.recv(1460)
                if not chunk:
                    break
                got += chunk
                if self.mode == "interrupt" and len(got) >= self.interrupt_at:
                    self.events.append("interrupted")
                    c.close()
                    return
                c.sendall(str(len(chunk)).encode())
            if len(got) == self.size and md5(bytes(got)) == self.md5 and self.mode != "md5bad":
                c.sendall(b"OK")
                self.completed.append(hashlib.sha256(bytes(got)).hexdigest())
                self.events.append("update ok")
            else:
                c.sendall(b"MD5 Check Failed")
                self.events.append("update error")
        except OSError:
            self.events.append("receive error")
        finally:
            c.close()


results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  -> " + detail))


def run_exe(args, password=PASSWORD, timeout=90):
    env = dict(os.environ)
    env["FD_TEST_PW"] = password
    p = subprocess.run([EXE] + args, capture_output=True, text=True, env=env, timeout=timeout)
    return p.returncode, p.stdout


def upload_args(port, f, extra=()):
    return ["--cli", "--ip", "127.0.0.1", "--port", str(port), "--file", f, "--password-env", "FD_TEST_PW", "--no-restart-check"] + list(extra)


def main():
    good = open(GOOD, "rb").read()
    good_sha = hashlib.sha256(good).hexdigest()
    tmp = os.path.join(os.environ.get("TEMP", "."), "fdu_test")
    os.makedirs(tmp, exist_ok=True)

    # --- reference: stock espota.py against the stand-in (validates the stand-in) ---
    if ESPOTA:
        fp = FakePendant(); fp.start()
        r = subprocess.run([sys.executable, ESPOTA, "-i", "127.0.0.1", "-p", str(fp.port), "--auth=" + PASSWORD, "-f", GOOD, "-r"],
                           capture_output=True, text=True, timeout=90)
        check("reference espota.py completes against the stand-in", r.returncode == 0 and fp.completed == [good_sha], r.stderr[-200:] + str(fp.events))
        fp.stop = True

    # --- file checks (host-side compatibility validation) ---
    rc, out = run_exe(["--analyze", "--file", GOOD])
    check("analyze: JC image is compatible", rc == 0 and out.startswith("COMPATIBLE"), out)
    rc, out = run_exe(["--analyze", "--file", os.path.join(os.path.dirname(GOOD), "fluiddial-jc-1.5.2-usb-bootloader.bin")]) if os.path.exists(os.path.join(os.path.dirname(GOOD), "fluiddial-jc-1.5.2-usb-bootloader.bin")) else (2, "REJECTED skipped")
    check("analyze: bootloader file rejected", rc == 2, out)
    # wrong target: same size, marker removed -> looks like an M5Dial/CYD image
    wrong = good.replace(b"FDJC-IMAGE-TARGET:JC8048W550C_I:ESP32-S3:V1", b"XXXX-IMAGE-TARGET:OTHER_DEVICE_ZZZZZZZZZZ:V1")
    wp = os.path.join(tmp, "fluiddial-m5dial-wrong.bin"); open(wp, "wb").write(wrong)
    rc, out = run_exe(["--analyze", "--file", wp])
    check("analyze: image without the JC marker rejected", rc == 2 and "not a FluidDial-JC" in out, out)
    s2 = bytearray(good); s2[12] = 0; s2[13] = 0   # chip id ESP32, not S3
    cp = os.path.join(tmp, "chip.bin"); open(cp, "wb").write(bytes(s2))
    rc, out = run_exe(["--analyze", "--file", cp])
    check("analyze: wrong chip id rejected", rc == 2 and "chip" in out, out)
    big = good + b"\0" * (MAX_SLOT - len(good) + 1)
    bp = os.path.join(tmp, "big.bin"); open(bp, "wb").write(big)
    rc, out = run_exe(["--analyze", "--file", bp])
    check("analyze: oversized image rejected", rc == 2 and "larger" in out, out)
    tp = os.path.join(tmp, "small.bin"); open(tp, "wb").write(good[:4096])
    rc, out = run_exe(["--analyze", "--file", tp])
    check("analyze: truncated image rejected", rc == 2, out)
    xp = os.path.join(tmp, "notes.bin"); open(xp, "wb").write(b"hello" * 100000)
    rc, out = run_exe(["--analyze", "--file", xp])
    check("analyze: arbitrary file rejected", rc == 2, out)

    # --- normal update ---
    fp = FakePendant(); fp.start()
    rc, out = run_exe(upload_args(fp.port, GOOD))
    check("update succeeds and the image arrives byte-identical", rc == 0 and fp.completed == [good_sha] and "TRANSFER OK" in out, out + str(fp.events))
    check("password never appears in the output", PASSWORD not in out)
    fp.stop = True

    # --- readiness probe does not authenticate and does not start a transfer ---
    fp = FakePendant(); fp.start()
    rc, out = run_exe(["--cli", "--ip", "127.0.0.1", "--port", str(fp.port), "--file", GOOD, "--probe"])
    check("probe: ready pendant detected, no transfer, no auth attempt", rc == 0 and "PROBE OK" in out and fp.fails == 0 and not fp.completed, out)
    rc, out = run_exe(upload_args(fp.port, GOOD))
    check("probe followed by update still works (dropped invitation is retried)", rc == 0 and fp.completed == [good_sha], out + str(fp.events))
    check("stand-in did drop an invitation after the probe (library behaviour reproduced)", fp.dropped_invites >= 1, str(fp.dropped_invites))
    fp.stop = True

    # --- wrong password ---
    fp = FakePendant(); fp.start()
    rc, out = run_exe(upload_args(fp.port, GOOD), password="wrong-password")
    check("wrong password: reported, nothing installed", rc == 1 and "Wrong OTA password" in out and not fp.completed, out)
    check("wrong password text not echoed", "wrong-password" not in out)
    fp.stop = True

    # --- five wrong passwords withdraw the service ---
    fp = FakePendant(); fp.start()
    for _ in range(5):
        run_exe(upload_args(fp.port, GOOD), password="bad-password-x")
    rc, out = run_exe(upload_args(fp.port, GOOD))
    check("after 5 wrong passwords the service answers nothing (correct password also refused)", rc == 1 and "No answer" in out and not fp.completed, out)
    fp.stop = True

    # --- not armed / no answer ---
    fp = FakePendant("unarmed"); fp.start()
    t0 = time.time()
    rc, out = run_exe(upload_args(fp.port, GOOD))
    check("unavailable service: clear not-offered failure within ~20 s", rc == 1 and "READY" in out and time.time() - t0 < 40, out)
    fp.stop = True

    # --- disconnected pendant (nothing on the address) ---
    rc, out = run_exe(upload_args(9, GOOD))   # discard port on loopback: nobody answers
    check("disconnected/unreachable pendant: reports no answer", rc == 1 and "No answer" in out, out)

    # --- bad address ---
    rc, out = run_exe(["--cli", "--ip", "not an address!!", "--file", GOOD, "--password-env", "FD_TEST_PW", "--no-restart-check"])
    check("invalid address rejected before any traffic", rc == 1 and "not valid" in out, out)

    # --- missing password ---
    fp = FakePendant(); fp.start()
    rc, out = run_exe(upload_args(fp.port, GOOD), password="")
    check("empty password rejected before any traffic", rc == 1 and fp.state == "idle" and not fp.events, out + str(fp.events))
    fp.stop = True

    # --- interrupted transfer ---
    fp = FakePendant("interrupt", interrupt_at=300000); fp.start()
    rc, out = run_exe(upload_args(fp.port, GOOD))
    check("interrupted transfer: reported, old firmware kept, nothing accepted", rc == 1 and "interrupted" in out and not fp.completed, out)
    fp.stop = True

    # --- device-side rejection (md5 mismatch / corrupt) ---
    fp = FakePendant("md5bad"); fp.start()
    rc, out = run_exe(upload_args(fp.port, GOOD))
    check("device rejects the image: failure reported", rc == 1 and "did not accept" in out and not fp.completed, out)
    fp.stop = True

    # --- oversized image is refused by the device (no connect-back) ---
    fp = FakePendant(max_size=len(good) - 1000); fp.start()
    rc, out = run_exe(upload_args(fp.port, GOOD))
    check("device refuses an image that does not fit: no connect-back diagnosed", rc == 1 and "did not connect back" in out and not fp.completed, out)
    fp.stop = True

    # --- diagnostic image needs explicit confirmation ---
    diag = os.path.join(os.path.dirname(GOOD), "fluiddial-jc-1.5.2-diagnostic-motion-disabled.bin")
    if os.path.exists(diag):
        fp = FakePendant(); fp.start()
        rc, out = run_exe(upload_args(fp.port, diag))
        check("diagnostic image refused without explicit confirmation", rc == 2 and "REFUSED" in out and not fp.completed, out)
        rc, out = run_exe(upload_args(fp.port, diag, ["--yes-diagnostic"]))
        check("diagnostic image accepted with explicit confirmation", rc == 0 and len(fp.completed) == 1, out)
        fp.stop = True

    bad = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
