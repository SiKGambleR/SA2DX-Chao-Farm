import ctypes, struct, time, collections
from ctypes import wintypes
from sa2probe import k32
from sa2trace import EXC, DBGEV, DBG_CONTINUE, DBG_NOT_HANDLED

PAGE_GUARD = 0x100


def watch(g, lo, hi, seconds, ignore=lambda eip: False):
    """Record EIPs that touch [lo, hi) using a guard page."""
    page = lo & ~0xFFF
    pages = list(range(page, ((hi + 0xFFF) & ~0xFFF), 0x1000))
    hits = collections.Counter()
    assert k32.DebugActiveProcess(g.p.pid)

    def arm():
        for p in pages:
            old = wintypes.DWORD()
            k32.VirtualProtectEx(g.h, ctypes.c_void_p(p), 0x1000, 0x04 | PAGE_GUARD, ctypes.byref(old))

    arm()
    ev = DBGEV()
    end = time.time() + seconds
    stepping = set()
    try:
        while time.time() < end:
            if not k32.WaitForDebugEvent(ctypes.byref(ev), 200):
                continue
            status = DBG_CONTINUE
            if ev.code == 1:
                exc = EXC.from_buffer(ev.u)
                th = k32.OpenThread(0x1FFFFF, False, ev.tid)
                ctx = (ctypes.c_char * 716)()
                struct.pack_into("<I", ctx, 0, 0x10007)
                if exc.code == 0x80000001:
                    k32.Wow64GetThreadContext(th, ctx)
                    eip = struct.unpack_from("<I", bytes(ctx), 184)[0]
                    addr = exc.info[1] or 0
                    if lo <= addr < hi and not ignore(eip):
                        hits[(hex(eip), hex(addr), exc.info[0])] += 1
                    efl = struct.unpack_from("<I", bytes(ctx), 192)[0] | 0x100
                    struct.pack_into("<I", ctx, 192, efl)
                    k32.Wow64SetThreadContext(th, ctx)
                    stepping.add(ev.tid)
                elif exc.code == 0x80000004 and ev.tid in stepping:
                    stepping.discard(ev.tid)
                    arm()
                elif exc.code == 0x80000003:
                    pass
                else:
                    status = DBG_NOT_HANDLED
                k32.CloseHandle(th)
            k32.ContinueDebugEvent(ev.pid, ev.tid, status)
    finally:
        for p in pages:
            old = wintypes.DWORD()
            k32.VirtualProtectEx(g.h, ctypes.c_void_p(p), 0x1000, 0x04, ctypes.byref(old))
        k32.DebugActiveProcessStop(g.p.pid)
    return hits
