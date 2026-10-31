import ctypes, struct, time, sys
from ctypes import wintypes
from sa2probe import Game, k32

k32.OpenThread.restype = wintypes.HANDLE
DBG_CONTINUE = 0x00010002
DBG_NOT_HANDLED = 0x80010001


class EXC(ctypes.Structure):
    _fields_ = [("code", wintypes.DWORD), ("flags", wintypes.DWORD), ("rec", ctypes.c_void_p),
                ("addr", ctypes.c_void_p), ("nparams", wintypes.DWORD), ("info", ctypes.c_void_p * 15)]


class DBGEV(ctypes.Structure):
    _fields_ = [("code", wintypes.DWORD), ("pid", wintypes.DWORD), ("tid", wintypes.DWORD), ("pad", wintypes.DWORD),
                ("u", ctypes.c_byte * 200)]


def protect(g, va, n=16):
    old = wintypes.DWORD()
    k32.VirtualProtectEx(g.h, ctypes.c_void_p(va), n, 0x40, ctypes.byref(old))


def wbytes(g, va, b):
    n = ctypes.c_size_t()
    k32.WriteProcessMemory(g.h, ctypes.c_void_p(va), b, len(b), ctypes.byref(n))


def trace(g, addrs, seconds, on_hit):
    orig = {a: g.read(a, 1) for a in addrs}
    for a in addrs:
        protect(g, a)
    assert k32.DebugActiveProcess(g.p.pid)
    for a in addrs:
        wbytes(g, a, b"\xCC")
    ev = DBGEV()
    end = time.time() + seconds
    stepping = {}
    stats = {"ev": 0, "bp": 0}
    try:
        while time.time() < end:
            if not k32.WaitForDebugEvent(ctypes.byref(ev), 200):
                continue
            status = DBG_CONTINUE
            stats["ev"] += 1; stats[(ev.code, hex(EXC.from_buffer(ev.u).code) if ev.code==1 else 0)] = stats.get((ev.code, hex(EXC.from_buffer(ev.u).code) if ev.code==1 else 0),0)+1
            if ev.code == 1:
                exc = EXC.from_buffer(ev.u)
                th = k32.OpenThread(0x1FFFFF, False, ev.tid)
                ctx = (ctypes.c_char * 716)()
                struct.pack_into("<I", ctx, 0, 0x10007)
                if exc.code in (0x4000001F,0x80000003) and exc.addr in addrs:
                    k32.Wow64GetThreadContext(th, ctx)
                    raw = bytes(ctx)
                    regs = dict(eax=struct.unpack_from("<I", raw, 176)[0], ebx=struct.unpack_from("<I", raw, 164)[0],
                                esp=struct.unpack_from("<I", raw, 196)[0], eip=exc.addr)
                    stats['bp'] += 1
                    stats["bp"] += 1
                    on_hit(g, regs)
                    wbytes(g, exc.addr, orig[exc.addr])
                    struct.pack_into("<I", ctx, 184, exc.addr)
                    efl = struct.unpack_from("<I", raw, 192)[0] | 0x100
                    struct.pack_into("<I", ctx, 192, efl)
                    k32.Wow64SetThreadContext(th, ctx)
                    stepping[ev.tid] = exc.addr
                elif exc.code in (0x4000001E,0x80000004) and ev.tid in stepping:
                    wbytes(g, stepping.pop(ev.tid), b"\xCC")
                elif exc.code in (0x4000001F,0x80000003):
                    pass
                else:
                    status = DBG_NOT_HANDLED
                k32.CloseHandle(th)
            k32.ContinueDebugEvent(ev.pid, ev.tid, status)
    finally:
        for a in addrs:
            wbytes(g, a, orig[a])
        k32.DebugActiveProcessStop(g.p.pid)
        print('trace stats', stats)




