"""Builds the lobby land tables with a west-side corridor.

For each of the five stock lobby tables it removes the wall wedge on the west
side (visual and collision), then merges a corridor built by convert_garden.py
(boxes only). Runs on the user's own extracted lobby tables; SAModel (SA Tools,
GPL-3.0) is used as an external dependency through pythonnet.

usage: python build_lobby_corridor.py <stock_dir> <out_dir>
  stock_dir holds objLandTableLobby{000,00k,0dk,h0k,hdk}.sa2blvl
"""
import math
import os
import subprocess
import sys
import tempfile

from pythonnet import load

load("coreclr")
import clr  # noqa: E402
import System  # noqa: E402

SAMODEL = r"C:\Users\leg4c\sa-tools-work\SATools\bin\lib\SAModel.dll"
SOURCE = (r"C:\Users\leg4c\sa-tools-work\splitout\out\chao\stg_garden00_ss"
          r"\landtablegarden00.c.sa1lvl")
CONVERT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "convert_garden.py")
clr.AddReference(SAMODEL)
from SAModel import LandTable, LandTableFormat, NJS_MESHSET, Triangle, Poly  # noqa: E402

# World layout. The corridor runs along -x; interior z is +-HALF, floor y=0.
HALF = 55.5
WALL = 6.0
X_IN, X_OUT = -136.0, -284.0
ALCOVE_X = -211.0
GAP = 14.2
ALCOVE_DEPTH = 28.0
TOP = 64.0
BEAM_H = 10.0  # same band height as the lobby ring beam
BOX_TOP = TOP - 1.0  # gate boxes end just below the wooden beams
WEDGE_MIN_ANGLE = 157.4
WEDGE_MIN_RADIUS = 140.0

# Texture ids per lobby variant: (banded trim, marble, rust, starfield, first added gate
# texture, carved panel, fluted shaft); the
# added textures (brick, stone, metal) come from build_lobby_textures.py.
TEXTURES = {
    "000": (9, 11, 12, 13, 15, 6, 8), "00k": (9, 11, 12, 13, 15, 6, 8),
    "0dk": (10, 12, 13, 14, 17, 7, 9), "h0k": (10, 12, 13, 14, 17, 7, 9),
    "hdk": (10, 12, 13, 14, 17, 7, 9),
}


