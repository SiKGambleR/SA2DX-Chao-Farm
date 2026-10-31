import ctypes, os, subprocess, sys, time, struct
from ctypes import wintypes

GAME = r"G:\SteamLibrary\steamapps\common\Sonic Adventure 2"
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

SC = {"enter": 0x1C, "up": 0x11, "down": 0x1F, "left": 0x1E, "right": 0x20, "esc": 0x01, "x": 0x2D, "z": 0x2C}


class Game:
    def __init__(self):
        self.p = subprocess.Popen([os.path.join(GAME, "sonic2app.exe")], cwd=GAME)
        self.h = None
        time.sleep(6)
        self.h = k32.OpenProcess(0x1F0FFF, False, self.p.pid)

    def read(self, va, n=4):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        k32.ReadProcessMemory(self.h, ctypes.c_void_p(va), buf, n, ctypes.byref(got))
        return buf.raw

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def write32(self, va, val):
        b = struct.pack("<I", val)
        n = ctypes.c_size_t(0)
        k32.WriteProcessMemory(self.h, ctypes.c_void_p(va), b, 4, ctypes.byref(n))

    def ptr_u8(self, va, off):
        return self.read(self.u32(va) + off, 1)[0]

    def hwnd(self):
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(h, _):
            pid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == self.p.pid and u32.IsWindowVisible(h):
                found.append(h)
            return True

        u32.EnumWindows(cb, 0)
        return found[0] if found else None

    def key(self, name, hold=0.12):
        sc = SC[name]
        ext = 0x0001 if sc & 0x80 else 0
        sc &= 0x7F
        u32.keybd_event(0, sc, 0x0008 | ext, 0)
        time.sleep(hold)
        u32.keybd_event(0, sc, 0x0008 | 0x0002 | ext, 0)
        time.sleep(0.25)

    def focus(self):
        h = self.hwnd()
        if h:
            u32.SetForegroundWindow(h)

    def state(self):
        return dict(menu=self.u32(0x1D7BB10), sub=self.u32(0x1D7BB14), sel=self.ptr_u8(0xC77F14, 0x40) if self.u32(0xC77F14) else None)

    def shot(self, path):
        from PIL import ImageGrab
        h = self.hwnd()
        r = wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        ImageGrab.grab((r.left, r.top, r.right, r.bottom)).save(path)

    def close(self):
        self.p.terminate()


if __name__ == "__main__":
    g = Game()
    try:
        g.focus()
        for i in range(30):
            print(i, g.state(), g.hwnd())
            time.sleep(1)
    finally:
        g.close()




