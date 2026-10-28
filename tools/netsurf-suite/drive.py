#!/usr/bin/env python3
"""Drives the NetSurf suite VM over QMP (tools/run-netsurf-suite.sh).

The guest's /bin/login (driver.c) brings up the desktop with a NetSurf
homepage on the host. From here, with the serial log, the host server's
request log and screendumps as evidence:

  1. click the taskbar's Web slot     -> a "NetSurf" window opens
  2. the homepage loads over HTTPS    -> page, style.css and logo.png fetched
  3. it renders                       -> the CSS colours and the image show
  4. click the purple link            -> page2.html fetched and shown
  5. click the address bar, Ctrl+U, type page3's URL, Enter
                                      -> page3.html fetched and shown

The mouse is a relative USB mouse that the kernel moves 1:1, dropping
reports over 80 px, so the pointer is moved in small steps: first into the
top-left corner, where it clamps to (0,0), then out to the target.

With NSDRIVE_REGS=1, a Web slot that opens nothing also prints each CPU's
RIP/RSP and local APIC state (a CPU halted behind an un-EOI'd vector shows
up in its ISR line).

Prints one "[nsdrive] PASS|FAIL ..." line per check, then
"[nsdrive] SCORE pass=P total=T fail=F"; exits 0 only when nothing failed.
"""
import argparse
import json
import os
import re
import socket
import sys
import time

# Must match tools/netsurf-suite/pages/style.css and host_server.py.
BAND = (0x2a, 0x9d, 0x8f)
LINK = (0x83, 0x38, 0xec)
LOGO = (0xe7, 0x6f, 0x51)
PAGE2 = (0xff, 0xbe, 0x0b)
PAGE3 = (0x3a, 0x86, 0xff)

# graphics-compositor/src/common.hpp
TASKBAR_CENTER_WIDTH = 420
TASKBAR_SLOTS = 5
TASKBAR_HEIGHT = 66
TASKBAR_BOTTOM_MARGIN = 25
WEB_SLOT = 4
BORDER = 1
TITLE_BAR = 34


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.file = self.sock.makefile("rw")
        self.read()                       # greeting
        self.cmd("qmp_capabilities")

    def read(self):
        while True:
            msg = json.loads(self.file.readline())
            if "event" not in msg:
                return msg

    def cmd(self, name, **args):
        self.file.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.file.flush()
        reply = self.read()
        if "error" in reply:
            raise RuntimeError("%s: %s" % (name, reply["error"]))
        return reply.get("return")

    def screendump(self, path):
        self.cmd("screendump", filename=path)
        return Image(path)

    def move(self, dx, dy):
        self.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}}])

    def button(self, down):
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])

    def keys(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": q} for q in qcodes])

    def key(self, qcode, down):
        self.cmd("input-send-event", events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}])


class Image:
    """A P6 PPM from QEMU's screendump."""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        parts = data.split(maxsplit=4)
        assert parts[0] == b"P6", parts[0]
        self.width, self.height = int(parts[1]), int(parts[2])
        self.pixels = parts[4][:self.width * self.height * 3]

    def find(self, rgb, box=None):
        """(count, bounding box) of the pixels of exactly this colour."""
        x0, y0, x1, y1 = box or (0, 0, self.width, self.height)
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(self.width, x1), min(self.height, y1)
        needle = bytes(rgb)
        count, bx0, by0, bx1, by1 = 0, x1, y1, -1, -1
        for y in range(y0, y1):
            row = self.pixels[(y * self.width + x0) * 3:(y * self.width + x1) * 3]
            i = row.find(needle)
            while i != -1:
                if i % 3 == 0:
                    x = x0 + i // 3
                    count += 1
                    bx0, bx1 = min(bx0, x), max(bx1, x)
                    by0, by1 = min(by0, y), max(by1, y)
                i = row.find(needle, i + 1)
        return count, (bx0, by0, bx1 + 1, by1 + 1)


