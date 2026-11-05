import struct, sys
from PIL import Image, ImageDraw, ImageFont
import prs, gvr

SRC = r"G:\SteamLibrary\steamapps\common\Sonic Adventure 2\resource\gd_PC\newMenu.prs"
ATLAS_OFF_MARK = 9  # index of the 512x512 label atlas inside the GVM
Y0 = 288  # blank band (v 144..168) in the label atlas
CELL_H = 48
COLS = (0, 100, 200, 300)  # bright English, dim English, bright JP, dim JP


def find_gvrt(d, n):
    i = -1
    for _ in range(n + 1):
        i = d.find(b"GVRT", i + 1)
    return i


def enc5a3(r, g, b, a):
    if a >= 0xE0:
        return 0x8000 | ((r * 31 // 255) << 10) | ((g * 31 // 255) << 5) | (b * 31 // 255)
    return ((a * 7 // 255) << 12) | ((r * 15 // 255) << 8) | ((g * 15 // 255) << 4) | (b * 15 // 255)


def bbox_rows(im, x0, x1, y0, y1):
    rows = [y for y in range(y0, y1) if any(im.getpixel((x, y))[3] > 0 for x in range(x0, x1))]
    return rows


def build(atlas):
    bright_line1 = atlas.crop((0, 240, 100, 264))
    dim_line1 = atlas.crop((100, 240, 200, 264))
    for col in COLS:
        bright = (col // 100) % 2 == 0
        src = bright_line1 if bright else dim_line1
        atlas.paste(Image.new("RGBA", (100, CELL_H), (0, 0, 0, 0)), (col, Y0))
        atlas.alpha_composite(src, (col, Y0 + 3))
        # WORLD line: render with a heavy wide font, then add the same outline/fill treatment
        font = ImageFont.truetype(r"C:\Windows\Fonts\ariblk.ttf", 60)
        big = Image.new("L", (400, 100), 0)
        ImageDraw.Draw(big).text((10, 10), "WORLD", font=font, fill=255)
        big = big.crop(big.getbbox()).resize((84, 14), Image.LANCZOS)
        txt = Image.new("L", (100, 24), 0)
        txt.paste(big, (10, 5))
        layer = Image.new("RGBA", (100, 24), (0, 0, 0, 0))
        if bright:
            shadow = Image.new("RGBA", (100, 24), (0, 0, 0, 0))
            shadow.paste((0, 0, 0, 218), (1, 1), txt)
            layer.alpha_composite(shadow)
            layer.paste((245, 245, 250, 255), (0, 0), txt)
        else:
            layer.paste((118, 118, 118, 255), (0, 0), txt)
        atlas.alpha_composite(layer, (col, Y0 + 24))
    return atlas


def main(out_prs_path):
    d = bytearray(prs.decompress(open(SRC, "rb").read()))
    gv = find_gvrt(d, ATLAS_OFF_MARK)
    assert struct.unpack_from(">HH", d, gv + 12) == (512, 512) and d[gv + 11] == 5
    off = gv + 16
    atlas = gvr.decode(d, off, 5, 512, 512)
    orig = atlas.copy()
    atlas = build(atlas)
    # patch only the 4x4 tiles that changed
    w = 512
    for ty in range(0, 512, 4):
        for tx in range(0, 512, 4):
            changed = any(atlas.getpixel((tx + x, ty + y)) != orig.getpixel((tx + x, ty + y))
                          for y in range(4) for x in range(4))
            if not changed:
                continue
            p = off + ((ty // 4) * (w // 4) + (tx // 4)) * 32
            for y in range(4):
                for x in range(4):
                    struct.pack_into(">H", d, p + (y * 4 + x) * 2, enc5a3(*atlas.getpixel((tx + x, ty + y))))
    atlas.save("atlas_new.png")
    out = prs.compress(bytes(d))
    assert prs.decompress(out) == bytes(d)
    open(out_prs_path, "wb").write(out)
    print("wrote", out_prs_path, len(out))


if __name__ == "__main__":
    main(sys.argv[1])