def corridor_specs(stripes, marble, rust, space, added, panel, shaft):
    brick, stone, metal = added, added + 1, added + 2
    box = lambda *v: ",".join("%g" % x for x in v)
    x_end_wall = X_OUT - WALL
    ax0, ax1 = ALCOVE_X - GAP, ALCOVE_X + GAP
    z_in, z_wall = HALF, HALF + WALL
    z_back = z_wall + ALCOVE_DEPTH
    floor = [box(x_end_wall - ALCOVE_DEPTH, X_IN, -6, 0, -z_back, z_back)]
    pillars = []
    caps = []
    for x in (-152.0, -270.0):
        for z in (z_in, -z_wall):
            # Fluted shaft with a carved-panel block under the beam, like the lobby ring.
            pillars.append(box(x - 3, x + 3, 0, TOP - 12, z, z + WALL))
            caps.append(box(x - 4.5, x + 4.5, TOP - 12, TOP, z - 1.5, z + WALL + 1.5))
    # The beams run unbroken over the gate boxes, which end just below them.
    beams = []
    for z in (z_in, -z_wall):
        beams.append(box(x_end_wall, X_IN, TOP, TOP + BEAM_H, z, z + WALL))
    beams.append(box(x_end_wall, x_end_wall + WALL, TOP, TOP + BEAM_H, -z_wall, z_wall))
    # Alcove walls and roofs (solid + visible).
    # Gate alcoves: Station Square (-z) brick, Egg Carrier (+z) metal,
    # Mystic Ruins (end) stone.
    alcoves = {1: [], -1: []}
    for sign in (1, -1):
        lo, hi = (z_in, z_back) if sign > 0 else (-z_back, -z_in)
        a = alcoves[sign]
        a.append(box(ax0 - WALL, ax0, 0, BOX_TOP - 5, lo, hi))
        a.append(box(ax1, ax1 + WALL, 0, BOX_TOP - 5, lo, hi))
        back = (z_back, z_back + WALL) if sign > 0 else (-z_back - WALL, -z_back)
        a.append(box(ax0 - WALL, ax1 + WALL, 0, BOX_TOP, back[0], back[1]))
        # Roof sits just under the beam height and inset so it never shares a plane with it.
        a.append(box(ax0 - WALL, ax1 + WALL, BOX_TOP - 5, BOX_TOP,
                     lo + (0.5 if sign > 0 else 0), hi - (0.5 if sign < 0 else 0)))
    ex_back = x_end_wall - ALCOVE_DEPTH
    end_alcove = [
        box(ex_back - WALL, ex_back, 0, BOX_TOP, -GAP - WALL, GAP + WALL),
        box(ex_back, X_OUT, 0, BOX_TOP - 5, GAP, GAP + WALL),
        box(ex_back, X_OUT, 0, BOX_TOP - 5, -GAP - WALL, -GAP),
        box(ex_back - WALL, X_OUT, BOX_TOP - 5, BOX_TOP, -GAP - WALL, GAP + WALL)]
    # Gold edge line continuing the lobby floor rim (texture 0 is the gold one
    # in every variant); runs along both sides, skipping the gate alcove mouths.
    gold = []
    for z0, z1 in ((z_in - 3.0, z_in), (-z_in, -z_in + 3.0)):
        gold.append(box(x_end_wall, ax0, -1, 0.4, z0, z1))
        gold.append(box(ax1, X_IN, -1, 0.4, z0, z1))
    gold.append(box(x_end_wall, x_end_wall + 3.0, -1, 0.4, GAP, z_in))
    gold.append(box(x_end_wall, x_end_wall + 3.0, -1, 0.4, -z_in, -GAP))
    boxes = []
    boxes.append("%d,0.015625:%s" % (space, ";".join(floor)))
    boxes.append("%d,0.125:%s" % (shaft, ";".join(pillars)))
    boxes.append("%d,0.111:%s" % (panel, ";".join(caps)))
    boxes.append("%d,0.1:%s" % (stripes, ";".join(beams)))
    boxes.append("%d,0.015625:%s" % (metal, ";".join(alcoves[1])))
    boxes.append("%d,0.015625:%s" % (brick, ";".join(alcoves[-1])))
    boxes.append("%d,0.015625:%s" % (stone, ";".join(end_alcove)))
    # Invisible side and end barriers with a gap at each gate alcove.
    slabs = []
    # Solid skirt under the corridor floor so nothing can slip beneath its edge
    # from the lower staircase steps.
    slabs.append(box(x_end_wall - ALCOVE_DEPTH, X_IN + 2, -80, -6, -z_back, z_back))
    for sign in (1, -1):
        lo, hi = (z_in, z_wall) if sign > 0 else (-z_wall, -z_in)
        slabs.append(box(x_end_wall, ax0, 0, 40, lo, hi))
        slabs.append(box(ax1, X_IN, 0, 40, lo, hi))
    # The stock staircase edge runs diagonally from (-145, 0) to (-134, 55); with
    # the wall cut away nothing stops the player stepping off it, so stair-step
    # an invisible wall just outside that edge.
    z0 = 8.0
    while z0 < HALF:
        z1 = min(z0 + 4.0, z_wall)
        edge = -145.0 + 0.2 * z0
        slabs.append(box(-152, edge + 0.5, -40, -0.5, z0, z1))
        z0 = z1
    slabs.append(box(x_end_wall, x_end_wall + WALL, 0, 40, GAP, z_wall))
    slabs.append(box(x_end_wall, x_end_wall + WALL, 0, 40, -z_wall, -GAP))
    visual = "0,0.015625:%s" % ";".join(gold)
    return "|".join(boxes), visual, ";".join(slabs)


def build_corridor(variant, path):
    boxes, visual, slabs = corridor_specs(*TEXTURES[variant])
    fills = []
    for s in (1, -1):
        for y0, y1 in ((24.2, 33.0), (-33.0, -24.2)):
            pts = [(-134.4, y0, s * 55.7), (-152.4, y0, s * 63.1),
                   (-152.4, y1, s * 63.1), (-134.4, y1, s * 55.7)]
            fills.append("%d,0.1,0,0.1,0:%s" % (
                TEXTURES[variant][0], ";".join("%g,%g,%g" % p for p in pts)))
    env = dict(os.environ, GARDEN_COL_RANGE="0,0", GARDEN_GATE="none",
               GARDEN_FILLS="|".join(fills),
               GARDEN_BOXES=boxes, GARDEN_VISUAL_BOXES=visual, GARDEN_SLABS=slabs,
               GARDEN_WATER_IDS="9999")
    subprocess.run([sys.executable, CONVERT, SAMODEL, SOURCE, path],
                   env=env, check=True, capture_output=True)


def rot_y(x, z, bams):
    a = bams * 2 * math.pi / 65536
    return x * math.cos(a) + z * math.sin(a), -x * math.sin(a) + z * math.cos(a)


def in_wedge(point):
    x, z = point
    return (math.hypot(x, z) >= WEDGE_MIN_RADIUS
            and abs(math.degrees(math.atan2(z, x))) >= WEDGE_MIN_ANGLE)


def strip_triangles(loops):
    for i in range(len(loops) - 2):
        if i % 2 == 0:
            yield loops[i], loops[i + 1], loops[i + 2]
        else:
            yield loops[i], loops[i + 2], loops[i + 1]