class Drive:
    def __init__(self, args):
        self.args = args
        self.qmp = None
        self.start = time.time()
        self.passed = self.total = 0
        self.shot_no = 0
        self.pointer = None

    def check(self, ok, what, detail=""):
        detail = ("%s, " % detail if detail else "") + "t=%.0fs" % (time.time() - self.start)
        self.total += 1
        self.passed += bool(ok)
        print("[nsdrive] %s %s%s" % ("PASS" if ok else "FAIL", what, (" (%s)" % detail) if detail else ""), flush=True)
        return ok

    def serial(self):
        try:
            with open(self.args.serial, "rb") as f:
                return f.read().decode("latin-1")
        except OSError:
            return ""

    def requests(self):
        try:
            with open(self.args.request_log) as f:
                return [line.split() for line in f if line.strip()]
        except OSError:
            return []

    def wait(self, cond, timeout, step=1.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            value = cond()
            if value:
                return value
            time.sleep(step)
        return cond()

    def fetched(self, path):
        return any(r[:2] == ["https", path] for r in self.requests())

    def shot(self, tag):
        self.shot_no += 1
        path = os.path.join(self.args.out, "shot%d-%s.ppm" % (self.shot_no, tag))
        return self.qmp.screendump(path)

    # ---- pointer ----------------------------------------------------------
    def steps(self, dx, dy):
        # <= 40 px per report, 60 ms apart: QEMU folds unsent motion into the
        # next report, and two of these are still under the kernel's 80 px cap.
        while dx or dy:
            sx = max(-40, min(40, dx))
            sy = max(-40, min(40, dy))
            self.qmp.move(sx, sy)
            dx -= sx
            dy -= sy
            time.sleep(0.06)

    def point(self, x, y):
        if self.pointer is None:
            self.steps(-4000, -4000)  # clamps at (0,0)
            time.sleep(0.3)
            self.pointer = (0, 0)
        self.steps(x - self.pointer[0], y - self.pointer[1])
        self.pointer = (x, y)
        time.sleep(0.3)

    def click(self, x, y):
        self.point(x, y)
        self.qmp.button(True)
        time.sleep(0.15)
        self.qmp.button(False)
        time.sleep(0.3)

    # ---- keyboard ---------------------------------------------------------
    QCODES = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), "-": ("minus",)}

    def type_text(self, text):
        for ch in text:
            if ch.isalnum():
                keys = (ch.lower(),) if not ch.isupper() else ("shift", ch.lower())
            else:
                keys = self.QCODES[ch]
            self.qmp.keys(*keys)
            time.sleep(0.12)

    # ---- the run ----------------------------------------------------------
    def netsurf_window(self):
        m = re.search(r"\[graphics\.compositor\] window id=\d+ x=(\d+) y=(\d+) w=(\d+) h=(\d+) title=NetSurf",
                      self.serial())
        return tuple(int(v) for v in m.groups()) if m else None

    def use_usb_mouse(self):
        """Input goes to QEMU's current mouse, which may be the chipset's PS/2
        one; point it at the USB mouse the guest drives."""
        mice = self.qmp.cmd("query-mice")
        print("[nsdrive] mice: %s" % ", ".join("%d=%s%s" % (m["index"], m["name"], "*" if m["current"] else "")
                                               for m in mice), flush=True)
        for m in mice:
            if "USB" in m["name"] and not m["current"]:
                self.qmp.cmd("human-monitor-command", **{"command-line": "mouse_set %d" % m["index"]})

    def run(self):
        self.qmp = Qmp(self.args.qmp)
        ready = self.wait(lambda: "[graphics.compositor] ready" in self.serial() and
                          "[nstest] network up" in self.serial(), self.args.boot_timeout)
        if not self.check(ready, "desktop up with an address"):
            return
        time.sleep(3)   # let the first frames settle
        self.use_usb_mouse()
        before = self.shot("desktop")
        W, H = before.width, before.height

        # 1. the Web slot, centre of the taskbar's fifth fifth
        slot = TASKBAR_CENTER_WIDTH // TASKBAR_SLOTS
        x = (W - TASKBAR_CENTER_WIDTH) // 2 + WEB_SLOT * slot + slot // 2
        y = H - TASKBAR_BOTTOM_MARGIN - TASKBAR_HEIGHT // 2
        self.click(x, y)
        self.shot("web-click")   # where the pointer ended up, for a failed run
        win = self.wait(self.netsurf_window, 90)
        if not self.check(win, "the taskbar's Web slot opens a NetSurf window", "window %s" % (win,)):
            if os.environ.get("NSDRIVE_REGS"):
                for _ in range(3):
                    regs = self.qmp.cmd("human-monitor-command", **{"command-line": "info registers -a"})
                    print("\n".join(l for l in regs.splitlines() if l.startswith(("CPU#", "RAX", "RIP", "RSP"))))
                    print(self.qmp.cmd("human-monitor-command", **{"command-line": "info lapic"}))
                    time.sleep(1)
            return
        wx, wy, ww, wh = win
        content = (wx + BORDER, wy + TITLE_BAR, wx + BORDER + ww, wy + TITLE_BAR + wh)

        # 2. the homepage and what it pulls in, over HTTPS
        for path in ("/ns/page.html", "/ns/style.css", "/ns/logo.png"):
            got = self.wait(lambda: self.fetched(path), 180 if path.endswith("html") else 60)
            self.check(got, "NetSurf fetches %s over https" % path)

        # 3. rendered: the band (CSS), the image, the link block
        time.sleep(4)
        page = None
        for _ in range(20):
            page = self.shot("page")
            if page.find(BAND, content)[0] >= 15000 and page.find(LOGO, content)[0] >= 14000:
                break
            time.sleep(2)
        n, _ = page.find(BAND, content)
        self.check(n >= 15000, "the stylesheet's colours are on screen", "%d px of #2a9d8f" % n)
        n, _ = page.find(LOGO, content)
        self.check(n >= 14000, "the PNG is decoded and drawn", "%d px of #e76f51" % n)
        n, box = page.find(LINK, content)
        if not self.check(n >= 15000, "the link block is drawn", "%d px of #8338ec" % n):
            return

        # 4. click the link
        self.click((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
        got = self.wait(lambda: self.fetched("/ns/page2.html"), 60)
        self.check(got, "clicking the link fetches page2.html")
        shown = self.wait(lambda: self.shot("page2").find(PAGE2, content)[0] > ww * wh // 3, 40, 2)
        self.check(shown, "page two is shown")

        # 5. the address bar: middle of the toolbar at the top of the window
        self.click(content[0] + ww // 2, content[1] + 15)
        self.qmp.keys("ctrl", "u")
        time.sleep(0.3)
        self.type_text("https://10.0.2.2:%d/ns/page3.html" % self.args.https_port)
        self.qmp.keys("ret")
        got = self.wait(lambda: self.fetched("/ns/page3.html"), 60)
        self.check(got, "typing a URL into the address bar fetches page3.html")
        shown = self.wait(lambda: self.shot("page3").find(PAGE3, content)[0] > ww * wh // 3, 40, 2)
        self.check(shown, "page three is shown")

        # 6. Shift held over several keys, as a person types: NetSurf gets the
        # real Shift and releases, and "1" under it must still give "!" (its
        # toolkit's own shifted table would not). After Shift and Ctrl are let
        # go, the next letter is a plain small one.
        self.click(content[0] + ww // 2, content[1] + 15)
        self.qmp.key("ctrl", True); time.sleep(0.1)
        self.qmp.keys("u"); time.sleep(0.1)
        self.qmp.key("ctrl", False); time.sleep(0.3)
        self.type_text("https://10.0.2.2:%d/ns/k" % self.args.https_port)
        self.qmp.key("shift", True); time.sleep(0.1)
        for k in ("a", "b", "1"):
            self.qmp.key(k, True); time.sleep(0.05)
            self.qmp.key(k, False); time.sleep(0.12)
        self.qmp.key("shift", False); time.sleep(0.1)
        self.type_text("c")
        self.qmp.keys("ret")
        got = self.wait(lambda: self.fetched("/ns/kAB!c"), 60)
        self.check(got, "Shift held over keys types AB! and lets go", "requests: %s" %
                   [r[1] for r in self.requests()][-3:])

        # 7. Editing the address: Home puts the missing "h" in front, End and
        # Left twice go back before the stray X, Delete takes it out.
        self.click(content[0] + ww // 2, content[1] + 15)
        self.qmp.keys("ctrl", "u"); time.sleep(0.3)
        self.type_text("ttps://10.0.2.2:%d/ns/edXt" % self.args.https_port)
        for k in ("home", "h", "end", "left", "left", "delete", "i", "ret"):
            self.qmp.keys(k); time.sleep(0.2)
        got = self.wait(lambda: self.fetched("/ns/edit"), 60)
        self.check(got, "Home, End, Left and Delete edit the address", "requests: %s" %
                   [r[1] for r in self.requests()][-3:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--request-log", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--https-port", type=int, required=True)
    ap.add_argument("--boot-timeout", type=int, default=300)
    args = ap.parse_args()
    drive = Drive(args)
    try:
        drive.run()
    except Exception as e:   # a dead VM or QMP error is a failure, not a crash
        drive.check(False, "driver error", repr(e))
    fail = drive.total - drive.passed
    print("[nsdrive] SCORE pass=%d total=%d fail=%d" % (drive.passed, drive.total, fail), flush=True)
    return 0 if fail == 0 and drive.total > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
