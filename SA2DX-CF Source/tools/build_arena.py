"""Builds the Chao World arena land table: a square grass floor ringed by building walls.

Boxes only; convert_garden.py does the conversion. Texture ids index the stock
land texture archive of the base stage: City Escape (landtx13) by default,
--pumpkin for Pumpkin Hill (landtx05, Knuckles/Rouge), --prison for Prison
Lane (landtx09, Tails) or --weaponsbed for Weapons Bed (landtx12, Eggman).

usage: python build_arena.py <out.sa2blvl> [--pumpkin|--prison|--weaponsbed]
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
LAYER = 25.0
# City Escape land texture ids (landtx13 order): 19 grass, 20 red brick,
# 41/46 plaster, 14 cobblestone; windows differ per side.
CITY = dict(GRASS=19, BRICK=20, PLASTER=46, CAP=14, WINDOWS=(7, 25, 45, 36))
# Pumpkin Hill (landtx05 order): 1 dirt, 14 rock, 6 brick, 13 stone block,
# 24 tread plate; the middle band uses the carved panels.
PUMPKIN = dict(GRASS=1, BRICK=14, PLASTER=13, CAP=24, WINDOWS=(20, 9, 4, 12))
# Prison Lane (landtx09 order): 16 concrete floor, 25 steel wall panel,
# 5 plain steel, 35 tread plate; hazard stripes, vents and control panels.
PRISON = dict(GRASS=16, BRICK=25, PLASTER=5, CAP=35, WINDOWS=(18, 10, 23, 21))
# Weapons Bed (landtx12 order): 6 asphalt floor, 21 steel wall panel,
# 3 plain steel, 38 tread plate; hazard stripes, vents and control panels.
WEAPONSBED = dict(GRASS=6, BRICK=21, PLASTER=3, CAP=38, WINDOWS=(20, 11, 22, 9))
BASES = {"--pumpkin": PUMPKIN, "--prison": PRISON, "--weaponsbed": WEAPONSBED}
GRASS, BRICK, PLASTER, CAP = CITY["GRASS"], CITY["BRICK"], CITY["PLASTER"], CITY["CAP"]
WINDOWS = CITY["WINDOWS"]


def box(*v):
    return ",".join("%g" % x for x in v)


def specs():
    out = HALF + WALL
    floor = [box(-out, out, -8, 0, -out, out)]
    # (x0, x1, z0, z1) footprint of each side wall.
    sides = [(-out, out, HALF, out), (-out, out, -out, -HALF),
             (-out, -HALF, -HALF, HALF), (HALF, out, -HALF, HALF)]
    # Stock SA2 ground COLs carry 0x40000001 and walls 0x20000001; without the
    # high bit the game's raycasts (mech walk steps, item ground snaps) miss.
    boxes = ["%d,0.0625,0x40000001:%s" % (GRASS, ";".join(floor))]
    brick, plaster, cap = [], [], []
    for side, (x0, x1, z0, z1) in enumerate(sides):
        brick.append(box(x0, x1, 0, LAYER, z0, z1))
        boxes.append("%d,0.04,0x20000001:%s" % (
            WINDOWS[side], box(x0, x1, LAYER, 2 * LAYER, z0, z1)))
        plaster.append(box(x0, x1, 2 * LAYER, 3 * LAYER, z0, z1))
        cap.append(box(x0 - 2, x1 + 2, 3 * LAYER, 3 * LAYER + 6, z0 - 2, z1 + 2))
    boxes.append("%d,0.04,0x20000001:%s" % (BRICK, ";".join(brick)))
    boxes.append("%d,0.04,0x20000001:%s" % (PLASTER, ";".join(plaster)))
    boxes.append("%d,0.04,0x20000001:%s" % (CAP, ";".join(cap)))
    barrier = [
        box(-out, out, 0, 400, HALF - 1, out),
        box(-out, out, 0, 400, -out, -HALF + 1),
        box(-out, -HALF + 1, 0, 400, -HALF, HALF),
        box(HALF - 1, out, 0, 400, -HALF, HALF),
    ]
    return "|".join(boxes), ";".join(barrier)

def main(target):
    boxes, slabs = specs()
    env = dict(os.environ, GARDEN_COL_RANGE="0,0", GARDEN_GATE="none",
               GARDEN_BOXES=boxes, GARDEN_SLABS=slabs, GARDEN_WATER_IDS="9999",
               GARDEN_SLAB_FLAGS="0x20000001")
    subprocess.run([sys.executable, CONVERT, SAMODEL, SOURCE, target],
                   env=env, check=True)


if __name__ == "__main__":
    if len(sys.argv) > 3 or (len(sys.argv) == 3 and sys.argv[2] not in BASES):
        sys.exit("unknown option: " + " ".join(sys.argv[2:]))
    if len(sys.argv) == 3:
        base = BASES[sys.argv[2]]
        GRASS, BRICK, PLASTER, CAP = base["GRASS"], base["BRICK"], base["PLASTER"], base["CAP"]
        WINDOWS = base["WINDOWS"]
    main(sys.argv[1])