def cut_gc(attach, obj, counters):
    from System.Collections.Generic import List
    from SAModel.GC import GCPrimitive, GCPrimitiveType, GCIndexAttributeFlags
    from System.Reflection import BindingFlags
    positions = [vs for vs in attach.VertexData if str(vs.Attribute) == "Position"][0].Data
    px, pz = float(obj.Position.X), float(obj.Position.Z)
    rot = int(obj.Rotation.Y)

    def world(index):
        v = positions[index].__implementation__
        x, z = rot_y(v.X, v.Z, rot)
        return x + px, z + pz

    for meshes in (attach.OpaqueMeshes, attach.TranslucentMeshes):
        last_flags = [None]
        for mesh in meshes:
            for p in mesh.Parameters:
                if type(p).__name__ == "IndexAttributeParameter":
                    last_flags[0] = p.IndexAttributes
            kept = []
            removed = 0
            for prim in mesh.Primitives:
                kind = str(prim.PrimitiveType)
                loops = list(prim.Loops)
                if kind == "Triangles":
                    tris = [tuple(loops[i:i + 3]) for i in range(0, len(loops) - 2, 3)]
                elif kind == "TriangleStrip":
                    tris = list(strip_triangles(loops))
                else:
                    raise ValueError("Unsupported GC primitive: " + kind)
                for tri in tris:
                    if all(in_wedge(world(l.PositionIndex)) for l in tri):
                        removed += 1
                    else:
                        kept.append(tri)
            if not removed:
                continue
            counters["gc"] += removed
            new_prim = GCPrimitive(GCPrimitiveType.Triangles)
            for tri in kept:
                for loop in tri:
                    new_prim.Loops.Add(loop)
            mesh.Primitives.Clear()
            if kept:
                mesh.Primitives.Add(new_prim)
            flags = last_flags[0]
            size = len(new_prim.GetBytes(flags)) if kept else 0
            mesh_type = clr.GetClrType(type(mesh))
            mesh_type.GetField("_primitiveSize",
                               BindingFlags.NonPublic | BindingFlags.Instance
                               ).SetValue(mesh, System.UInt32(size))


def cut_basic(attach, obj, counters):
    px, pz = float(obj.Position.X), float(obj.Position.Z)
    meshes = attach.Mesh
    for mi in range(len(meshes)):
        ms = meshes[mi]
        kept = []
        removed = 0
        for poly in ms.Poly:
            idx = [int(i) for i in poly.Indexes]
            kind = str(ms.PolyType)
            if kind == "Triangles":
                tris = [(0, 1, 2)]
            elif kind == "Quads":
                tris = [(0, 1, 2), (2, 1, 3)]
            elif kind == "Strips":
                reverse = bool(poly.Reversed)
                tris = []
                for i in range(len(idx) - 2):
                    flip = (i % 2 == 1) != reverse
                    tris.append((i, i + 2, i + 1) if flip else (i, i + 1, i + 2))
            else:
                raise ValueError("Unsupported poly type: " + kind)
            for tri in tris:
                corner = [idx[c] for c in tri]
                pts = [(attach.Vertex[i].X + px, attach.Vertex[i].Z + pz) for i in corner]
                if all(in_wedge(p) for p in pts):
                    removed += 1
                else:
                    kept.append(corner)
        if not removed:
            continue
        counters["basic"] += removed
        polys = [Triangle(*[System.UInt16(i) for i in t]) for t in kept]
        new = NJS_MESHSET(System.Array[Poly](polys), False, False, False)
        new.MaterialID = ms.MaterialID
        new.PolyName = ms.PolyName
        meshes[mi] = new


def cut_table(table):
    counters = {"gc": 0, "basic": 0}
    for col in table.COL:
        for obj in col.Model.GetObjects():
            attach = obj.Attach
            if attach is None:
                continue
            if attach.GetType().Name == "GCAttach":
                cut_gc(attach, obj, counters)
            elif attach.GetType().Name == "BasicAttach":
                cut_basic(attach, obj, counters)
    return counters


def is_gc(col):
    return any(o.Attach is not None and o.Attach.GetType().Name == "GCAttach"
               for o in col.Model.GetObjects())


def main(stock_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for variant in TEXTURES:
        stock_path = os.path.join(stock_dir, "objLandTableLobby%s.sa2blvl" % variant)
        corridor_path = os.path.join(tempfile.mkdtemp(), "corridor.sa2blvl")
        build_corridor(variant, corridor_path)
        table = LandTable.LoadFromFile(stock_path)
        corridor = LandTable.LoadFromFile(corridor_path)
        counters = cut_table(table)
        stock_gc = [c for c in table.COL if is_gc(c)]
        stock_basic = [c for c in table.COL if not is_gc(c)]
        corridor_gc = [c for c in corridor.COL if is_gc(c)]
        corridor_basic = [c for c in corridor.COL if not is_gc(c)]
        table.COL.Clear()
        for col in stock_gc + corridor_gc + stock_basic + corridor_basic:
            table.COL.Add(col)
        out = os.path.join(out_dir, "Lobby%s.sa2blvl" % variant)
        table.SaveToFile(out, LandTableFormat.SA2B, False)
        print(variant, "cut", counters, "cols", len(table.COL),
              "corridor gc/basic", len(corridor_gc), len(corridor_basic), "->", out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

