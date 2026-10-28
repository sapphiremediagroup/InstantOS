#!/usr/bin/env python3
"""Keyboard suite driver (tools/run-kbd-suite.sh). For each keyboard (ps2: the
i8042 alone; usb: usb-kbd on xHCI; ohci: usb-kbd on OHCI) it boots QEMU twice
and presses keys over QMP like a user:

  console  the probe image (/bin/login is console_probe.c, which prints each
           console read in hex): the bytes of plain keys, controls, Alt, the
           cursor/editing/F keys with and without modifiers, and a held key's
           repeats, which must arrive as whole sequences.
  terminal the system image: Super+T opens the terminal and bash's command
           lines are edited with the keys. Each line is a curl to the host
           server, whose request log shows the line bash ran. Also auto-repeat,
           the keypad with Num Lock on and off, Caps Lock, and (when this QEMU
           has the trace events, as tools/qemu-rtl8168/build-qemu.sh's does)
           the lock lights the keyboard was told to show.
"""
import argparse, os, re, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "netsurf-suite"))
from drive import Qmp   # noqa: E402

PASSED = 0
TOTAL = 0


def check(cond, name, detail=""):
    global PASSED, TOTAL
    TOTAL += 1
    PASSED += 1 if cond else 0
    print("[kbdsuite] %s %s%s" % ("PASS" if cond else "FAIL", name, (" (%s)" % detail) if detail else ""), flush=True)
    return cond


