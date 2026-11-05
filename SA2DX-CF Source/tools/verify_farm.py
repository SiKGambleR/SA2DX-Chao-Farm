"""Step driver for in-game verification of the Chao Farm polish work.

Launches the game once (pid kept in build\verify\pid.txt) and then lets later
invocations attach and run one action each, so an agent can inspect
screenshots between steps.

usage:
  python verify_farm.py launch
  python verify_farm.py state [shotname]
  python verify_farm.py key <enter|up|down|left|right|esc|x|z> [times] [shotname]
  python verify_farm.py shot <name>
  python verify_farm.py burst <name> <seconds>   (screenshot every 0.4 s)
  python verify_farm.py song
  python verify_farm.py kill
"""
import ctypes
import os
import struct
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sa2probe import GAME, SC, k32, u32

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "verify")
PIDFILE = os.path.join(OUT, "pid.txt")

GameModeAddr = 0x174B040
CurrentLevelAddr = 0x1934B70
GameStateAddr = 0x1934BE0
NextLevelAddr = 0x1934BEC
CurrentCharacterAddr = 0x1934B80
SongAddr = 0x1936268
SelectionAddr = 0x1D1BF08
MenuAddr = 0x1D7BB10


class Attached:
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(0x1F0FFF, False, pid)

    def read(self, va, n=4):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        k32.ReadProcessMemory(self.h, ctypes.c_void_p(va), buf, n, ctypes.byref(got))
        return buf.raw

    def u32v(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def i16(self, va):
        return struct.unpack("<h", self.read(va, 2))[0]

    def i8(self, va):
        return struct.unpack("<b", self.read(va, 1))[0]

    def hwnd(self):
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(h, _):
            pid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == self.pid and u32.IsWindowVisible(h):
                found.append(h)
            return True

        u32.EnumWindows(cb, 0)
        return found[0] if found else None

    def focus(self):
        h = self.hwnd()
        if h:
            u32.SetForegroundWindow(h)
            time.sleep(0.2)

    def key(self, name, hold=0.12):
        sc = SC[name]
        u32.keybd_event(0, sc, 0x0008, 0)
        time.sleep(hold)
        u32.keybd_event(0, sc, 0x0008 | 0x0002, 0)
        time.sleep(0.3)

    def shot(self, name):
        from PIL import ImageGrab
        h = self.hwnd()
        if not h:
            print("no window")
            return
        r = wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        path = os.path.join(OUT, name + ".png")
        ImageGrab.grab((r.left, r.top, r.right, r.bottom), all_screens=True).save(path)
        print("shot", path)

    def state(self):
        song = self.read(SongAddr, 32).split(b"\0")[0].decode("latin1", "replace")
        return dict(mode=self.u32v(GameModeAddr), menu=self.u32v(MenuAddr),
                    level=self.i16(CurrentLevelAddr), next=self.i16(NextLevelAddr),
                    gstate=self.i16(GameStateAddr), chr=self.u32v(CurrentCharacterAddr),
                    sel=self.i8(SelectionAddr), song=song)


def attach():
    pid = int(open(PIDFILE).read())
    return Attached(pid)


def main():
    os.makedirs(OUT, exist_ok=True)
    cmd = sys.argv[1]
    if cmd == "launch":
        p = subprocess.Popen([os.path.join(GAME, "sonic2app.exe")], cwd=GAME)
        open(PIDFILE, "w").write(str(p.pid))
        print("pid", p.pid)
        time.sleep(8)
        g = Attached(p.pid)
        g.focus()
        g.shot("00_boot")
        print(g.state())
        return
    g = attach()
    if cmd == "state":
        print(g.state())
        if len(sys.argv) > 2:
            g.shot(sys.argv[2])
    elif cmd == "key":
        times = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        g.focus()
        for _ in range(times):
            g.key(sys.argv[2])
            print(g.state())
        if len(sys.argv) > 4:
            time.sleep(0.6)
            g.shot(sys.argv[4])
    elif cmd == "shot":
        g.shot(sys.argv[2])
        print(g.state())
    elif cmd == "burst":
        seconds = float(sys.argv[3])
        end = time.time() + seconds
        i = 0
        while time.time() < end:
            g.shot("%s_%02d" % (sys.argv[2], i))
            print(g.state())
            i += 1
            time.sleep(0.4)
    elif cmd == "song":
        print(g.state()["song"])
    elif cmd == "kill":
        subprocess.run(["taskkill", "/PID", str(g.pid), "/F"], capture_output=True)
        print("killed", g.pid)


if __name__ == "__main__":
    main()
