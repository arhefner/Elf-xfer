#!/usr/bin/env python3
"""
mem-xfr protocol tests. Run with "make test", or directly:

    python3 test-mem-xfr.py

Needs no 1802 and no serial hardware: it builds a pty, puts the real mem-xfr
binary on one end, and drives the other end itself.

WHAT THIS IS FOR

The mocks below are Python transcriptions of loadbin (max_mon.asm:716) and
savebin (max_mon.asm:815), and mem-xfr is run against THEM rather than
against another copy of itself. That distinction is the whole point: a
mem-xfr-to-mem-xfr round trip passes happily with the handshake inverted on
both sides, so it cannot tell you the wire format is right. Only a peer that
independently implements what the 1802 actually does can.

    mock_loadbin   peer for "mem-xfr -s"
    mock_savebin   peer for "mem-xfr -r"

SOME TESTS ASSERT ON TIMING, DELIBERATELY

A bit-banged UART has no hold register. f_bread latches the first falling
edge it sees as a start bit, with no requirement that the line have been
idle beforehand -- so a byte that arrives before the 1802 is inside its own
polling loop is lost outright, not queued. mem-xfr therefore has to pace in
two opposite directions: "-d" microseconds AFTER each byte it originates,
and BEFORE each echo or ack it sends in reply.

A pty has none of that timing, so the functional result cannot show a
pacing fault at all. This was not theoretical: mem-xfr shipped with the
terminating 'x' of a receive unpaced, savebin hung on real hardware waiting
for a byte that had already gone past, and on a pty the faulty build still
exited 0 with every data byte intact. What exposes it is measuring the idle
gap before the reply (mock_savebin's gap_before_x) and asserting it is
roughly "-d". Keep that habit for any new reply byte.
"""

import os
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MEMXFR = os.path.join(HERE, "mem-xfr")

CMD_BLOCK = 0x01
CMD_END = 0x00
ACK = 0xAA
SYNC = 0x55
OVER = ord('x')

failures = []
passes = []

# Artifacts go to a scratch directory, never next to the source.
TMP = tempfile.mkdtemp(prefix="mem-xfr-test.")


def p(name):
    return os.path.join(TMP, name)


class ProtocolError(Exception):
    pass


class Wire:
    """One end of a pty, a byte at a time, with timeouts."""

    def __init__(self, fd):
        self.fd = fd

    def read(self, n, timeout=10.0):
        buf = b""
        end = time.time() + timeout
        while len(buf) < n:
            left = end - time.time()
            if left <= 0:
                raise ProtocolError(
                    "timeout: wanted %d bytes, got %d (%r)" % (n, len(buf), buf))
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, n - len(buf))
            except OSError:
                raise ProtocolError("pty closed after %d of %d bytes" %
                                    (len(buf), n))
            if not chunk:
                raise ProtocolError("EOF after %d of %d bytes" % (len(buf), n))
            buf += chunk
        return buf

    def rb(self):
        return self.read(1)[0]

    def expect(self, value, what):
        got = self.rb()
        if got != value:
            raise ProtocolError("%s: got %02x, expected %02x" %
                                (what, got, value))
        return got

    def w(self, b):
        if isinstance(b, int):
            b = bytes([b])
        os.write(self.fd, b)


def mock_loadbin(w, ra=0):
    """max_mon.asm:716. Returns {address: byte} of everything stored."""
    w.expect(SYNC, "loadbin handshake")
    w.w(ACK)

    mem = {}
    blocks = []
    while True:
        cmd = w.rb()
        if cmd == CMD_END:          # bz lbover -- NOT echoed
            break
        w.w(cmd)                    # call f_type, before the check
        if cmd != CMD_BLOCK:
            raise ProtocolError("loadbin: bad command %02x" % cmd)

        hi = w.rb(); w.w(hi)
        lo = w.rb(); w.w(lo)
        count = (hi << 8) | lo

        ahi = w.rb(); w.w(ahi)
        alo = w.rb(); w.w(alo)      # echoes the pre-offset value (plo r8)
        address = (ahi << 8) | alo

        data = w.read(count, timeout=20.0)
        dest = (address + ra) & 0xFFFF
        for i, byte in enumerate(data):
            mem[(dest + i) & 0xFFFF] = byte
        blocks.append((address, count))

        w.w(ACK)

    w.expect(OVER, "loadbin terminator")
    return {"mem": mem, "blocks": blocks}


