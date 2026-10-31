"""Builds the Chao World arena land table: a square grass floor ringed by rock walls.

Boxes only; convert_garden.py does the conversion. Texture ids index the stock
Green Forest land texture archive (landtx03): 10 = mossy grass, 9 = dark rock.

usage: python build_arena.py <out.sa2blvl>
"""
import os
import subprocess
import sys

SAMODEL = r"C:\Users\leg4c\sa-tools-work\SATools\bin\lib\SAModel.dll"
SOURCE = (r"C:\Users\leg4c\sa-tools-work\splitout\out\chao\stg_garden00_ss"
          r"\landtablegarden00.c.sa1lvl")
CONVERT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "convert_garden.py")

HALF = 300.0
WALL = 10.0
WALL_H = 60.0
GRASS, ROCK = 10, 9


def box(*v):
    return ",".join("%g" % x for x in v)


def specs():
    out = HALF + WALL
    floor = [box(-out, out, -8, 0, -out, out)]
    walls = [
        box(-out, out, 0, WALL_H, HALF, out),
        box(-out, out, 0, WALL_H, -out, -HALF),
        box(-out, -HALF, 0, WALL_H, -HALF, HALF),
        box(HALF, out, 0, WALL_H, -HALF, HALF),
    ]
    # Rock caps and an invisible barrier so nothing can climb or jump out.
    caps = [
        box(-out - 2, out + 2, WALL_H, WALL_H + 6, HALF - 2, out + 2),
        box(-out - 2, out + 2, WALL_H, WALL_H + 6, -out - 2, -HALF + 2),
        box(-out - 2, -HALF + 2, WALL_H, WALL_H + 6, -HALF, HALF),
        box(HALF - 2, out + 2, WALL_H, WALL_H + 6, -HALF, HALF),
    ]
    barrier = [
        box(-out, out, 0, 400, HALF - 1, out),
        box(-out, out, 0, 400, -out, -HALF + 1),
        box(-out, -HALF + 1, 0, 400, -HALF, HALF),
        box(HALF - 1, out, 0, 400, -HALF, HALF),
    ]
    boxes = ["%d,0.0625:%s" % (GRASS, ";".join(floor)),
             "%d,0.04:%s" % (ROCK, ";".join(walls + caps))]
    return "|".join(boxes), ";".join(barrier)


def main(target):
    boxes, slabs = specs()
    env = dict(os.environ, GARDEN_COL_RANGE="0,0", GARDEN_GATE="none",
               GARDEN_BOXES=boxes, GARDEN_SLABS=slabs, GARDEN_WATER_IDS="9999")
    subprocess.run([sys.executable, CONVERT, SAMODEL, SOURCE, target],
                   env=env, check=True)


if __name__ == "__main__":
    main(sys.argv[1])
