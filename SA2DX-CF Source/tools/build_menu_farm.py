"""Replaces the main-menu DOWNLOAD CONTENT button label with "CHAO FARM".

The port's extra menu labels (LEADERBOARDS..DOWNLOAD CONTENT) live in the
mainmenu_button_all_ex atlas (texture 11 of newMenu, 1024x1024 DXT5) inside
PRS\newMenu.pak. Each label occupies one row; the left half of the atlas holds
the white 3D-outlined (selected) style and the right half the flat grey
(unselected) style. DOWNLOAD CONTENT is the last English row (frames 83/93) and
the last Japanese row (frames 103/113).

"CHAO FARM" is composed from the menu's own glyphs so the font matches exactly:
C and O and R from CONTROLS, H from HELP & OPTIONS, A and M from EXIT GAME,
F from UNLOCK FULL GAME. Both styles are rebuilt from their own column.

Outputs mod\gd_PC\PRS\newMenu.pak and a preview.

usage: python build_menu_farm.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image

from build_stagemap_farm import parse_pak, dxt5_decode, dds32_from, write_pak, face_runs

GD = r"G:\SteamLibrary\steamapps\common\Sonic Adventure 2\resource\gd_PC"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PAK = os.path.join(ROOT, "mod", "gd_PC", "PRS", "newMenu.pak")

# English label rows in the _ex atlas, as (x0, y0, x1, y1) pixels for the
# white-3D (left) column. The grey column is the same rows shifted +512 in x.
EN_ROWS = {
    "HELP":      (0,  80, 512, 120),  # HELP & OPTIONS   (frame 76/86)
    "CONTROLS":  (0, 160, 512, 200),  # CONTROLS         (frame 78/88)
    "CREDITS":   (0, 240, 512, 280),  # CREDITS          (frame 80/90)
    "EXITGAME":  (0, 280, 512, 320),  # EXIT GAME        (frame 81/91)
    "FULLGAME":  (0, 320, 512, 360),  # UNLOCK FULL GAME (frame 82/92)
}
EN_TARGET = (0, 360, 512, 400)        # DOWNLOAD CONTENT (frame 83/93)
JP_TARGET = (0, 940, 512, 1000)       # JP row           (frame 103/113)


def col_runs(strip, grey):
    """Column runs containing letter ink. White style keys on the bright face;
    grey style keys on any opaque pixel (it has no dark outline)."""
    w, h = strip.size
    px = strip.load()
    cols = []
    for x in range(w):
        hit = False
        for y in range(h):
            r, g, b, a = px[x, y]
            if grey:
                if a > 140:
                    hit = True
                    break
            elif a > 180 and min(r, g, b) > 170:
                hit = True
                break
        cols.append(hit)
    runs = []
    x = 0
    while x < w:
        if not cols[x]:
            x += 1
            continue
        s = x
        while x < w and cols[x]:
            x += 1
        runs.append((s, x - 1))
    return runs


def row_glyphs(atlas, box, x_shift, grey):
    x0, y0, x1, y1 = box
    strip = atlas.crop((x0 + x_shift, y0, x1 + x_shift, y1))
    return strip, col_runs(strip, grey)


def pick(strip, runs, index, tail=6):
    s, e = runs[index]
    limit = runs[index + 1][0] - 1 if index + 1 < len(runs) else strip.width
    img = strip.crop((s, 0, min(e + 1 + tail, limit), strip.height))
    return img, e - s + 1


def face_top_bottom(img, grey):
    px = img.load()
    if grey:
        rows = [y for y in range(img.height) for x in range(img.width)
                if px[x, y][3] > 140]
    else:
        rows = [y for y in range(img.height) for x in range(img.width)
                if px[x, y][3] > 180 and min(px[x, y][:3]) > 170]
        if not rows:
            rows = [y for y in range(img.height) for x in range(img.width)
                    if px[x, y][3] > 180]
    return (min(rows), max(rows)) if rows else (0, img.height - 1)


def compose(atlas, x_shift):
    grey = x_shift != 0
    rows = {name: row_glyphs(atlas, box, x_shift, grey) for name, box in EN_ROWS.items()}
    # letter -> (row, index)
    spec = [("CONTROLS", 0), ("HELP", 0), ("EXITGAME", 5), ("CONTROLS", 1), None,
            ("FULLGAME", 6), ("EXITGAME", 5), ("CONTROLS", 4), ("EXITGAME", 6)]
    # Validate the words segmented into the expected letter counts.
    expect = {"HELP": 12, "CONTROLS": 8, "CREDITS": 7, "EXITGAME": 8, "FULLGAME": 14}
    for name, (strip, runs) in rows.items():
        if len(runs) != expect[name]:
            raise RuntimeError("%s segmented into %d glyphs (expected %d)"
                               % (name, len(runs), expect[name]))
    # reference cap height from CONTROLS 'C'
    cstrip, cruns = rows["CONTROLS"]
    ref_img, _ = pick(cstrip, cruns, 0)
    rt, rb = face_top_bottom(ref_img, grey)
    ref_h = rb - rt + 1

    glyphs = []
    for item in spec:
        if item is None:
            glyphs.append(None)
            continue
        name, idx = item
        strip, runs = rows[name]
        img, advance = pick(strip, runs, idx, tail=0 if grey else 6)
        if grey:
            # grey has no 3D extrusion; keep only the letter's own pixels so no
            # anti-aliasing sliver from the next glyph survives
            bbox = img.getbbox()
            if bbox:
                img = img.crop((0, bbox[1], bbox[2], bbox[3]))
                advance = bbox[2]
        top, bottom = face_top_bottom(img, grey)
        glyphs.append((img, advance, top))

    letter_gap = cruns[1][0] - cruns[0][1] - 1
    word_gap = int(ref_h * 0.55)
    tail = 6
    width = sum(word_gap if g is None else g[1] + letter_gap for g in glyphs) - letter_gap + tail
    out = Image.new("RGBA", (max(1, width), atlas.crop(EN_ROWS["CONTROLS"]).height),
                    (0, 0, 0, 0))
    x = 0
    for g in glyphs:
        if g is None:
            x += word_gap
            continue
        img, advance, top = g
        out.alpha_composite(img, (x, rt - top))
        x += advance + letter_gap
    return out.crop(out.getbbox())


def place(atlas, text, box, x_shift):
    x0, y0, x1, y1 = box
    rw, rh = x1 - x0, y1 - y0
    scale = min((rw - 8) / text.width, (rh - 6) / text.height, 1.0)
    img = text if scale >= 0.999 else text.resize(
        (max(1, round(text.width * scale)), max(1, round(text.height * scale))),
        Image.LANCZOS)
    atlas.paste(Image.new("RGBA", (rw, rh), (0, 0, 0, 0)), (x0 + x_shift, y0))
    ox = x0 + x_shift + (rw - img.width) // 2
    oy = y0 + (rh - img.height) // 2
    atlas.alpha_composite(img, (ox, oy))


def main():
    a, entries = parse_pak(open(os.path.join(GD, "PRS", "newMenu.pak"), "rb").read())
    out_entries = []
    for long, short, blob in entries:
        if short.endswith(b"mainmenu_button_all_ex.dds"):
            atlas = dxt5_decode(blob[128:], 1024, 1024)
            white = compose(atlas, 0)      # left column style
            grey = compose(atlas, 512)     # right column style
            for x_shift, art in ((0, white), (512, grey)):
                place(atlas, art, EN_TARGET, x_shift)
                place(atlas, art, JP_TARGET, x_shift)
            atlas.save(os.path.join(ROOT, "build", "menu_ex_patched.png"))
            blob = dds32_from(atlas)
        out_entries.append((long, short, blob))
    write_pak(OUT_PAK, a, out_entries)


if __name__ == "__main__":
    main()