def mock_savebin(w, address, data):
    """max_mon.asm:815. Sends data as savebin would, from `address`."""
    w.expect(ACK, "savebin handshake")
    w.w(SYNC)

    addr = address
    remaining = len(data)
    off = 0
    blocks = []
    while remaining:
        blk = 512 if remaining >= 512 else remaining
        remaining -= blk

        for value, what in ((CMD_BLOCK, "cmd"),
                            ((blk >> 8) & 0xFF, "count hi"),
                            (blk & 0xFF, "count lo"),
                            ((addr >> 8) & 0xFF, "addr hi"),
                            (addr & 0xFF, "addr lo")):
            w.w(value)
            w.expect(value, "savebin echo of %s" % what)

        w.w(data[off:off + blk])
        w.expect(ACK, "savebin block ack")

        blocks.append((addr, blk))
        addr = (addr + blk) & 0xFFFF
        off += blk

    w.w(CMD_END)
    # Time the idle gap before the terminator arrives. savebin only calls
    # f_read AFTER f_type has sent this $00, and f_bread latches the first
    # falling edge it sees with no idle-time requirement of its own -- so a
    # terminator that arrives instantly is one a bit-banged UART would miss
    # entirely. Measuring the gap is what makes that assertable from here; a
    # mock that merely reads the byte with a generous timeout cannot see the
    # fault at all, and didn't -- see this file's own header comment.
    t0 = time.time()
    w.expect(OVER, "savebin terminator")
    return {"blocks": blocks, "gap_before_x": time.time() - t0}


def mock_savebin_trailing(w, address, data, trailer=b"done\r\n> "):
    """savebin, plus what the MONITOR prints once savebin has returned.

    s_cmd emits "done" and then the ">" prompt after savebin returns, which
    happens while mem-xfr is still finishing up and still owns the port. Those
    bytes belong to whoever takes the port back (minicom), and mem-xfr must
    not consume or discard them.
    """
    res = mock_savebin(w, address, data)
    w.w(trailer)
    res["trailer"] = trailer
    return res


# Whatever was still queued on the slave side after mem-xfr exited, for the
# most recent run(..., catch_trailing=True). See that test for why.
LAST_TRAILING = b""


def run(args, mock, mockargs=(), timeout=25, catch_trailing=False):
    """Run mem-xfr with stdin/stdout on a pty; drive `mock` from the parent.

    catch_trailing keeps a second descriptor open on the slave so that bytes
    the far end sent near the end of the session can be read back AFTER
    mem-xfr has exited -- which is what minicom does, holding the port while
    handing it to a transfer program. Without a second holder there is no
    way to tell "mem-xfr discarded it" from "the pty went away with it".
    """
    global LAST_TRAILING

    LAST_TRAILING = b""
    master, slave = pty.openpty()
    tty.setraw(slave)
    tty.setraw(master)

    proc = subprocess.Popen([MEMXFR] + args, stdin=slave, stdout=slave,
                            stderr=subprocess.PIPE)
    keep = os.dup(slave) if catch_trailing else None
    os.close(slave)

    w = Wire(master)
    result, mock_err = None, None
    try:
        result = mock(w, *mockargs)
    except Exception as exc:                       # noqa: BLE001
        mock_err = exc

    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        if mock_err is None:
            mock_err = ProtocolError("mem-xfr did not exit")

    stderr = proc.stderr.read().decode(errors="replace")
    proc.stderr.close()

    if keep is not None:
        # Read whatever is still queued on the slave now that mem-xfr has
        # gone. If its tty_reset() flushed the input queue, this is empty.
        try:
            while True:
                r, _, _ = select.select([keep], [], [], 0.3)
                if not r:
                    break
                chunk = os.read(keep, 4096)
                if not chunk:
                    break
                LAST_TRAILING += chunk
        except OSError:
            pass
        os.close(keep)

    os.close(master)
    return proc.returncode, stderr, result, mock_err


