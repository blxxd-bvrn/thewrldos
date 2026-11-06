#!/usr/bin/env python3
"""Drive THE WRLD OS in headless QEMU via QMP: move/click the mouse, type, take screenshots.
Usage (as a library): h = Harness('wrldos.iso'); h.wait(5); h.click(100,200); h.shot('x.png')"""
import json, os, socket, subprocess, sys, time, tempfile
from PIL import Image

class Harness:
    def __init__(self, iso, mem=256, outdir=".", hda=None, boot="d"):
        self.dir = tempfile.mkdtemp()
        self.sock = os.path.join(self.dir, "qmp.sock")
        self.serial = os.path.join(outdir, "serial.log")
        self.outdir = outdir
        args = ["qemu-system-i386", "-m", str(mem), "-vga", "std", "-display", "none", "-serial", "file:" + self.serial,
                "-qmp", "unix:%s,server,nowait" % self.sock, "-boot", boot]
        if iso: args += ["-cdrom", iso]
        if hda: args += ["-drive", "file=%s,format=raw,if=ide,index=0" % hda]
        self.p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX); self.s.connect(self.sock); break
            except OSError: time.sleep(0.1)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")
        self.mx = self.my = None

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r

    def wait(self, s): time.sleep(s)
    def shot(self, path, scale=1):
        ppm = os.path.join(self.dir, "s.ppm")
        self.cmd("screendump", filename=ppm)
        time.sleep(0.3)
        im = Image.open(ppm)
        if scale != 1: im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
        im.save(path); return im
    def rel(self, dx, dy):
        while dx or dy:
            sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
            self.cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": sx}}, {"type": "rel", "data": {"axis": "y", "value": sy}}])
            dx -= sx; dy -= sy; time.sleep(0.01)
    def move(self, x, y):
        if self.mx is None:
            self.rel(-3000, -3000); time.sleep(0.1); self.mx = self.my = 0
        self.rel(x - self.mx, y - self.my); self.mx, self.my = x, y; time.sleep(0.1)
    def button(self, down, btn="left"):
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": btn}}])
    def click(self, x, y, btn="left", double=False):
        self.move(x, y)
        for _ in range(2 if double else 1):
            self.button(True, btn); time.sleep(0.06); self.button(False, btn); time.sleep(0.08)
        time.sleep(0.3)
    def drag(self, x0, y0, x1, y1):
        self.move(x0, y0); self.button(True); time.sleep(0.1)
        steps = 10
        for i in range(1, steps + 1):
            self.move(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        self.button(False); time.sleep(0.3)
    KEYMAP = {" ": "spc", "\n": "ret", ".": "dot", "/": "slash", "\\": "backslash", "-": "minus", "=": "equal",
              ",": "comma", ";": "semicolon", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right", "`": "grave_accent"}
    SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
               "_": "minus", "+": "equal", ":": "semicolon", '"': "apostrophe", "<": "comma", ">": "dot", "?": "slash", "|": "backslash", "{": "bracket_left", "}": "bracket_right", "~": "grave_accent"}
    def key(self, *names):
        evs = [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": n}}} for n in names]
        self.cmd("input-send-event", events=evs); time.sleep(0.03)
        evs = [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": n}}} for n in reversed(names)]
        self.cmd("input-send-event", events=evs); time.sleep(0.03)
    def type(self, text):
        for ch in text:
            if ch in self.SHIFTED: self.key("shift", self.SHIFTED[ch])
            elif ch.isupper(): self.key("shift", ch.lower())
            elif ch in self.KEYMAP: self.key(self.KEYMAP[ch])
            else: self.key(ch)
    def quit(self):
        try: self.cmd("quit")
        except Exception: pass
        self.p.wait(5)
