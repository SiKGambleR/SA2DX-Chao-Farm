import time, struct, collections, sys
from sa2probe import Game
from sa2trace import trace


def to_circles(g):
    g.focus()
    while g.state()["sub"] != 3:
        time.sleep(0.5)
    time.sleep(3)
    for i in range(3):
        g.focus(); g.key("enter", 0.3); time.sleep(1.5)
        if g.state()["menu"] == 9:
            break
    time.sleep(3)
    g.key("enter", 0.3); time.sleep(3)
    g.key("left", 0.3); time.sleep(1)
    g.key("enter", 0.3)
    while not (g.state()["menu"] == 1 and g.state()["sub"] == 2):
        time.sleep(0.5)
    time.sleep(2)
    g.key("enter", 0.3)
    while g.state()["sub"] != 4:
        time.sleep(0.5)
    time.sleep(2)


if __name__ == "__main__":
    seen = collections.OrderedDict()
    addrs = [int(a, 16) for a in sys.argv[1:]] or [0x66EF40, 0x77D0B0]

    def on_hit(g, r):
        ret = struct.unpack("<I", g.read(r["esp"], 4))[0]
        sp = r["ebx"] if r["eip"] == 0x66EF40 else struct.unpack("<I", g.read(r["esp"] + 4, 4))[0]
        d = g.read(sp, 48)
        x, y, z, sx, sy = struct.unpack_from("<5f", d, 0)
        tl, ta = struct.unpack_from("<II", d, 28)
        key = (hex(r["eip"]), hex(ret), round(x), round(y), hex(tl), r["eax"])
        seen[key] = seen.get(key, 0) + 1

    g = Game()
    try:
        to_circles(g)
        trace(g, addrs, 3, on_hit)
        for k, v in seen.items():
            print(k, v)
    finally:
        g.close()
