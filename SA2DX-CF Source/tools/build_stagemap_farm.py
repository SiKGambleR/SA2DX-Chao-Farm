"""Adds the Chao Farm stage-select art to the game's stageMap texture archive.

Draws into the empty bottom strip of the stg_stagename atlas:
  - "CHAO FARM" composed from the atlas's own letters (CHAO WORLD, FINAL RUSH,
    METAL HARBOR), so the style matches exactly;
  - a 48x48 (96x96 in the HD pak) node tile: the Chao photo with a golden key.

Patches both the GVR atlas inside stageMap.prs and the 1024x1024 DXT5 DDS in
PRS\stageMap.pak (rewritten as uncompressed 32-bit). Prints the texanim uv
values the plugin needs.

Outputs: mod\gd_PC\stageMap.prs, mod\gd_PC\PRS\stageMap.pak, previews in build\.

usage: python build_stagemap_farm.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw

import gvr
import prs

GD = r"G:\SteamLibrary\steamapps\common\Sonic Adventure 2\resource\gd_PC"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PRS = os.path.join(ROOT, "mod", "gd_PC", "stageMap.prs")
OUT_PAK = os.path.join(ROOT, "mod", "gd_PC", "PRS", "stageMap.pak")

# Source strips in the stock atlas (uv units, with 1 unit of slack):
CHAO_WORLD_UV = (173, 221, 250, 230)   # C H A O W O R L D
FINAL_RUSH_UV = (93, 166, 161, 174)    # F I N A L R U S H
MISSION_ST_UV = (86, 134, 181, 142)    # M I S S I O N S T R E E T
NAME_POS_UV = (4, 232)    # paste origin of "CHAO FARM"
ICON_BOX_UV = (228, 231, 252, 255)  # 24x24 uv = 48px GVR, 96px DDS


def segment(img):
    """Splits a name strip into letter images on transparent columns."""
    w, h = img.size
    cols = [any(img.getpixel((x, y))[3] > 16 for y in range(h)) for x in range(w)]
    letters = []
    x = 0
    while x < w:
        if not cols[x]:
            x += 1
            continue
        start = x
        while x < w and cols[x]:
            x += 1
        letters.append((start, img.crop((start, 0, x, h))))
    return letters


def face_runs(strip):
    """Column runs that contain white letter-face pixels (not just the gray
    south-east extrusion), i.e. one run per letter."""
    w, h = strip.size
    px = strip.load()
    face = []
    for x in range(w):
        hit = False
        for y in range(h):
            r, g, b, a = px[x, y]
            if a > 200 and r > 200 and g > 200 and b > 200:
                hit = True
                break
        face.append(hit)
    runs = []
    x = 0
    while x < w:
        if not face[x]:
            x += 1
            continue
        start = x
        while x < w and face[x]:
            x += 1
        runs.append((start, x - 1))
    return runs


def compose_name(hi_atlas):
    """Builds 'CHAO FARM' at 4x from the stock name strips, so the font,
    shading and extrusion match every other stage name exactly. Letters from
    the smaller-row strips are rescaled to CHAO WORLD's cap height."""
    chao = hi_atlas.crop(tuple(v * 4 for v in CHAO_WORLD_UV))
    final = hi_atlas.crop(tuple(v * 4 for v in FINAL_RUSH_UV))
    mission = hi_atlas.crop(tuple(v * 4 for v in MISSION_ST_UV))
    cw = face_runs(chao)       # C H A O W O R L D
    fr = face_runs(final)      # F I N A L R U S H
    ms = face_runs(mission)    # M I S S I O N S T R E E T
    assert len(cw) == 9 and len(fr) == 9 and len(ms) == 13, (len(cw), len(fr), len(ms))
    tail = 8  # px of extrusion kept to the right of each letter face

    def face_bounds(img):
        px = img.load()
        rows = [y for y in range(img.height) for x in range(img.width)
                if px[x, y][3] > 200 and min(px[x, y][:3]) > 200]
        return min(rows), max(rows)

    def glyph(strip, runs, index):
        s, e = runs[index]
        # keep the extrusion tail but never the next letter's face
        limit = runs[index + 1][0] - 1 if index + 1 < len(runs) else strip.width
        img = strip.crop((s, 0, min(e + 1 + tail, limit), strip.height))
        return img, e - s + 1

    ref, _ = glyph(chao, cw, 0)
    ref_top, ref_bottom = face_bounds(ref)
    ref_height = ref_bottom - ref_top + 1

    def normalized(strip, runs, index):
        img, advance = glyph(strip, runs, index)
        top, bottom = face_bounds(img)
        height = bottom - top + 1
        if height != ref_height:
            factor = ref_height / height
            img = img.resize((max(1, round(img.width * factor)),
                              max(1, round(img.height * factor))), Image.LANCZOS)
            advance = max(1, round(advance * factor))
            top = round(top * factor)
        return img, advance, top

    seq = [normalized(chao, cw, 0), normalized(chao, cw, 1), normalized(chao, cw, 2),
           normalized(chao, cw, 3), None,
           normalized(final, fr, 0), normalized(chao, cw, 2), normalized(chao, cw, 6),
           normalized(mission, ms, 0)]
    gap = cw[1][0] - cw[0][1] - 1          # letter gap as in CHAO WORLD
    word_gap = cw[4][0] - cw[3][1] - 1     # the CHAO<->WORLD space
    width = sum(word_gap if g is None else g[1] + gap for g in seq) - gap + tail
    out = Image.new("RGBA", (width, chao.height), (0, 0, 0, 0))
    x = 0
    for g in seq:
        if g is None:
            x += word_gap
            continue
        img, advance, top = g
        out.alpha_composite(img, (x, ref_top - top))
        x += advance + gap
    return out


