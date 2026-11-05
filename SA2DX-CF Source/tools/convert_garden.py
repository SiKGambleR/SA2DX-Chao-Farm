"""Converts a SADX garden land table (.sa1lvl) into an SA2B land table (.sa2blvl).

Runs on the user's own SADX data. It drives the SAModel library from X-Hax SA
Tools (GPL-3.0) as an external dependency through pythonnet; nothing from it is
copied into or shipped with the mod. It emits textured GC geometry and
preserves solid BasicAttach collision models.

usage: python convert_garden.py <SAModel.dll> <input.sa1lvl> <output.sa2blvl>
"""
import math
import sys

from pythonnet import load

load("coreclr")
import clr  # noqa: E402
import System  # noqa: E402,F401


import os

# Source COLs with no visible geometry that wall off the passage behind the
# waterfall: a vertical plane at x=-7 and the tunnel's slanted guard strips.
def _ids(name, default):
    value = os.environ.get(name)
    if value is None:
        return default
    return {int(v) for v in value.split(",") if v.strip()}


# Per-map overrides: GARDEN_WALL_COLS, GARDEN_WATER_IDS (comma lists) and
# GARDEN_GATE ("x,y,z,texture", or "none"). Defaults are the Mystic Ruins map.
INVISIBLE_WALL_COLS = _ids("GARDEN_WALL_COLS", {5})
# COLs removed outright (visible or not), and hand-built solid slabs
# "x0,x1,y0,y1,z0,z1;..." that replace broken source collision.
DROP_COLS = _ids("GARDEN_DROP_COLS", set())
# Visible COLs that also get a solid collision copy built from their own mesh.
SOLID_VISIBLE = _ids("GARDEN_SOLID_VISIBLE", set())
SLABS = [tuple(float(v) for v in s.split(","))
         for s in os.environ.get("GARDEN_SLABS", "").split(";") if s.strip()]

# Water textures are exported to the plugin's blended overlay.
WATER_TEXTURE_IDS = _ids("GARDEN_WATER_IDS", {44, 17})
# Stone gate at the tunnel entrance: centre x, floor y, centre z, texture id.
_gate = os.environ.get("GARDEN_GATE", "122,15,-159,20")
GATE_ENABLED = _gate != "none"
GATE_X = GATE_Y = GATE_Z = 0.0
GATE_TEXTURE = 0
if GATE_ENABLED:
    _g = _gate.split(",")
    GATE_X, GATE_Y, GATE_Z, GATE_TEXTURE = float(_g[0]), float(_g[1]), float(_g[2]), int(_g[3])


