"""Drives the game from boot to the Chao Farm node (33) and screenshots it."""
import sys, time
sys.path.insert(0, "tools")
from verify_farm import attach

SEL = 0x1D1BF08
g = attach(); g.focus()


def tap(seq, pause=0.35):
    for k in seq:
        g.key(k); time.sleep(pause)


# intro -> file select
tap(["enter", "enter", "enter"], 1.2)
time.sleep(1)
tap(["enter"], 1.2)          # pick file 01
tap(["left", "enter"], 0.6)  # OK on warning
time.sleep(2)
tap(["enter"], 1.0)          # 1P PLAY
tap(["right"], 0.5)          # circle STORY -> STAGE SELECT
g.shot("f0_circle")
tap(["enter"], 0.5)
time.sleep(3)
print("entered stage select:", g.state())
g.shot("f1_map")