class Vm:
    def __init__(self, args, mode, iso, tag, trace=None):
        self.out = os.path.join(args.out, "%s-%s" % (tag, mode))
        os.makedirs(self.out, exist_ok=True)
        self.serial = os.path.join(self.out, "serial.log")
        self.sockdir = tempfile.mkdtemp()   # QMP's socket cannot live on drvfs
        sock = os.path.join(self.sockdir, "qmp.sock")
        subprocess.run(["cp", args.ovmf_vars, os.path.join(self.out, "vars.fd")], check=True)
        if mode == "ohci":
            usb = ["-device", "pci-ohci,id=ohci", "-device", "usb-kbd,bus=ohci.0"]
        else:
            usb = ["-device", "qemu-xhci,id=usb", "-device", "usb-mouse,bus=usb.0"]
            if mode == "usb":
                usb += ["-device", "usb-kbd,bus=usb.0"]
        tr = []
        if trace:
            tr = ["-trace", "ps2_set_ledstate,file=" + trace, "-trace", "hid_kbd_set_leds,file=" + trace]
        self.proc = subprocess.Popen([args.qemu] + tr + [
            "-drive", "if=pflash,format=raw,readonly=on,file=" + args.ovmf_code,
            "-drive", "if=pflash,format=raw,file=" + os.path.join(self.out, "vars.fd"),
            "-drive", "file=%s,media=cdrom" % iso] + usb + [
            "-device", "virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off",
            "-netdev", "user,id=net0",
            "-accel", "tcg", "-cpu", "max,+tsc", "-m", "2G", "-smp", str(args.smp),
            "-display", "none", "-monitor", "none", "-qmp", "unix:%s,server,nowait" % sock,
            "-serial", "file:" + self.serial, "-no-reboot"])
        self.qmp = Qmp(sock)

    def log(self):
        try:
            with open(self.serial, "rb") as f:
                return f.read().decode("latin-1")
        except FileNotFoundError:
            return ""

    def wait(self, cond, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if cond():
                return True
            time.sleep(1)
        return False

    def hold(self, qcode, seconds):
        self.key(qcode, True)
        time.sleep(seconds)
        self.key(qcode, False)
        time.sleep(0.3)

    def key(self, qcode, down):
        self.qmp.cmd("input-send-event", events=[
            {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}])

    def close(self):
        crashed = re.search(r"Unknown Instruction|User process crash|KERNEL PANIC", self.log())
        self.proc.kill()
        self.proc.wait()
        subprocess.run(["rm", "-rf", self.sockdir])
        return not crashed


# ---- console ---------------------------------------------------------------

CONSOLE_CASES = [
    (("a",), "61"),
    (("shift", "a"), "41"),
    (("ret",), "0a"),
    (("kp_enter",), "0a"),
    (("backspace",), "08"),
    (("tab",), "09"),
    (("esc",), "1b"),
    (("ctrl", "c"), "03"),
    (("ctrl", "backslash"), "1c"),
    (("ctrl", "spc"), "00"),
    (("alt", "x"), "1b78"),
    (("up",), "1b5b41"),
    (("down",), "1b5b42"),
    (("right",), "1b5b43"),
    (("left",), "1b5b44"),
    (("home",), "1b5b48"),
    (("end",), "1b5b46"),
    (("insert",), "1b5b327e"),
    (("delete",), "1b5b337e"),
    (("pgup",), "1b5b357e"),
    (("pgdn",), "1b5b367e"),
    (("f1",), "1b4f50"),
    (("f5",), "1b5b31357e"),
    (("f12",), "1b5b32347e"),
    (("ctrl", "left"), "1b5b313b3544"),
    (("shift", "up"), "1b5b313b3241"),
    (("ctrl", "f1"), "1b5b313b3550"),
    (("shift", "f5"), "1b5b31353b327e"),
    (("ctrl", "alt", "delete"), "1b5b333b377e"),
    (("kp_4",), "34"),   # Num Lock is on at boot
]


def run_console(args, mode):
    vm = Vm(args, mode, args.probe_iso, "console")
    try:
        if not check(vm.wait(lambda: "[kbdprobe] ready" in vm.log(), args.boot_timeout),
                     "%s console: the probe is up" % mode):
            return
        time.sleep(5)

        def reads():
            return re.findall(r"\[kbdprobe\] ([0-9a-f]+)", vm.log())
        for keys, want in CONSOLE_CASES:
            before = len(reads())
            vm.qmp.keys(*keys)
            vm.wait(lambda: "".join(reads()[before:]) == want, 3)
            time.sleep(0.3)
            got = "".join(reads()[before:])
            check(got == want, "%s console: %s" % (mode, "+".join(keys)), got if got == want else "got %s, want %s" % (got, want))

        before = len(reads())
        vm.hold("right", 1.2)
        time.sleep(1)
        got = reads()[before:]
        count = "".join(got).count("1b5b43")
        whole = all(re.fullmatch(r"(1b5b43)+", r) for r in got)
        check(whole and 10 <= count <= 45, "%s console: a held Right repeats whole sequences" % mode,
              "%d sequences in %d reads" % (count, len(got)))
    finally:
        check(vm.close(), "%s console: no crash" % mode)


# ---- terminal --------------------------------------------------------------

QCODES = {"#": ("shift", "3"), " ": ("spc",), ":": ("shift", "semicolon"), "/": ("slash",),
          ".": ("dot",), "-": ("minus",)}


def run_terminal(args, mode, request_log, trace):
    vm = Vm(args, mode, args.system_iso, "terminal", trace)
    url = "curl -s 10.0.2.2:%d" % args.http_port

    def typ(text):
        for ch in text:
            if ch.isalnum():
                k = ("shift", ch.lower()) if ch.isupper() else (ch,)
            else:
                k = QCODES[ch]
            vm.qmp.keys(*k)
            time.sleep(0.15)

    def key(*k):
        vm.qmp.keys(*k)
        time.sleep(0.3)

    def run_line(*steps):
        """Type and edit a command line, press Enter, give curl time."""
        for s in steps:
            if isinstance(s, str):
                typ(s)
            else:
                s()
        time.sleep(5)

    try:
        if not check(vm.wait(lambda: "[graphics.compositor] ready" in vm.log(), args.boot_timeout),
                     "%s terminal: the desktop is up" % mode):
            return
        time.sleep(8)
        vm.qmp.keys("meta_l", "t")
        if not check(vm.wait(lambda: re.search(r"window id=\d+ .*title=Terminal", vm.log()), 60),
                     "%s terminal: Super+T opens the terminal" % mode):
            return
        time.sleep(6)
        start = len(open(request_log).read().splitlines())

        # Capitals that used to be arrows (H K M P), then Left, Backspace, End;
        # Up brings the same line back from history.
        run_line(url + "/k/HKMPX", lambda: key("left"), lambda: key("backspace"), "Q",
                 lambda: key("end"), "Z", lambda: key("ret"))
        run_line(lambda: key("up"), lambda: key("ret"))
        run_line(url + "/c/one", lambda: key("ctrl", "a"), lambda: key("ctrl", "k"), url + "/c/two",
                 lambda: key("ret"))
        run_line(url + "/h/AB", lambda: key("home"), "#", lambda: key("ret"))     # a comment: no request
        run_line(url + "/d/ABC", lambda: key("left"), lambda: key("left"), lambda: key("delete"),
                 lambda: key("ret"))
        run_line(url + "/w/AB", lambda: key("ctrl", "left"), "Q", lambda: key("ctrl", "right"), "Z",
                 lambda: key("ret"))
        run_line(url + "/a/AB", lambda: key("alt", "b"), "Q", lambda: key("ret"))  # Alt as ESC prefix
        run_line(url + "/u/AB XY", lambda: key("ctrl", "w"), lambda: key("backspace"), "Z",
                 lambda: key("ret"))
        run_line(url + "/x/bad", lambda: key("ctrl", "c"), url + "/x/good", lambda: key("ret"))

        def held_shift():
            vm.key("shift", True); time.sleep(0.1)
            for k in ("a", "b"):
                vm.key(k, True); time.sleep(0.05)
                vm.key(k, False); time.sleep(0.12)
            vm.key("shift", False); time.sleep(0.1)
        run_line(url + "/s/", held_shift, "c", lambda: key("ret"))
        run_line(url + "/r/", lambda: vm.hold("x", 1.2), lambda: key("ret"))
        run_line(url + "/n/", lambda: key("kp_4"), lambda: key("kp_2"), lambda: key("kp_enter"))
        run_line(url + "/L/", lambda: key("caps_lock"), "ab", lambda: key("caps_lock"), "c", lambda: key("ret"))
        run_line(url + "/m/13", lambda: key("num_lock"), lambda: key("kp_4"), lambda: key("num_lock"),
                 lambda: key("kp_2"), lambda: key("ret"))
        key("scroll_lock")
        time.sleep(1)

        paths = [l.split()[1] for l in open(request_log).read().splitlines()[start:] if l.strip()]
        print("[kbdsuite] %s terminal: requests %s" % (mode, paths), flush=True)
        name = mode + " terminal: "
        check(paths.count("/k/HKMQXZ") == 2, name + "capitals, Left, Backspace, End; Up repeats the line")
        check("/c/two" in paths and "/c/one" not in paths, name + "Ctrl+A, Ctrl+K")
        check(not any(p.startswith("/h/") for p in paths), name + "Home")
        check("/d/AC" in paths, name + "Delete")
        check("/w/QABZ" in paths, name + "Ctrl+Left/Right move by word")
        check("/a/QAB" in paths, name + "Alt+B moves back a word")
        check("/u/ABZ" in paths, name + "Ctrl+W deletes a word")
        check("/x/good" in paths and not any("bad" in p for p in paths), name + "Ctrl+C drops the line")
        check("/s/ABc" in paths, name + "Shift held over keys")
        reps = [len(p) - 3 for p in paths if p.startswith("/r/") and set(p[3:]) == {"x"}]
        check(len(reps) == 1 and 10 <= reps[0] <= 45, name + "holding x 1.2 s repeats it", str(reps))
        check("/n/42" in paths, name + "keypad digits and Enter")
        check("/L/ABc" in paths, name + "Caps Lock on, off")
        check("/m/123" in paths, name + "Num Lock off: keypad 4 is Left")

        if trace:
            leds = []
            for line in open(trace, errors="replace"):
                m = re.search(r"ps2_set_ledstate \S+ ledstate (\d+)", line)
                if m and mode == "ps2":   # PS/2 orders them Scroll, Num, Caps
                    v = int(m.group(1))
                    leds.append((1 if v & 2 else 0) | (2 if v & 4 else 0) | (4 if v & 1 else 0))
                m = re.search(r"hid_kbd_set_leds leds 0x([0-9a-f]+)", line)
                if m and mode != "ps2":
                    leds.append(int(m.group(1), 16))
            while leds and leds[0] != 1:   # QEMU's and the firmware's resets
                leds.pop(0)
            want = [1, 3, 1, 0, 1, 5]      # boot Num, +Caps, -Caps, -Num, +Num, +Scroll
            check(leds == want, name + "the lock lights", "%s, want %s" % (leds, want))
    finally:
        check(vm.close(), "%s terminal: no crash" % mode)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modes", default="ps2 usb ohci")
    ap.add_argument("--parts", default="console terminal")
    ap.add_argument("--probe-iso", required=True)
    ap.add_argument("--system-iso", required=True)
    ap.add_argument("--request-log", required=True)
    ap.add_argument("--http-port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--trace", action="store_true", help="this QEMU has hid_kbd_set_leds")
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--boot-timeout", type=int, default=300)
    ap.add_argument("--ovmf-code", default="/usr/share/edk2/x64/OVMF_CODE.4m.fd")
    ap.add_argument("--ovmf-vars", default="/usr/share/edk2/x64/OVMF_VARS.4m.fd")
    args = ap.parse_args()
    for mode in args.modes.split():
        if "console" in args.parts.split():
            try:
                run_console(args, mode)
            except Exception as e:   # a dead VM or QMP error is a failure
                check(False, "%s console: driver error" % mode, repr(e))
        if "terminal" in args.parts.split():
            trace = None
            if args.trace:
                trace = os.path.join(args.out, "leds-%s.trace" % mode)
                if os.path.exists(trace):
                    os.remove(trace)
            try:
                run_terminal(args, mode, args.request_log, trace)
            except Exception as e:
                check(False, "%s terminal: driver error" % mode, repr(e))
    print("[kbdsuite] SCORE pass=%d total=%d fail=%d" % (PASSED, TOTAL, TOTAL - PASSED), flush=True)
    return 0 if TOTAL > 0 and PASSED == TOTAL else 1


if __name__ == "__main__":
    sys.exit(main())