def main(dll, source, target, textured_arg="textured"):
    clr.AddReference(dll)
    from SAModel import LandTable, LandTableFormat, Basic_PolyType
    from SAModel.GC import (GCAttach, GCVertexSet, GCVertexAttribute, GCDataType,
                            GCStructType, GCMesh, GCPrimitive, GCPrimitiveType, Loop,
                            IndexAttributeParameter, GCIndexAttributeFlags,
                            BlendAlphaParameter, GCBlendModeControl, GCParameter,
                            VtxAttrFmtParameter, DiffuseColorParameter,
                            TexTEVModeParameter, TexCoordGenParameter, StripFlagsParameter, Color,
                            TextureParameter, GCTileMode, Vector3, UV)
    from System.Collections.Generic import List
    from System.Reflection import BindingFlags

    textured = textured_arg == "textured"
    mesh_type = clr.GetClrType(GCMesh)

    table = LandTable.LoadFromFile(source)
    collision_table = LandTable.LoadFromFile(source)
    converted = 0
    collision_copies = 0
    visible_flag = 0x80000000
    solid_flag = 0x00000001

    def triangles(poly_type, poly):
        tris, idx = triangles_raw(poly_type, poly)
        if os.environ.get("GARDEN_FLIP"):
            tris = [(a, c, b) for a, b, c in tris]
        return tris, idx

    def triangles_raw(poly_type, poly):
        idx = [int(i) for i in poly.Indexes]
        if poly_type == Basic_PolyType.Triangles:
            return [(0, 1, 2)], idx
        if poly_type == Basic_PolyType.Quads:
            return [(0, 1, 2), (2, 1, 3)], idx
        if poly_type == Basic_PolyType.Strips:
            tris = []
            reverse = bool(poly.Reversed)
            for i in range(len(idx) - 2):
                flip = (i % 2 == 1) != reverse
                tris.append((i, i + 2, i + 1) if flip else (i, i + 1, i + 2))
            return tris, idx
        return [(0, i, i + 1) for i in range(1, len(idx) - 1)], idx

    counter = [0]

    water_triangles = []

    def build(basic, owner=None):
        counter[0] += 1
        base = 'gcmodel_%d' % counter[0]
        gc = GCAttach()
        gc.Name = 'attach_' + base
        gc.Bounds = basic.Bounds
        gc.VertexName = base + '_vertex'
        gc.OpaqueMeshName = base + '_opaque'
        gc.TranslucentMeshName = base + '_translucent'
        positions = GCVertexSet(GCVertexAttribute.Position, GCDataType.Float32,
                                GCStructType.Position_XYZ)
        for v in basic.Vertex:
            positions.Data.Add(Vector3(v.X, v.Y, v.Z))
        sets = [positions]
        normals = None
        colors = None
        if textured:
            # Stock Neutral meshes use a vertex colour channel and no normals.
            colors = GCVertexSet(GCVertexAttribute.Color0)
            for _ in basic.Vertex:
                colors.Data.Add(Color(255, 255, 255, 255))
            sets.append(colors)
        elif basic.Normal is not None and len(basic.Normal) == len(basic.Vertex):
            normals = GCVertexSet(GCVertexAttribute.Normal, GCDataType.Float32,
                                  GCStructType.Normal_XYZ)
            for n in basic.Normal:
                normals.Data.Add(Vector3(n.X, n.Y, n.Z))
            sets.append(normals)
        meshes = ([], [])
        uv_set = None
        if textured:
            uv_set = GCVertexSet(GCVertexAttribute.Tex0)
            sets.append(uv_set)
        for ms in basic.Mesh:
            corner_count = sum(len(p.Indexes) for p in ms.Poly)
            uvs = ms.UV if ms.UV is not None else None
            has_uv = (
                textured and uvs is not None and len(uvs) == corner_count
            )
            material = None
            if basic.Material is not None and 0 <= int(ms.MaterialID) < len(basic.Material):
                material = basic.Material[int(ms.MaterialID)]
            mirror_u = has_uv and material is not None and bool(material.FlipU)

            if (has_uv and material is not None and owner is not None
                    and int(material.TextureID) in WATER_TEXTURE_IDS):
                # The land renderer ignores alpha, so water is exported for the
                # plugin's own blended overlay instead of being drawn here.
                if any(abs(float(a) - b) > 1e-4 for a, b in
                       ((owner.Rotation.X, 0), (owner.Rotation.Y, 0), (owner.Rotation.Z, 0))):
                    raise ValueError("Water object rotation is not supported.")
                px, py, pz = float(owner.Position.X), float(owner.Position.Y), float(owner.Position.Z)
                sx, sy, sz = float(owner.Scale.X), float(owner.Scale.Y), float(owner.Scale.Z)
                base = 0
                for poly in ms.Poly:
                    tris, idx = triangles(ms.PolyType, poly)
                    for tri in tris:
                        corners = []
                        for c in tri:
                            v = basic.Vertex[idx[c]]
                            uv = uvs[base + c]
                            corners.append((v.X * sx + px, v.Y * sy + py, v.Z * sz + pz,
                                            float(uv.U), float(uv.V)))
                        shift_u = math.floor(min(c[3] for c in corners))
                        shift_v = math.floor(min(c[4] for c in corners))
                        water_triangles.append((int(material.TextureID), [
                            (c[0], c[1], c[2], c[3] - shift_u, c[4] - shift_v)
                            for c in corners]))
                    base += len(idx)
                continue

            # Match stock's 8-bit indices unless this mesh's index range requires 16-bit.
            flags = GCIndexAttributeFlags.HasPosition
            if len(basic.Vertex) > 256:
                flags |= GCIndexAttributeFlags.Position16BitIndex
            if colors is not None:
                flags |= GCIndexAttributeFlags.HasColor
                if len(basic.Vertex) > 256:
                    flags |= GCIndexAttributeFlags.Color16BitIndex
            if normals is not None:
                flags |= GCIndexAttributeFlags.HasNormal | GCIndexAttributeFlags.Normal16BitIndex
            if has_uv:
                flags |= GCIndexAttributeFlags.HasUV
                if uv_set.Data.Count + corner_count > 256:
                    flags |= GCIndexAttributeFlags.UV16BitIndex

            alpha = material is not None and (
                bool(material.UseAlpha) or int(material.TextureID) in WATER_TEXTURE_IDS)
            params = List[GCParameter]()
            if has_uv:
                # Stock SA2B meshes always set position and normal formats first.
                pos_fmt = VtxAttrFmtParameter(GCVertexAttribute.Position)
                pos_fmt.Data = System.UInt32(70656)
                params.Add(pos_fmt)
                nrm_fmt = VtxAttrFmtParameter(GCVertexAttribute.Normal)
                nrm_fmt.Data = System.UInt32(223744)
                params.Add(nrm_fmt)
            params.Add(IndexAttributeParameter(flags))
            if has_uv:
                params.Add(VtxAttrFmtParameter(GCVertexAttribute.Tex0))
            blend = BlendAlphaParameter()
            if alpha:
                blend.SourceAlpha = GCBlendModeControl.SrcAlpha
                blend.DestAlpha = GCBlendModeControl.InverseSrcAlpha
            else:
                blend.SourceAlpha = GCBlendModeControl.SrcAlpha
                blend.DestAlpha = GCBlendModeControl.InverseSrcAlpha
            params.Add(blend)
            if alpha and not material.UseAlpha:
                # Neutral's own water meshes use this exact blend word.
                blend.Data = System.UInt32(0x6500)
            if has_uv:
                # Despite its name this sets GX colour channel and texgen counts (1 and 1).
                strip = StripFlagsParameter()
                strip.Data = System.UInt32(68369)
                if os.environ.get("GARDEN_IGNORE_AMBIENT"):
                    # SAModel's StripFlags setter writes the flag byte to the wrong field.
                    strip.Data = System.UInt32(0x10B17)
                params.Add(strip)
                if material is not None and int(material.TextureID) in WATER_TEXTURE_IDS:
                    from SAModel.GC import AmbientColorParameter
                    ambient = AmbientColorParameter()
                    ambient.Data = System.UInt32(0x60FFFFFF)
                    params.Add(ambient)
            if has_uv and material is not None:
                is_water = int(material.TextureID) in WATER_TEXTURE_IDS
                diffuse = DiffuseColorParameter()
                c = material.DiffuseColor
                diffuse.Data = System.UInt32(
                    (int(c.A) << 24) | (int(c.R) << 16) | (int(c.G) << 8) | int(c.B)
                )
                if not is_water:
                    # Stock water meshes have no diffuse parameter at all.
                    params.Add(diffuse)
                tile = GCTileMode.WrapU | GCTileMode.WrapV
                if material.ClampU:
                    tile &= ~GCTileMode.WrapU
                if material.FlipU:
                    tile |= GCTileMode.MirrorU
                params.Add(TextureParameter(int(os.environ.get("GARDEN_FORCE_TEX", material.TextureID)), tile))
                tev = TexTEVModeParameter()
                tev.Data = System.UInt32(4)
                params.Add(tev)
                tcg = TexCoordGenParameter()
                tcg.Data = System.UInt32(4170)
                params.Add(tcg)

            primitive = GCPrimitive(GCPrimitiveType.Triangles)
            corner_base = 0
            for poly in ms.Poly:
                tris, idx = triangles(ms.PolyType, poly)
                if any(index < 0 or index >= len(basic.Vertex) for index in idx):
                    raise ValueError("A mesh polygon references a vertex outside its attach.")
                for tri in tris:
                    shift_u = shift_v = 0.0
                    if has_uv:
                        # Rebase each triangle by whole texture repeats so the
                        # stored coordinates stay small; tiling is unchanged.
                        us = [float(uvs[corner_base + c].U) for c in tri]
                        vs = [float(uvs[corner_base + c].V) for c in tri]
                        step_u = 2.0 if mirror_u else 1.0
                        shift_u = math.floor(min(us) / step_u) * step_u
                        shift_v = math.floor(min(vs))
                    for corner in tri:
                        loop = Loop()
                        loop.PositionIndex = idx[corner]
                        loop.NormalIndex = idx[corner]
                        if colors is not None:
                            loop.Color0Index = idx[corner]
                        if has_uv:
                            src_uv = uvs[corner_base + corner]
                            loop.UV0Index = len(uv_set.Data)
                            uv_set.Data.Add(UV(float(src_uv.U) - shift_u,
                                               float(src_uv.V) - shift_v))
                        primitive.Loops.Add(loop)
                corner_base += len(idx)
            prims = List[GCPrimitive]()
            prims.Add(primitive)
            mesh = GCMesh(params, prims)
            # SAModel under-counts nothing here but over-counts UV bytes, so set the real size.
            size = len(primitive.GetBytes(flags))
            mesh_type.GetField(
                "_primitiveSize", BindingFlags.NonPublic | BindingFlags.Instance
            ).SetValue(mesh, System.UInt32(size))
            mesh.ParameterName = '%s_param_%d' % (base, counter[0] * 1000 + len(meshes[0]) + len(meshes[1]))
            mesh.PrimitiveName = mesh.ParameterName.replace('_param_', '_prim_')
            meshes[1 if alpha else 0].append(mesh)

        for s in sets:
            s.DataName = '%s_%s' % (base, str(s.Attribute).lower())
            gc.VertexData.Add(s)
        for m in meshes[0]:
            gc.OpaqueMeshes.Add(m)
        for m in meshes[1]:
            gc.TranslucentMeshes.Add(m)
        return gc

    # SA2 flags and COL ordering come from the library's own SA1 -> SA2 conversion.
    import os
    import tempfile
    scratch = os.path.join(tempfile.mkdtemp(), "level.sa2lvl")
    table.SaveToFile(scratch, LandTableFormat.SA2, False)
    reference = LandTable.LoadFromFile(scratch)

    def key(col):
        c = col.Bounds.Center
        return (round(c.X, 2), round(c.Y, 2), round(c.Z, 2), round(col.Bounds.Radius, 2))

    pending = {}
    for index, col in enumerate(reference.COL):
        pending.setdefault(key(col), []).append((index, col))
    ordered = []
    for source_index, col in enumerate(table.COL):
        index, ref = pending[key(col)].pop(0)
        col.Flags = ref.Flags
        ordered.append((index, source_index, col))
    ordered.sort(key=lambda pair: pair[0])

    render_cols = []
    water_cols = []

    def all_empty(col):
        found = False
        for obj in col.Model.GetObjects():
            attach = obj.Attach
            if attach is not None and attach.GetType().Name == "GCAttach":
                found = True
                if attach.OpaqueMeshes.Count + attach.TranslucentMeshes.Count:
                    return False
        return found
    basic_cols = []
    expected_solid_collision_cols = 0
    for _, source_index, col in ordered:
        if os.environ.get("GARDEN_COL_RANGE"):
            lo, hi = (int(v) for v in os.environ["GARDEN_COL_RANGE"].split(","))
            if not lo <= source_index < hi:
                continue
        flags = int(col.Flags) & 0xFFFFFFFF
        if source_index in DROP_COLS:
            continue
        if source_index in SOLID_VISIBLE:
            flags |= solid_flag
        if source_index in INVISIBLE_WALL_COLS and not flags & visible_flag:
            continue
        if flags & 0x2 and not flags & solid_flag:
            # SADX water plane: draw it, but keep it purely visual so the player
            # never enters a swimming state in the pool.
            has_textured = any(
                ms.UV is not None
                for obj in col.Model.GetObjects()
                if obj.Attach is not None
                and obj.Attach.GetType().Name == "BasicAttach"
                for ms in obj.Attach.Mesh)
            if not has_textured:
                if int(col.Flags) & 0xFFFFFFFF == 0x20000002 and os.environ.get("GARDEN_WATER_PLANE"):
                    # Invisible water-volume plane: give it the pool texture.
                    tex_id = int(os.environ["GARDEN_WATER_PLANE"])
                    for obj in col.Model.GetObjects():
                        att = obj.Attach
                        if att is None or att.GetType().Name != "BasicAttach":
                            continue
                        px, py, pz = float(obj.Position.X), float(obj.Position.Y), float(obj.Position.Z)
                        for ms in att.Mesh:
                            for poly in ms.Poly:
                                tris, idx = triangles(ms.PolyType, poly)
                                for tri in tris:
                                    cs = []
                                    for c in tri:
                                        v = att.Vertex[idx[c]]
                                        x, y, z = v.X + px, v.Y + py, v.Z + pz
                                        cs.append((x, y, z, x / 32.0, z / 32.0))
                                    su = math.floor(min(c[3] for c in cs))
                                    sv = math.floor(min(c[4] for c in cs))
                                    water_triangles.append((tex_id, [
                                        (c[0], c[1], c[2], c[3] - su, c[4] - sv) for c in cs]))
                continue
            for obj in col.Model.GetObjects():
                attach = obj.Attach
                if attach is not None and attach.GetType().Name == "BasicAttach":
                    obj.Attach = build(attach, obj)
                    converted += 1
            col.Flags = System.Int32(visible_flag - (1 << 32))
            if all_empty(col):
                converted -= 1
                continue
            water_cols.append(col)
            continue
        if flags & solid_flag:
            expected_solid_collision_cols += 1
        if flags & visible_flag:
            if not flags & solid_flag and flags & 0x20:
                col.Flags = System.Int32((flags & ~0x20) - (1 << 32))
            for obj in col.Model.GetObjects():
                attach = obj.Attach
                if attach is not None and attach.GetType().Name == "BasicAttach":
                    obj.Attach = build(attach, obj)
                    converted += 1
            if all_empty(col):
                # A COL that only held water has nothing left to draw.
                converted -= 1
            else:
                render_cols.append(col)
            if flags & solid_flag:
                collision_col = collision_table.COL[source_index]
                strip = int(os.environ.get("GARDEN_STRIP_FLAGS", "0"), 0)
                collision_col.Flags = flags & ~visible_flag & ~strip
                # Stock terrain render COLs carry only visible | 0x8000.
                col.Flags = System.Int32((visible_flag | 0x8000) - (1 << 32))
                for object_index, obj in enumerate(
                    collision_col.Model.GetObjects()
                ):
                    obj.Name = "%s_collision_%d_%d" % (
                        obj.Name,
                        source_index,
                        object_index,
                    )
                    shared = obj.Attach
                    if shared is not None and shared.GetType().Name == "BasicAttach":
                        # Labels must be unique: maps repeat one model in many COLs.
                        tag = "_c%d_%d" % (source_index, object_index)
                        for field in ("Name", "VertexName", "NormalName",
                                      "MeshName", "MaterialName"):
                            value = getattr(shared, field)
                            if value:
                                setattr(shared, field, value + tag)
                        if shared.Mesh is not None:
                            for ms in shared.Mesh:
                                for field in ("PolyName", "PolyNormalName",
                                              "UVName", "VColorName"):
                                    value = getattr(ms, field)
                                    if value:
                                        setattr(ms, field, value + tag)
                if not os.environ.get("GARDEN_NO_COLLISION"):
                    basic_cols.append(collision_col)
                collision_copies += 1
        else:
            strip = int(os.environ.get("GARDEN_STRIP_FLAGS", "0"), 0)
            if strip and flags & strip:
                col.Flags = (flags & ~strip) - ((flags & ~strip) >> 31 << 32)
            basic_cols.append(col)

    def box_attach(boxes, texture=None, uv_scale=None):
        from SAModel import (BasicAttach, NJS_MESHSET, NJS_MATERIAL, Triangle, Poly,
                             BoundingSphere, Vertex)
        from SAModel import UV as BasicUV
        verts = []
        polys = []
        uvs = []
        for x0, x1, y0, y1, z0, z1 in boxes:
            b = len(verts)
            for x in (x0, x1):
                for y in (y0, y1):
                    for z in (z0, z1):
                        verts.append(Vertex(x, y, z))
            # corner index = (x * 4) + (y * 2) + z
            faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1),
                     (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
            for f in faces:
                for tri in ((f[0], f[1], f[2]), (f[0], f[2], f[3])):
                    polys.append(Triangle(*[System.UInt16(b + i) for i in tri]))
                    for i in tri:
                        if uv_scale:
                            p = verts[b + i]
                            fv = [verts[b + k] for k in f]
                            if len({round(q.X, 3) for q in fv}) == 1:
                                a_, b_ = p.Z, p.Y
                            elif len({round(q.Y, 3) for q in fv}) == 1:
                                a_, b_ = p.X, p.Z
                            else:
                                a_, b_ = p.X, p.Y
                            uvs.append(BasicUV(float(a_ * uv_scale), float(b_ * uv_scale)))
                        else:
                            corner = [(0, 0), (0, 1), (1, 1), (1, 0)][f.index(i)]
                            uvs.append(BasicUV(float(corner[0]), float(corner[1])))
        meshset = NJS_MESHSET(System.Array[Poly](polys), False, False, False)
        meshset.UV = System.Array[BasicUV](uvs)
        material = NJS_MATERIAL()
        material.TextureID = GATE_TEXTURE if texture is None else texture
        material.UseAlpha = False
        material.DoubleSided = True
        material.UseTexture = True
        normals = System.Array[Vertex]([Vertex(0, 1, 0)] * len(verts))
        mesh_list = List[NJS_MESHSET]()
        mesh_list.Add(meshset)
        material_list = List[NJS_MATERIAL]()
        material_list.Add(material)
        attach = BasicAttach(System.Array[Vertex](verts), normals, mesh_list, material_list)
        counter[0] += 1
        n = "gategeo_%d" % counter[0]
        attach.Name = n
        attach.VertexName = n + "_vertex"
        attach.NormalName = n + "_normal"
        attach.MeshName = n + "_mesh"
        attach.MaterialName = n + "_material"
        meshset.PolyName = n + "_poly"
        meshset.UVName = n + "_uv"
        xs = [v.X for v in verts]
        ys = [v.Y for v in verts]
        zs = [v.Z for v in verts]
        centre = Vertex((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2,
                        (min(zs) + max(zs)) / 2)
        radius = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
        attach.Bounds = BoundingSphere(centre, float(radius))
        return attach, centre, float(radius)

    def gate_col(basic_attach, centre, radius, flags):
        from SAModel import COL, NJS_OBJECT, BoundingSphere
        obj = NJS_OBJECT()
        obj.Attach = basic_attach
        obj.Name = "garden_gate_%d" % counter[0]
        counter[0] += 1
        col = COL()
        col.Model = obj
        col.Bounds = BoundingSphere(centre, radius)
        col.Flags = System.Int32(flags - (1 << 32) if flags & 0x80000000 else flags)
        return col

    # Stone gate over the tunnel entrance, built from the Mystic stone texture.
    # Lobby gate proportions (from its collision): pillars 9 wide x 42 tall at
    # +-16 from the centre, 23 clear opening, 20 deep, lintel above. The opening
    # runs along x, so lobby local z maps to world -x and local x to world z.
    gx, gy, gz = GATE_X, GATE_Y, GATE_Z
    gate_boxes = [
        (gx - 6.0, gx + 24.0, gy, gy + 41, gz - 20.5, gz - 11.5),
        (gx - 6.0, gx + 24.0, gy, gy + 41, gz + 11.5, gz + 20.5),
        (gx - 6.0, gx + 24.0, gy + 41, gy + 62, gz - 20.5, gz + 20.5),
    ]
    if GATE_ENABLED:
        attach, centre, radius = box_attach(gate_boxes)
        render_gate = gate_col(attach, centre, radius, 0)
        render_gate.Model.Attach = build(attach)
        converted += 1
        render_gate.Flags = System.Int32((visible_flag | 0x8000) - (1 << 32))
        render_cols.append(render_gate)
        attach, centre, radius = box_attach(gate_boxes)
        basic_cols.append(gate_col(attach, centre, radius, solid_flag))
        expected_solid_collision_cols += 1

    # Visible patches over holes in the source walls. Each entry is
    # "tex,ua,ub,va,vb:x,y,z;x,y,z;..." (convex polygon, u = ua*x+ub, v = va*y+vb).
    for fill in [f for f in os.environ.get("GARDEN_FILLS", "").split("|") if f.strip()]:
        from SAModel import (BasicAttach, NJS_MESHSET, NJS_MATERIAL, Triangle, Poly,
                             BoundingSphere, Vertex)
        from SAModel import UV as BasicUV
        head, points = fill.split(":")
        tex, ua, ub, va, vb = head.split(",")
        pts = [tuple(float(c) for c in p.split(",")) for p in points.split(";")]
        verts = [Vertex(*p) for p in pts]
        polys = []
        uvs = []
        for i in range(1, len(pts) - 1):
            for k in (0, i, i + 1):
                uvs.append(BasicUV(float(ua) * pts[k][0] + float(ub),
                                   float(va) * pts[k][1] + float(vb)))
            polys.append(Triangle(System.UInt16(0), System.UInt16(i), System.UInt16(i + 1)))
        meshset = NJS_MESHSET(System.Array[Poly](polys), False, False, False)
        meshset.UV = System.Array[BasicUV](uvs)
        material = NJS_MATERIAL()
        material.TextureID = int(tex)
        material.UseAlpha = False
        material.DoubleSided = True
        material.UseTexture = True
        normals = System.Array[Vertex]([Vertex(0, 0, 1)] * len(verts))
        mesh_list = List[NJS_MESHSET]()
        mesh_list.Add(meshset)
        material_list = List[NJS_MATERIAL]()
        material_list.Add(material)
        attach = BasicAttach(System.Array[Vertex](verts), normals, mesh_list, material_list)
        counter[0] += 1
        n = "fill_%d" % counter[0]
        attach.Name = n
        attach.VertexName = n + "_vertex"
        attach.NormalName = n + "_normal"
        attach.MeshName = n + "_mesh"
        attach.MaterialName = n + "_material"
        meshset.PolyName = n + "_poly"
        meshset.UVName = n + "_uv"
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        zs = [p[2] for p in pts]
        centre = Vertex((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2,
                        (min(zs) + max(zs)) / 2)
        radius = float(max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)))
        attach.Bounds = BoundingSphere(centre, radius)
        render_fill = gate_col(attach, centre, radius, 0)
        render_fill.Model.Attach = build(attach)
        converted += 1
        render_fill.Flags = System.Int32((visible_flag | 0x8000) - (1 << 32))
        render_cols.append(render_fill)

    # GARDEN_BOXES "tex,uvscale[,colflags]:x0,x1,y0,y1,z0,z1;..." (items split
    # by "|"): solid, textured boxes with world-space UVs. colflags defaults to
    # plain solid; stock SA2 ground uses 0x40000001 and walls 0x20000001, and
    # the game's raycasts (mech walk steps, item ground snaps) miss COLs
    # without those bits.
    for spec in [s for s in os.environ.get("GARDEN_BOXES", "").split("|") if s.strip()]:
        head, body = spec.split(":")
        head_parts = head.split(",")
        tex, scale = head_parts[0], head_parts[1]
        colflags = int(head_parts[2], 0) if len(head_parts) > 2 else solid_flag
        if not colflags & solid_flag:
            raise RuntimeError("GARDEN_BOXES colflags must keep the solid bit.")
        boxes = [tuple(float(v) for v in b.split(",")) for b in body.split(";") if b.strip()]
        attach, centre, radius = box_attach(boxes, int(tex), float(scale))
        render_box = gate_col(attach, centre, radius, 0)
        render_box.Model.Attach = build(attach)
        converted += 1
        render_box.Flags = System.Int32((visible_flag | 0x8000) - (1 << 32))
        render_cols.append(render_box)
        attach, centre, radius = box_attach(boxes, int(tex), float(scale))
        basic_cols.append(gate_col(attach, centre, radius, colflags))
        expected_solid_collision_cols += 1

    # GARDEN_VISUAL_BOXES: same format, drawn only (no collision), so thin trim
    # such as floor edging cannot act as a step or barrier.
    for spec in [s for s in os.environ.get("GARDEN_VISUAL_BOXES", "").split("|") if s.strip()]:
        head, body = spec.split(":")
        tex, scale = head.split(",")
        boxes = [tuple(float(v) for v in b.split(",")) for b in body.split(";") if b.strip()]
        attach, centre, radius = box_attach(boxes, int(tex), float(scale))
        render_box = gate_col(attach, centre, radius, 0)
        render_box.Model.Attach = build(attach)
        converted += 1
        render_box.Flags = System.Int32((visible_flag | 0x8000) - (1 << 32))
        render_cols.append(render_box)

    if SLABS:
        slab_flags = int(os.environ.get("GARDEN_SLAB_FLAGS", "0") or "0", 0) or solid_flag
        if not slab_flags & solid_flag:
            raise RuntimeError("GARDEN_SLAB_FLAGS must keep the solid bit.")
        for slab in SLABS:
            attach, centre, radius = box_attach([slab])
            basic_cols.append(gate_col(attach, centre, radius, slab_flags))
            expected_solid_collision_cols += 1

    # Water draws last so its blend lands on top of the pool floor.
    table.COL.Clear()
    for col in render_cols + water_cols + basic_cols:
        table.COL.Add(col)

    basic_attach_count = sum(
        col.Model.Attach.GetType().Name == "BasicAttach"
        for col in basic_cols
    )
    if basic_attach_count != len(basic_cols):
        raise RuntimeError("The collision group contains a non-Basic attachment.")
    table.TextureList = 0
    if os.environ.get("GARDEN_DEBUG_NAMES"):
        seen_names = {}
        for ci, col in enumerate(table.COL):
            for obj in col.Model.GetObjects():
                labels = [("obj", obj.Name)]
                attach = obj.Attach
                if attach is not None:
                    labels.append(("attach", attach.Name))
                    if attach.GetType().Name == "GCAttach":
                        labels.append(("vtx", attach.VertexName))
                        for s in attach.VertexData:
                            labels.append(("vdata", s.DataName))
                        for m in list(attach.OpaqueMeshes) + list(attach.TranslucentMeshes):
                            labels.append(("param", m.ParameterName))
                            labels.append(("prim", m.PrimitiveName))
                for kind, label in labels:
                    if label in seen_names:
                        print("DUP", kind, label, seen_names[label], ci)
                    seen_names[label] = ci
    import struct
    with open(os.path.splitext(target)[0] + ".water", "wb") as handle:
        handle.write(struct.pack("<I", len(water_triangles)))
        for texture, corners in water_triangles:
            handle.write(struct.pack("<I", texture))
            for corner in corners:
                handle.write(struct.pack("<5f", *corner))
    table.SaveToFile(target, LandTableFormat.SA2B, False)
    verified = LandTable.LoadFromFile(target)
    if len(verified.COL) != len(table.COL) or int(verified.TextureList) != 0:
        raise RuntimeError("The saved SA2B land table failed its round-trip checks.")
    verified_gc_cols = 0
    verified_basic_cols = 0
    verified_basic_solid_cols = 0
    for col in verified.COL:
        attach = col.Model.Attach
        if attach is None:
            continue
        if attach.GetType().Name == "GCAttach":
            verified_gc_cols += 1
        elif attach.GetType().Name == "BasicAttach":
            verified_basic_cols += 1
            if int(col.Flags) & solid_flag:
                verified_basic_solid_cols += 1
                for obj in col.Model.GetObjects():
                    collision_attach = obj.Attach
                    if (
                        collision_attach is None
                        or collision_attach.GetType().Name != "BasicAttach"
                    ):
                        continue
                    meshes = (
                        list(collision_attach.Mesh)
                        if collision_attach.Mesh is not None
                        else []
                    )
                    if not meshes:
                        raise RuntimeError(
                            "A solid BasicAttach collision model has no mesh sets."
                        )
                    vertices = (
                        list(collision_attach.Vertex)
                        if collision_attach.Vertex is not None
                        else []
                    )
                    for mesh in meshes:
                        polygons = list(mesh.Poly) if mesh.Poly is not None else []
                        if not polygons:
                            raise RuntimeError(
                                "A solid BasicAttach collision mesh has no polygons."
                            )
                        for polygon in polygons:
                            indexes = [int(index) for index in polygon.Indexes]
                            if any(
                                index < 0 or index >= len(vertices)
                                for index in indexes
                            ):
                                raise RuntimeError(
                                    "A solid BasicAttach collision polygon references "
                                    "a vertex outside its attach."
                                )
        else:
            raise RuntimeError("The saved land table contains an unsupported COL attach.")
    if verified_gc_cols + verified_basic_cols != len(verified.COL):
        raise RuntimeError("The saved SA2B land table contains COLs without supported attaches.")
    if verified_gc_cols != converted:
        raise RuntimeError("The saved SA2B land table lost converted render attaches.")
    if verified_basic_cols != len(basic_cols):
        raise RuntimeError("The saved SA2B land table lost BasicAttach collision models.")
    if verified_basic_solid_cols != expected_solid_collision_cols:
        raise RuntimeError("The saved SA2B land table lost solid collision models.")
    if any(
        index >= verified_gc_cols
        and col.Model.Attach.GetType().Name == "GCAttach"
        for index, col in enumerate(verified.COL)
    ):
        raise RuntimeError("The saved SA2B COL order does not place GC models first.")
    for index, col in enumerate(verified.COL):
        if index >= verified_gc_cols and int(col.Flags) & visible_flag:
            raise RuntimeError("A BasicAttach collision COL is still marked visible.")
    print("Converted %d render attaches; wrote %d BasicAttach COLs including %d "
          "hidden collision copies; wrote %s"
          % (converted, verified_basic_cols, collision_copies, target))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