def draw_key(size):
    """A golden key with a round bow, drawn large and downscaled."""
    s = 8
    w, hgt = 30 * s, 56 * s
    img = Image.new("RGBA", (w, hgt), (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    outline = (72, 48, 8, 255)
    ow = 2 * s

    def gold(y):
        t = y / hgt
        return (int(255 - 40 * t), int(224 - 80 * t), int(90 - 60 * t), 255)

    # bow (annulus)
    cx, cy, ro, ri = 15 * s, 11 * s, 10 * s, 4.6 * s
    # shaft and teeth as rectangles
    shaft = (cx - 2.6 * s, cy + ro - s, cx + 2.6 * s, hgt - 2 * s)
    teeth = [(shaft[2] - s, hgt - 12 * s, cx + 8 * s, hgt - 9 * s),
             (shaft[2] - s, hgt - 6 * s, cx + 8 * s, hgt - 3 * s)]
    # outline pass
    dr.ellipse((cx - ro - ow, cy - ro - ow, cx + ro + ow, cy + ro + ow), fill=outline)
    dr.rectangle((shaft[0] - ow, shaft[1] - ow, shaft[2] + ow, shaft[3] + ow), fill=outline)
    for t in teeth:
        dr.rectangle((t[0] - ow, t[1] - ow, t[2] + ow, t[3] + ow), fill=outline)
    # gold fill with vertical shading
    fill = Image.new("RGBA", (w, hgt), (0, 0, 0, 0))
    fdr = ImageDraw.Draw(fill)
    fdr.ellipse((cx - ro, cy - ro, cx + ro, cy + ro), fill=(255, 255, 255, 255))
    fdr.rectangle(shaft, fill=(255, 255, 255, 255))
    for t in teeth:
        fdr.rectangle(t, fill=(255, 255, 255, 255))
    px = fill.load()
    for y in range(hgt):
        colr = gold(y)
        for x in range(w):
            if px[x, y][3]:
                px[x, y] = colr
    img.alpha_composite(fill)
    # bow hole
    dr.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), fill=(0, 0, 0, 0))
    dr.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), outline=outline, width=ow)
    # specular highlight
    dr.ellipse((cx - ro + 2.6 * s, cy - ro + 1.4 * s, cx - ro + 7 * s, cy - ro + 4 * s),
               fill=(255, 255, 240, 230))
    img = img.rotate(-38, expand=True, resample=Image.BICUBIC)
    img = img.crop(img.getbbox())
    ratio = min(size / img.width, size / img.height)
    return img.resize((max(1, round(img.width * ratio)),
                       max(1, round(img.height * ratio))), Image.LANCZOS)


def load_icon_art(path):
    """User-supplied key art: uses its alpha, or keys out a white/black bg."""
    src = Image.open(path).convert("RGBA")
    px = src.load()
    alpha_min = min(px[x, y][3] for y in range(0, src.height, 7)
                    for x in range(0, src.width, 7))
    if alpha_min == 255:
        corners = [px[0, 0], px[src.width - 1, 0], px[0, src.height - 1],
                   px[src.width - 1, src.height - 1]]
        bright = sum(c[0] + c[1] + c[2] for c in corners) / 4 > 380
        for y in range(src.height):
            for x in range(src.width):
                r, g, b, _ = px[x, y]
                m = (r + g + b) / 3
                if bright and r > 235 and g > 235 and b > 235:
                    px[x, y] = (r, g, b, 0)
                elif not bright and m < 16:
                    px[x, y] = (r, g, b, 0)
    return src.crop(src.getbbox())