def relay(fd_a, fd_b, stop):
    """Crossed-cable bridge between two pty masters, for the self round trip."""
    while not stop[0]:
        r, _, _ = select.select([fd_a, fd_b], [], [], 0.05)
        for fd in r:
            try:
                data = os.read(fd, 4096)
            except OSError:
                stop[0] = True
                return
            if not data:
                stop[0] = True
                return
            try:
                os.write(fd_b if fd is fd_a else fd_a, data)
            except OSError:
                stop[0] = True
                return


def run_pair(send_args, recv_args, timeout=40):
    """mem-xfr -s against mem-xfr -r over two bridged ptys."""
    m1, s1 = pty.openpty()
    m2, s2 = pty.openpty()
    for fd in (s1, s2, m1, m2):
        tty.setraw(fd)

    recv = subprocess.Popen([MEMXFR] + recv_args, stdin=s2, stdout=s2,
                            stderr=subprocess.PIPE)
    send = subprocess.Popen([MEMXFR] + send_args, stdin=s1, stdout=s1,
                            stderr=subprocess.PIPE)
    os.close(s1)
    os.close(s2)

    # Start the relay only once BOTH processes are certain to be past their
    # own tty_raw(), which calls tcsetattr(..., TCSAFLUSH, ...) and so
    # DISCARDS anything already pending on its input queue.
    #
    # With the relay running first, mem-xfr -r's opening $AA was forwarded
    # into the sender's input queue and the sender's own tty_raw() then
    # flushed it away -- after which -s waited out the full 25.5s timeout for
    # a handshake ack already thrown on the floor, while -r sat waiting for a
    # command byte (it had received the $55 quite happily). Intermittent at
    # about half of all runs, depending purely on who won the race.
    #
    # This is an artifact of pointing two HOSTS at each other, not a defect
    # against the 1802: mem-xfr speaks first in both directions ($55 for -s,
    # $AA for -r) and loadbin/savebin only ever reply after that opening
    # byte -- always after tty_raw() has run -- so on a real link there is
    # never a byte in flight for the flush to eat.
    time.sleep(0.6)

    stop = [False]
    thread = threading.Thread(target=relay, args=(m1, m2, stop), daemon=True)
    thread.start()

    rcs = {}
    try:
        rcs["send"] = send.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        send.kill()
        rcs["send"] = -1
    try:
        rcs["recv"] = recv.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        recv.kill()
        rcs["recv"] = -1

    stop[0] = True
    thread.join(timeout=2)
    errs = {"send": send.stderr.read().decode(errors="replace"),
            "recv": recv.stderr.read().decode(errors="replace")}
    send.stderr.close()
    recv.stderr.close()
    os.close(m1)
    os.close(m2)
    return rcs, errs


def hexfile(path, records):
    """records: [(address, bytes)] -> an Intel hex file with real checksums."""
    with open(path, "w") as fp:
        for addr, data in records:
            for i in range(0, len(data), 16):
                part = data[i:i + 16]
                a = addr + i
                fields = [len(part), (a >> 8) & 0xFF, a & 0xFF, 0x00]
                fields += list(part)
                cks = (-sum(fields)) & 0xFF
                fp.write(":%02X%04X00%s%02X\n" % (
                    len(part), a, "".join("%02X" % b for b in part), cks))
        fp.write(":00000001FF\n")


