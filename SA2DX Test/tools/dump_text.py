import ctypes, subprocess, sys, time, os
from ctypes import wintypes

GAME = r"G:\SteamLibrary\steamapps\common\Sonic Adventure 2"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sa2_text_dump.bin")
BASE, SIZE = 0x401000, 0x46F591

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

p = subprocess.Popen([os.path.join(GAME, "sonic2app.exe")], cwd=GAME)
time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 12)
h = k32.OpenProcess(0x0410, False, p.pid)
buf = ctypes.create_string_buffer(SIZE)
got = ctypes.c_size_t(0)
ok = k32.ReadProcessMemory(h, ctypes.c_void_p(BASE), buf, SIZE, ctypes.byref(got))
print("pid", p.pid, "ok", ok, "read", got.value, "err", ctypes.get_last_error())
open(OUT, "wb").write(buf.raw[:got.value])
p.terminate()
