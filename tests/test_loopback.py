#!/usr/bin/env python3
"""Integration test for tools/m17bridge: KISS in, loopback, KISS out.

Starts the bridge on ephemeral ports, connects two KISS clients and checks that
a frame sent by one arrives intact at the other, that `disable` stops it, and
that the control console reports plausible state.

BSD Zero Clause License.  See LICENSE.
"""
import os
import socket
import subprocess
import sys
import threading
import time

FEND = 0xC0
FESC = 0xDB

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "tools", "m17bridge")

failures = 0
checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)


def kiss_encode(frame, port=0, cmd=0):
    out = bytearray([FEND, (port << 4) | cmd])
    for b in frame:
        if b == FEND:
            out += bytes([FESC, 0xDC])
        elif b == FESC:
            out += bytes([FESC, 0xDD])
        else:
            out.append(b)
    out.append(FEND)
    return bytes(out)


class Reader:
    def __init__(self, sock):
        self.s = sock
        self.buf = bytearray()

    def _parse(self):
        while self.buf and self.buf[0] != FEND:
            del self.buf[0]
        if not self.buf:
            return None
        try:
            end = self.buf.index(FEND, 1)
        except ValueError:
            return None
        raw = bytes(self.buf[1:end])
        del self.buf[: end + 1]
        if not raw:
            return None
        header = raw[0]
        payload = bytearray()
        esc = False
        for b in raw[1:]:
            if esc:
                # KISS uses explicit codes, not an XOR: DC -> FEND, DD -> FESC
                payload.append(FEND if b == 0xDC else FESC if b == 0xDD else b ^ 0x20)
                esc = False
            elif b == FESC:
                esc = True
            else:
                payload.append(b)
        return header >> 4, header & 0x0F, bytes(payload)

    def frame(self, timeout=2.0):
        deadline = time.time() + timeout
        while True:
            f = self._parse()
            if f is not None:
                return f
            left = deadline - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                data = self.s.recv(4096)
            except socket.timeout:
                return None
            if not data:
                return None
            self.buf += data


def connect(port):
    deadline = time.time() + 5.0
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2.0)
            s.settimeout(2.0)
            return s
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("could not connect to port %d" % port)


def command(ctl, text):
    ctl.sendall((text + "\n").encode())
    buf = b""
    deadline = time.time() + 2.0
    while time.time() < deadline:
        try:
            data = ctl.recv(4096)
        except socket.timeout:
            break
        if not data:
            break
        buf += data
        if b"\n" in buf:
            break
    return buf.decode(errors="replace").strip()


def main():
    proc = subprocess.Popen(
        [BIN, "--kiss-port", "0", "--control-port", "0",
         "--enable", "1", "--loopback", "1"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)

    lines = []
    stdout = proc.stdout
    assert stdout is not None

    def drain():
        for line in stdout:
            lines.append(line.rstrip())

    threading.Thread(target=drain, daemon=True).start()

    kiss_port = ctl_port = None
    deadline = time.time() + 5.0
    while time.time() < deadline and kiss_port is None:
        for line in list(lines):
            if "kiss_port=" in line:
                for field in line.split():
                    if field.startswith("kiss_port="):
                        kiss_port = int(field.split("=")[1])
                    elif field.startswith("control_port="):
                        ctl_port = int(field.split("=")[1])
        time.sleep(0.02)

    check(kiss_port is not None and ctl_port is not None, "bridge reported its ports")
    if kiss_port is None or ctl_port is None:
        proc.kill()
        return 1

    a = connect(kiss_port)
    b = connect(kiss_port)
    ctl = connect(ctl_port)
    ra, rb = Reader(a), Reader(b)
    time.sleep(0.1)  # let the bridge register both clients

    # 1. a small frame crosses intact
    frame = bytes(range(0x82, 0x82 + 12))
    a.sendall(kiss_encode(frame))
    got = rb.frame()
    check(got is not None, "small frame delivered")
    if got:
        check(got[0] == 0 and got[1] == 0, "delivered on port 0 command 0")
        check(got[2] == frame, "small frame payload unchanged")

    # 2. a large frame with escape bytes crosses intact
    big = bytes([0xC0, 0xDB, 0x7E] * 200)  # 600 bytes, forces 25 frames
    a.sendall(kiss_encode(big))
    got = rb.frame()
    check(got is not None and got[2] == big, "600-byte frame with escapes intact")

    # 3. status reports the connection and the counters
    status = command(ctl, "status")
    check(status.startswith("status"), "status responds")
    check("enabled=1" in status, "status shows enabled")
    check("clients=2" in status, "status shows two clients")
    check("delivered=2" in status, "status counts deliveries")

    # 4. disable stops transmission
    check(command(ctl, "disable") == "ok disabled", "disable acknowledged")
    a.sendall(kiss_encode(b"should not appear"))
    check(rb.frame(0.6) is None, "no delivery while disabled")

    # 5. enable resumes it
    check(command(ctl, "enable") == "ok enabled", "enable acknowledged")
    a.sendall(kiss_encode(b"after enable"))
    got = rb.frame()
    check(got is not None and got[2] == b"after enable", "delivery resumes after enable")

    # 6. loopback off means nothing is delivered, but the frame is counted
    check(command(ctl, "loopback off") == "ok loopback=off", "loopback off acknowledged")
    a.sendall(kiss_encode(b"discarded"))
    check(rb.frame(0.6) is None, "no delivery with loopback off")
    status = command(ctl, "status")
    check("noradio=1" in status, "loopback off counted the frame as not transmitted")

    # 7. bad command is rejected but the bridge stays up
    check(command(ctl, "frobnicate").startswith("error"), "unknown command rejected")
    check(command(ctl, "help").startswith("commands:"), "help lists commands")

    # 8. clean shutdown
    check(command(ctl, "quit") == "ok bye", "quit acknowledged")
    try:
        proc.wait(timeout=5.0)
        check(proc.returncode == 0, "clean exit status")
    except subprocess.TimeoutExpired:
        check(False, "bridge exited after quit")
        proc.kill()

    print("%d checks, %d failure%s" % (checks, failures, "" if failures == 1 else "s"))
    if failures:
        print("\n--- bridge log ---")
        print("\n".join(lines[-40:]))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