def record(rtype, addr, data):
    """One Intel hex record of any type, with a correct checksum."""
    fields = [len(data), (addr >> 8) & 0xFF, addr & 0xFF, rtype]
    fields += list(data)
    return ":%s%02X\n" % ("".join("%02X" % b for b in fields),
                          (-sum(fields)) & 0xFF)


def check(name, cond, detail=""):
    if cond:
        passes.append(name)
        print("  PASS  %s" % name)
    else:
        failures.append((name, detail))
        print("  FAIL  %s\n          %s" % (name, detail))


def main():
    if not os.path.exists(MEMXFR):
        print("mem-xfr not built: %s\nRun make first." % MEMXFR)
        return 2

    print("=== send: binary -> loadbin ===")
    payload = bytes((i * 7 + 3) & 0xFF for i in range(1100))
    with open(p("in.bin"), "wb") as fp:
        fp.write(payload)

    rc, err, res, merr = run(["-s", "-d", "0", "-a", "0x3000", p("in.bin")],
                             mock_loadbin)
    check("send binary: mock completed", merr is None, repr(merr))
    check("send binary: exit 0", rc == 0, "rc=%r err=%s" % (rc, err))
    if res:
        expect = {0x3000 + i: b for i, b in enumerate(payload)}
        check("send binary: bytes land at 0x3000", res["mem"] == expect,
              "got %d bytes, expected %d" % (len(res["mem"]), len(expect)))
        check("send binary: 512-byte blocking",
              res["blocks"] == [(0x3000, 512), (0x3200, 512), (0x3400, 76)],
              repr(res["blocks"]))

    print("=== send: -o/-l slice ===")
    rc, err, res, merr = run(
        ["-s", "-d", "0", "-a", "0x2000", "-o", "10", "-l", "20",
         p("in.bin")], mock_loadbin)
    check("send slice: mock completed", merr is None, repr(merr))
    check("send slice: exit 0", rc == 0, "rc=%r err=%s" % (rc, err))
    if res:
        expect = {0x2000 + i: b for i, b in enumerate(payload[10:30])}
        check("send slice: correct 20 bytes at 0x2000", res["mem"] == expect,
              repr(sorted(res["mem"].items())[:4]))

    print("=== send: loadbin's own ra offset ===")
    rc, err, res, merr = run(["-s", "-d", "0", "-a", "0x0100", "-l", "16",
                              p("in.bin")], mock_loadbin, (0x4000,))
    check("send with ra: mock completed (echo of pre-offset low byte)",
          merr is None, repr(merr))
    if res:
        expect = {0x4100 + i: b for i, b in enumerate(payload[:16])}
        check("send with ra: ra added on the 1802 side", res["mem"] == expect,
              repr(sorted(res["mem"].items())[:3]))

    print("=== send: sparse Intel hex keeps its gaps ===")
    lo_data = bytes(range(16))
    hi_data = bytes(range(0x80, 0x90))
    hexfile(p("sparse.hex"), [(0x0100, lo_data), (0x0900, hi_data)])

    rc, err, res, merr = run(["-s", "-x", "-d", "0", p("sparse.hex")],
                             mock_loadbin)
    check("sparse hex: mock completed", merr is None, repr(merr))
    check("sparse hex: exit 0", rc == 0, "rc=%r err=%s" % (rc, err))
    if res:
        check("sparse hex: two blocks, gap not transmitted",
              res["blocks"] == [(0x0100, 16), (0x0900, 16)],
              repr(res["blocks"]))
        check("sparse hex: only 32 bytes total", len(res["mem"]) == 32,
              "%d bytes" % len(res["mem"]))

    print("=== send: -a clips a hex image ===")
    rc, err, res, merr = run(["-s", "-x", "-d", "0", "-a", "0x900",
                              p("sparse.hex")], mock_loadbin)
    check("hex -a clip: mock completed", merr is None, repr(merr))
    if res:
        check("hex -a clip: only the high run sent",
              res["blocks"] == [(0x0900, 16)], repr(res["blocks"]))

    print("=== send: -o relocates a hex image ===")
    rc, err, res, merr = run(["-s", "-x", "-d", "0", "-o", "0x1000",
                              p("sparse.hex")], mock_loadbin)
    check("hex -o reloc: mock completed", merr is None, repr(merr))
    if res:
        check("hex -o reloc: addresses shifted by 0x1000",
              res["blocks"] == [(0x1100, 16), (0x1900, 16)],
              repr(res["blocks"]))

    print("=== send: non-data record types ===")
    body = bytes(range(16))
    for name, extra, should_pass, why in (
            ("t05.hex", record(0x05, 0, b"\x00\x00\x01\x00"), True,
             "type 05 (start linear addr) skipped, as asm/02 emits"),
            ("t03.hex", record(0x03, 0, b"\x00\x00\x01\x00"), True,
             "type 03 (start segment addr) skipped"),
            ("t04zero.hex", record(0x04, 0, b"\x00\x00"), True,
             "type 04 with zero base accepted"),
            ("t02zero.hex", record(0x02, 0, b"\x00\x00"), True,
             "type 02 with zero base accepted"),
            ("t04nz.hex", record(0x04, 0, b"\x00\x01"), False,
             "type 04 with nonzero base rejected"),
            ("t06.hex", record(0x06, 0, b"\x00"), False,
             "unknown record type 06 rejected"),
    ):
        path = p(name)
        with open(path, "w") as fp:
            fp.write(record(0x00, 0x0100, body))
            fp.write(extra)
            fp.write(":00000001FF\n")

        if should_pass:
            rc, err, res, merr = run(["-s", "-x", "-d", "0", path],
                                     mock_loadbin)
            ok = (rc == 0 and merr is None and res is not None and
                  res["mem"] == {0x0100 + i: b for i, b in enumerate(body)})
            check(why, ok, "rc=%r merr=%r err=%s" % (rc, merr, err.strip()))
        else:
            # mem-xfr parses the file before it ever touches the tty, so a
            # parse failure is observable without a pty at all.
            q = subprocess.run([MEMXFR, "-s", "-x", "-d", "0", path],
                               capture_output=True)
            check(why, q.returncode != 0,
                  "rc=%d %s" % (q.returncode, q.stderr.decode()[:140]))

    print("=== send: real ledclock.hex (sparse, carries a type 05) ===")
    real = os.path.join(REPO, "ledclock.hex")
    if os.path.exists(real):
        rc, err, res, merr = run(["-s", "-x", "-v", "-d", "0", real],
                                 mock_loadbin, timeout=60)
        expect_runs = [(0x0100, 230), (0x0200, 20), (0x0221, 8),
                       (0x0300, 218), (0x03E0, 16), (0x03F1, 17),
                       (0x0500, 262), (0x0700, 204), (0x0800, 199)]
        check("ledclock.hex: exit 0", rc == 0,
              "rc=%r merr=%r err=%s" % (rc, merr, err.strip()[:200]))
        check("ledclock.hex: 9 runs, gaps preserved",
              res is not None and res["blocks"] == expect_runs,
              repr(res and res["blocks"]))
        check("ledclock.hex: 1174 data bytes",
              res is not None and len(res["mem"]) == 1174,
              repr(res and len(res["mem"])))
        check("ledclock.hex: -v names entry point 0100h",
              "entry point 0100h" in err, err.strip()[:200])
    else:
        # *.hex is gitignored, so a fresh clone legitimately has no copy.
        print("  SKIP  ledclock.hex not present (it is gitignored)")

    print("=== receive: savebin -> binary ===")
    out = p("out.bin")
    rc, err, res, merr = run(["-r", "-d", "0", out], mock_savebin,
                             (0x0300, payload))
    check("recv binary: mock completed", merr is None, repr(merr))
    check("recv binary: exit 0", rc == 0, "rc=%r err=%s" % (rc, err))
    if os.path.exists(out):
        got = open(out, "rb").read()
        check("recv binary: file matches payload", got == payload,
              "%d bytes vs %d" % (len(got), len(payload)))

    print("=== receive: -a/-l assertions ===")
    rc, err, res, merr = run(["-r", "-d", "0", "-a", "0x300", "-l", "1100",
                              out], mock_savebin, (0x0300, payload))
    check("recv -a/-l matching: exit 0", rc == 0, "rc=%r err=%s" % (rc, err))

    rc, err, res, merr = run(["-r", "-d", "0", "-a", "0x400", out],
                             mock_savebin, (0x0300, payload))
    check("recv -a mismatch: exit nonzero", rc != 0, "rc=%r" % rc)
    check("recv -a mismatch: says so", "-a said 0400h" in err, err.strip())

    rc, err, res, merr = run(["-r", "-d", "0", "-l", "999", out],
                             mock_savebin, (0x0300, payload))
    check("recv -l mismatch: exit nonzero", rc != 0, "rc=%r" % rc)
    check("recv -l mismatch: says so", "-l said 999" in err, err.strip())

    print("=== receive: -x writes absolute addresses ===")
    outhex = p("out.hex")
    small = bytes(range(32))
    rc, err, res, merr = run(["-r", "-x", "-d", "0", outhex], mock_savebin,
                             (0x0300, small))
    check("recv hex: exit 0", rc == 0, "rc=%r err=%s" % (rc, err))
    if os.path.exists(outhex):
        text = open(outhex).read()
        check("recv hex: first record at 0300h", ":200300" in text,
              repr(text.strip().splitlines()[:2]))
        check("recv hex: has EOF record", ":00000001FF" in text,
              text.strip()[-40:])
        rc2, err2, res2, merr2 = run(["-s", "-x", "-d", "0", outhex],
                                     mock_loadbin)
        check("recv hex: re-sending it reproduces the bytes",
              res2 is not None and res2["mem"] ==
              {0x0300 + i: b for i, b in enumerate(small)},
              repr(res2 and sorted(res2["mem"].items())[:3]))

    print("=== receive: -o patches an existing file in place ===")
    patchfile = p("patch.bin")
    with open(patchfile, "wb") as fp:
        fp.write(b"\xEE" * 64)
    rc, err, res, merr = run(["-r", "-d", "0", "-o", "16", patchfile],
                             mock_savebin, (0x0300, bytes(range(16))))
    check("recv -o: exit 0", rc == 0, "rc=%r err=%s" % (rc, err))
    if os.path.exists(patchfile):
        got = open(patchfile, "rb").read()
        check("recv -o: leading bytes preserved (not truncated)",
              got[:16] == b"\xEE" * 16, repr(got[:20]))
        check("recv -o: data written at offset 16",
              got[16:32] == bytes(range(16)), repr(got[16:32]))
        check("recv -o: trailing bytes preserved",
              len(got) == 64 and got[32:] == b"\xEE" * 32, "len=%d" % len(got))

    print("=== receive: the trailing 'x' must be paced ===")
    # Regression test for a real hardware fault: recv_image sent the
    # terminator with send_byte (delay AFTER the write) instead of
    # reply_byte (delay BEFORE it), so it arrived with no lead-in and
    # savebin's f_read never caught its start bit. The functional result
    # cannot see this -- on a pty the faulty build still exits 0 with every
    # byte intact -- so the gap itself is what gets asserted. -d is large
    # here so the measurement is unambiguous against scheduler noise.
    delay_us = 8000
    rc, err, res, merr = run(["-r", "-d", str(delay_us), p("xpace.bin")],
                             mock_savebin, (0x0300, bytes(range(64))),
                             timeout=60)
    check("paced 'x': exit 0", rc == 0,
          "rc=%r merr=%r err=%s" % (rc, merr, err.strip()))
    if res:
        gap = res["gap_before_x"]
        want = 0.7 * delay_us / 1e6
        check("paced 'x': preceded by an idle gap of about -d", gap >= want,
              "gap was %.5fs, wanted >= %.5fs (the fault shows as ~0)" %
              (gap, want))

    print("=== receive: the far end's closing output must survive our exit ===")
    # Regression test for a real hardware symptom: a receive worked, savebin
    # returned and the monitor went back to its prompt, but the monitor's
    # "done" and ">" never appeared on screen. tty_reset() used TCSAFLUSH,
    # which discards unread input -- and by that point the unread input was
    # exactly those bytes. mem-xfr was eating the far end's closing output
    # itself, just before handing the port back.
    #
    # -x is deliberate: store_hex walks all 65536 addresses, which reliably
    # keeps mem-xfr busy long enough for the trailer to land BEFORE
    # tty_reset() runs. With a small binary payload the window is too tight
    # for the test to mean anything.
    rc, err, res, merr = run(["-r", "-x", "-d", "0", p("trail.hex")],
                             mock_savebin_trailing,
                             (0x0300, bytes(range(64))),
                             timeout=60, catch_trailing=True)
    check("closing output: exit 0", rc == 0,
          "rc=%r merr=%r err=%s" % (rc, merr, err.strip()))
    check("closing output: the far end's 'done' is not discarded",
          b"done" in LAST_TRAILING,
          "still queued after exit = %r (empty means tty_reset flushed it)"
          % (LAST_TRAILING,))

    print("=== exact block-size boundaries ===")
    for size, want in ((512, [(0x1000, 512)]),
                       (513, [(0x1000, 512), (0x1200, 1)])):
        data = bytes(range(256)) * 3
        with open(p("b.bin"), "wb") as fp:
            fp.write(data[:size])
        rc, err, res, merr = run(["-s", "-d", "0", "-a", "0x1000",
                                  p("b.bin")], mock_loadbin)
        check("send %d bytes: blocking %r" % (size, want),
              res is not None and res["blocks"] == want,
              repr(res and res["blocks"]))

    print("=== mem-xfr -s <-> mem-xfr -r ===")
    selfout = p("self.bin")
    rcs, errs = run_pair(["-s", "-d", "0", "-a", "0x0500", p("in.bin")],
                         ["-r", "-d", "0", "-a", "0x0500", selfout])
    check("self round trip: both exit 0",
          rcs.get("send") == 0 and rcs.get("recv") == 0,
          "rcs=%r send_err=%s recv_err=%s" % (rcs, errs["send"], errs["recv"]))
    if os.path.exists(selfout):
        check("self round trip: bytes identical",
              open(selfout, "rb").read() == payload,
              "%d bytes" % os.path.getsize(selfout))

    print("=== argument validation ===")
    for args, why in (
            (["-s"], "no file"),
            (["-r"], "no file given to -r"),
            ([p("in.bin")], "neither -s nor -r"),
            (["-s", "-a", "zzz", p("in.bin")], "bad -a"),
            (["-s", "-l", "0", p("in.bin")], "-l 0"),
            (["-s", "-a", "0x10000", p("in.bin")], "-a past 64K"),
            (["-s", "-o", "99999", p("in.bin")], "-o past EOF"),
            (["-s", "-a", "0xFFF0", p("in.bin")], "length running past 64K"),
    ):
        q = subprocess.run([MEMXFR] + args, capture_output=True)
        check("rejects %s" % why, q.returncode != 0,
              "rc=%d out=%s" % (q.returncode, q.stderr.decode()[:120]))

    print("\n%d passed, %d failed" % (len(passes), len(failures)))
    for name, detail in failures:
        print("FAILED: %s -- %s" % (name, detail))
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        status = main()
    finally:
        if failures:
            # Leave the artifacts behind when something failed; they are
            # usually what you want to look at next.
            print("artifacts kept in %s" % TMP)
        else:
            shutil.rmtree(TMP, ignore_errors=True)
    sys.exit(status)
