import sys, time
from sa2probe import Game
from trace_circles import to_circles
from sa2watch import watch

g = Game()
try:
    to_circles(g)
    md = g.u32(0xC77F14)
    print("MainMenuData", hex(md))
    lo = md
    hits = watch(g, lo, lo + 0x1000, 4, ignore=lambda e: False)
    for k, v in sorted(hits.items()):
        print(k, v)
finally:
    g.close()