def make_icon(photo, size, key_art=None):
    """Node tile: just the key art on a transparent tile."""
    del photo
    tile = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    if key_art is not None:
        ratio = min(size * 0.98 / key_art.width, size * 0.98 / key_art.height)
        key = key_art.resize((max(1, round(key_art.width * ratio)),
                              max(1, round(key_art.height * ratio))), Image.LANCZOS)
    else:
        key = draw_key(round(size * 0.92))
    tile.alpha_composite(key, ((size - key.width) // 2, (size - key.height) // 2))
    return tile


def paste_art(atlas, scale, name4x, key_art=None):
    if scale == 4:
        name = name4x
    else:
        name = name4x.resize((max(1, name4x.width * scale // 4),
                              max(1, name4x.height * scale // 4)), Image.LANCZOS)
    nx, ny = NAME_POS_UV[0] * scale, NAME_POS_UV[1] * scale
    ix0, iy0, ix1, iy1 = (v * scale for v in ICON_BOX_UV)
    atlas.paste(Image.new("RGBA", (name.width, name.height), (0, 0, 0, 0)), (nx, ny))
    atlas.paste(Image.new("RGBA", (ix1 - ix0, iy1 - iy0), (0, 0, 0, 0)), (ix0, iy0))
    atlas.alpha_composite(name, (nx, ny))
    atlas.alpha_composite(make_icon(None, ix1 - ix0, key_art), (ix0, iy0))
    return name4x.size


# ---- GVM / PRS side -------------------------------------------------------

def gvm_entries(d):
    count = struct.unpack_from(">H", d, 0xA)[0]
    esize = 38
    names = [d[0xC + i * esize + 2:0xC + i * esize + 30].split(b"\0")[0].decode("latin1")
             for i in range(count)]
    offs = []
    i = -1
    while True:
        i = d.find(b"GVRT", i + 1)
        if i < 0:
            break
        fmt = d[i + 11]
        w, h = struct.unpack_from(">HH", d, i + 12)
        offs.append((i, fmt, w, h))
    return list(zip(names, offs))


def enc5a3(r, g, b, a):
    if a >= 0xE0:
        return 0x8000 | ((r * 31 // 255) << 10) | ((g * 31 // 255) << 5) | (b * 31 // 255)
    return ((a * 7 // 255) << 12) | ((r * 15 // 255) << 8) | ((g * 15 // 255) << 4) | (b * 15 // 255)


def patch_gvr_atlas(d, off, before, after):
    d = bytearray(d)
    for ty in range(0, 512, 4):
        for tx in range(0, 512, 4):
            dirty = any(after.getpixel((tx + x, ty + y)) != before.getpixel((tx + x, ty + y))
                        for y in range(4) for x in range(4))
            if not dirty:
                continue
            p = off + ((ty // 4) * 128 + (tx // 4)) * 32
            for y in range(4):
                for x in range(4):
                    struct.pack_into(">H", d, p + (y * 4 + x) * 2,
                                     enc5a3(*after.getpixel((tx + x, ty + y))))
    return bytes(d)


# ---- DDS / pak side -------------------------------------------------------

def dxt5_decode(data, w, h):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    p = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            a0, a1 = data[p], data[p + 1]
            abits = int.from_bytes(data[p + 2:p + 8], "little")
            c0, c1 = struct.unpack_from("<HH", data, p + 8)
            cbits = struct.unpack_from("<I", data, p + 12)[0]
            p += 16
            def c565(v):
                return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63,
                        (v & 31) * 255 // 31)
            ca, cb = c565(c0), c565(c1)
            cols = [ca, cb,
                    tuple((2 * ca[i] + cb[i]) // 3 for i in range(3)),
                    tuple((ca[i] + 2 * cb[i]) // 3 for i in range(3))]
            if a0 > a1:
                alphas = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
            else:
                alphas = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
            for y in range(4):
                for x in range(4):
                    i = y * 4 + x
                    px[bx + x, by + y] = cols[(cbits >> (2 * i)) & 3] \
                        + (alphas[(abits >> (3 * i)) & 7],)
    return img


def dds32_from(img):
    w, h = img.size
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<I", hdr, 4, 124)
    struct.pack_into("<I", hdr, 8, 0x100F)          # CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
    struct.pack_into("<II", hdr, 12, h, w)
    struct.pack_into("<I", hdr, 20, w * 4)          # pitch
    struct.pack_into("<I", hdr, 76, 32)             # pixel format size
    struct.pack_into("<I", hdr, 80, 0x41)           # RGB|ALPHAPIXELS
    struct.pack_into("<I", hdr, 88, 32)
    struct.pack_into("<IIII", hdr, 92, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    struct.pack_into("<I", hdr, 108, 0x1000)        # caps: TEXTURE
    body = bytearray(w * h * 4)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            body[i:i + 4] = bytes((b, g, r, a))
            i += 4
    return bytes(hdr) + bytes(body)


def parse_pak(d):
    p = 0x25
    a = struct.unpack_from("<I", d, p)[0]
    p += 20
    count = struct.unpack_from("<I", d, p)[0]
    p += 4
    metas = []
    for _ in range(count):
        n1 = struct.unpack_from("<I", d, p)[0]; p += 4
        long = d[p:p + n1]; p += n1
        n2 = struct.unpack_from("<I", d, p)[0]; p += 4
        short = d[p:p + n2]; p += n2
        s1, s2 = struct.unpack_from("<II", d, p); p += 8
        metas.append([long, short, s1])
    entries = []
    for meta in metas:
        entries.append((meta[0], meta[1], d[p:p + meta[2]]))
        p += meta[2]
    return a, entries


def write_pak(path, a, entries):
    head = bytearray(0x39)
    head[0:4] = b"\x01pak"
    total = sum(len(b) for _, _, b in entries)
    struct.pack_into("<I", head, 0x25, a)
    struct.pack_into("<II", head, 0x29, total, total)
    head += struct.pack("<I", len(entries))
    body = b""
    for long, short, blob in entries:
        head += struct.pack("<I", len(long)) + long
        head += struct.pack("<I", len(short)) + short
        head += struct.pack("<II", len(blob), len(blob))
        body += blob
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(bytes(head) + body)
    print("wrote", path, len(head) + len(body))


def main():
    key_art = load_icon_art(sys.argv[1]) if len(sys.argv) > 1 else None
    # PRS side (512 GVR atlas)
    d = prs.decompress(open(os.path.join(GD, "stageMap.prs"), "rb").read())
    entries = dict(gvm_entries(d))
    noff, nfmt, nw, nh = entries["stg_stagename"]
    assert (nfmt, nw, nh) == (5, 512, 512)
    atlas = gvr.decode(d, noff + 16, 5, 512, 512)

    # pak side first: the clean 1024 atlas is the glyph source for the name
    a, pak_entries = parse_pak(open(os.path.join(GD, "PRS", "stageMap.pak"), "rb").read())
    name4x = None
    for long, short, blob in pak_entries:
        if short.endswith(b"stg_stagename.dds"):
            hd = struct.unpack_from("<31I", blob, 4)
            assert blob[84:88] == b"DXT5" and (hd[3], hd[2]) == (1024, 1024)
            name4x = compose_name(dxt5_decode(blob[128:], 1024, 1024))
    assert name4x is not None

    before = atlas.copy()
    tw, th = paste_art(atlas, 2, name4x, key_art)
    atlas.save(os.path.join(ROOT, "build", "stagename_512_patched.png"))
    patched = patch_gvr_atlas(d, noff + 16, before, atlas)
    out = prs.compress(patched)
    assert prs.decompress(out) == patched
    os.makedirs(os.path.dirname(OUT_PRS), exist_ok=True)
    open(OUT_PRS, "wb").write(out)
    print("wrote", OUT_PRS, len(out))

    out_entries = []
    for long, short, blob in pak_entries:
        if short.endswith(b"stg_stagename.dds"):
            big = dxt5_decode(blob[128:], 1024, 1024)
            paste_art(big, 4, name4x, key_art)
            big.save(os.path.join(ROOT, "build", "stagename_1024_patched.png"))
            blob = dds32_from(big)
        out_entries.append((long, short, blob))
    write_pak(OUT_PAK, a, out_entries)

    uw = -(-tw // 4)
    uh = -(-th // 4)
    print("texanim name: sz=(%d,%d) uv=(%d,%d,%d,%d) tex=14" % (
        uw * 2, uh * 2, NAME_POS_UV[0], NAME_POS_UV[1],
        NAME_POS_UV[0] + uw, NAME_POS_UV[1] + uh))
    print("texanim icon: sz=(59,59) uv=(%d,%d,%d,%d) tex=14" % ICON_BOX_UV)


if __name__ == "__main__":
    main()
