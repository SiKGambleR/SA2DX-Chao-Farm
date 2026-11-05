"""Builds the Chao Farm loading-screen title card from the game's own Chao World card.

Keeps the "Chao" line and the arrow badge of stg_title90, replaces "World" with
"Farm" rendered in a rounded bold font, recolored with the original card's own
per-row gradient and given the same black outline and gray drop shadow.

Outputs:
  mod\gd_PC\stg_titlefarm.prs        (GVM, GVR fmt5, texture renamed)
  mod\gd_PC\PRS\stg_titlefarm.pak    (DDS32 + inf, cloned from stg_title90.pak)
  build\stg_titlefarm_preview.png

usage: python build_farm_title.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFont, ImageFilter

import gvr
import prs

GD = r"G:\SteamLibrary\steamapps\common\Sonic Adventure 2\resource\gd_PC"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_PRS = os.path.join(GD, "stg_title90.prs")
SRC_PAK = os.path.join(GD, "PRS", "stg_title90.pak")
OUT_PRS = os.path.join(ROOT, "mod", "gd_PC", "stg_titlefarm.prs")
OUT_PAK = os.path.join(ROOT, "mod", "gd_PC", "PRS", "stg_titlefarm.pak")
PREVIEW = os.path.join(ROOT, "build", "stg_titlefarm_preview.png")

WORLD_TOP, WORLD_BOTTOM = 46, 90  # rows cleared and redrawn with "Farm"
TEXT_TOP = 49                     # original "World" started here
SHADOW = (99, 99, 99, 255)
OUTLINE = 2
SHADOW_OFFSET = (3, 3)


def gradient_rows(img):
    """Per-row fill color sampled from the original 'World' letters."""
    colors = {}
    for y in range(49, 86):
        best = None
        for x in range(1, 188):
            r, g, b, a = img.getpixel((x, y))
            if a > 200 and max(r, g, b) > 150 and abs(r - g) + abs(g - b) + abs(r - b) > 20:
                lum = r + g + b
                if best is None or lum > best[0]:
                    best = (lum, (r, g, b))
        if best:
            colors[y] = best[1]
    ys = sorted(colors)
    full = {}
    for y in range(0, 128):
        src = min(ys, key=lambda v: abs(v - y))
        full[y] = colors[src]
    return full


def render_farm(img):
    """Default card: reuse the 'Chao' line and restyle 'Farm' from the gradient."""
    grad = gradient_rows(img)
    font = None
    for name, size in (("comicbd.ttf", 76), ("ariblk.ttf", 72)):
        try:
            font = ImageFont.truetype(os.path.join(r"C:\Windows\Fonts", name), size)
            break
        except OSError:
            continue
    big = Image.new("L", (600, 160), 0)
    ImageDraw.Draw(big).text((20, 10), "Farm", font=font, fill=255)
    box = big.getbbox()
    big = big.crop(box)
    target_h = 37
    target_w = round(big.width * target_h / big.height)
    mask = big.resize((target_w, target_h), Image.LANCZOS).point(lambda p: 255 if p > 96 else 0)

    for y in range(WORLD_TOP, WORLD_BOTTOM):
        for x in range(256):
            img.putpixel((x, y), (0, 0, 0, 0))

    ox, oy = 4, TEXT_TOP
    layer = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    # drop shadow
    sh = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    sh.paste(SHADOW, (ox + SHADOW_OFFSET[0], oy + SHADOW_OFFSET[1]), mask)
    layer.alpha_composite(sh)
    # black outline by dilation
    fat = Image.new("L", (256, 128), 0)
    fat.paste(mask, (ox, oy))
    fat = fat.filter(ImageFilter.MaxFilter(OUTLINE * 2 + 1))
    outline = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    outline.paste((16, 16, 16, 255), (0, 0), fat)
    layer.alpha_composite(outline)
    # gradient fill
    fill = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    for y in range(mask.height):
        row = grad[49 + round(y * 36 / max(1, mask.height - 1))]
        for x in range(mask.width):
            if mask.getpixel((x, y)):
                fill.putpixel((ox + x, oy + y), row + (255,))
    layer.alpha_composite(fill)
    img.alpha_composite(layer)
    return img


def render_from_logo(img, logo_path):
    """Builds the card from user-supplied logo art: keys out the black
    background, fits the logo into the text area, keeps the arrow badge."""
    src = Image.open(logo_path).convert("RGBA")
    w, h = src.size
    px = src.load()
    x0, y0, x1, y1 = w, h, 0, 0
    for y in range(h):
        for x in range(int(w * 0.25), w):
            r, g, b, _ = px[x, y]
            if max(r, g, b) > 24:
                x0, y0 = min(x0, x), min(y0, y)
                x1, y1 = max(x1, x), max(y1, y)
    logo = src.crop((x0, y0, x1 + 1, y1 + 1))
    lp = logo.load()
    for y in range(logo.height):
        for x in range(logo.width):
            r, g, b, _ = lp[x, y]
            a = max(0, min(255, (max(r, g, b) - 8) * 3))
            lp[x, y] = (r, g, b, a)
    # text area is x 0..185, y 0..92 (the arrow badge sits at 187+, 95+)
    box_w, box_h = 184, 92
    ratio = min(box_w / logo.width, box_h / logo.height)
    logo = logo.resize((round(logo.width * ratio), round(logo.height * ratio)),
                       Image.LANCZOS)
    for y in range(0, 93):
        for x in range(256):
            img.putpixel((x, y), (0, 0, 0, 0))
    for y in range(93, 128):
        for x in range(0, 186):
            img.putpixel((x, y), (0, 0, 0, 0))
    img.alpha_composite(logo, (2, max(0, (93 - logo.height) // 2)))
    return img


def enc5a3(r, g, b, a):
    if a >= 0xE0:
        return 0x8000 | ((r * 31 // 255) << 10) | ((g * 31 // 255) << 5) | (b * 31 // 255)
    return ((a * 7 // 255) << 12) | ((r * 15 // 255) << 8) | ((g * 15 // 255) << 4) | (b * 15 // 255)


def write_prs(img, d):
    d = bytearray(d)
    name = b"stg_titlefarm" + b"\x00" * 15
    assert d[0xE:0xE + 28].startswith(b"stg_title90"), d[0xE:0xE + 28]
    d[0xE:0xE + 28] = name
    off = 0x50 + 16
    for ty in range(0, 128, 4):
        for tx in range(0, 256, 4):
            p = off + ((ty // 4) * 64 + (tx // 4)) * 32
            for y in range(4):
                for x in range(4):
                    struct.pack_into(">H", d, p + (y * 4 + x) * 2,
                                     enc5a3(*img.getpixel((tx + x, ty + y))))
    out = prs.compress(bytes(d))
    assert prs.decompress(out) == bytes(d)
    os.makedirs(os.path.dirname(OUT_PRS), exist_ok=True)
    open(OUT_PRS, "wb").write(out)
    print("wrote", OUT_PRS, len(out))


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
    blobs = []
    for meta in metas:
        blobs.append(d[p:p + meta[2]]); p += meta[2]
    return a, list(zip(metas, blobs))


def write_pak(img, d):
    a, entries = parse_pak(d)
    out_entries = []
    for (long, short, _), blob in entries:
        long = long.replace(b"stg_title90", b"stg_titlefarm")
        short = short.replace(b"stg_title90", b"stg_titlefarm")
        if short.endswith(b".dds"):
            header = bytearray(blob[:128])
            px = bytearray(blob[128:])
            assert struct.unpack_from("<II", header, 12) == (128, 256)
            i = 0
            for y in range(128):
                for x in range(256):
                    r, g, b_, al = img.getpixel((x, y))
                    px[i:i + 4] = bytes((b_, g, r, al))
                    i += 4
            blob = bytes(header) + bytes(px)
        elif short.endswith(b".inf"):
            inf = bytearray(blob)
            inf[0:28] = b"stg_titlefarm" + b"\x00" * 15
            blob = bytes(inf)
        out_entries.append((long, short, blob))
    body = b""
    total = sum(len(b) for _, _, b in out_entries)
    head = bytearray(0x39)
    head[0:4] = b"\x01pak"
    struct.pack_into("<I", head, 0x25, a)
    struct.pack_into("<II", head, 0x29, total, total)
    # the two dwords at 0x31 stay 0
    head += struct.pack("<I", len(out_entries))
    for long, short, blob in out_entries:
        head += struct.pack("<I", len(long)) + long
        head += struct.pack("<I", len(short)) + short
        head += struct.pack("<II", len(blob), len(blob))
        body += blob
    os.makedirs(os.path.dirname(OUT_PAK), exist_ok=True)
    open(OUT_PAK, "wb").write(bytes(head) + body)
    print("wrote", OUT_PAK, len(head) + len(body))


def main():
    d = prs.decompress(open(SRC_PRS, "rb").read())
    img = gvr.decode(d, 0x50 + 16, 5, 256, 128)
    if len(sys.argv) > 1:
        img = render_from_logo(img, sys.argv[1])
    else:
        img = render_farm(img)
    img.save(PREVIEW)
    print("preview", PREVIEW)
    write_prs(img, d)
    write_pak(img, open(SRC_PAK, "rb").read())


if __name__ == "__main__":
    main()
