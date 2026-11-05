"""Adds three gate-box textures (brick, stone, metal) to the stock lobby texture
archives and writes the extended archives for the mod's gd_PC folder.

usage: python build_lobby_textures.py <stock_prs_dir> <out_dir>
  stock_prs_dir holds al_stg_lobby_{000,00k,0dk,h0k,hdk}_tex.prs
The textures are drawn procedurally, so no game art is copied.
"""
import os
import random
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prs  # noqa: E402

VARIANTS = ("000", "00k", "0dk", "h0k", "hdk")
NEW_NAMES = ("gate_brick", "gate_stone", "gate_metal")
SIZE = 128
HEADER_FIXED = 0xC
ENTRY = 38


def noise(img, amount, seed):
    rnd = random.Random(seed)
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b = px[x, y][:3]
            n = rnd.randint(-amount, amount)
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)),
                        max(0, min(255, b + n)))
    return img


def make_brick():
    rnd = random.Random(1)
    img = Image.new("RGB", (SIZE, SIZE), (92, 70, 62))
    d = ImageDraw.Draw(img)
    rows, brick_w = 8, 32
    row_h = SIZE // rows
    for row in range(rows):
        offset = (brick_w // 2) if row % 2 else 0
        for col in range(-1, SIZE // brick_w + 1):
            x0 = col * brick_w + offset
            tone = rnd.randint(-18, 18)
            base = (150 + tone, 70 + tone // 2, 52 + tone // 3)
            d.rectangle([x0 + 1, row * row_h + 1, x0 + brick_w - 2,
                         row * row_h + row_h - 2], fill=base)
    return noise(img, 9, 11)


def make_stone():
    rnd = random.Random(2)
    img = Image.new("RGB", (SIZE, SIZE), (52, 54, 58))
    d = ImageDraw.Draw(img)
    y = 0
    while y < SIZE:
        h = min(rnd.choice((24, 32, 40)), SIZE - y)
        x = 0
        while x < SIZE:
            w = min(rnd.choice((32, 48, 64)), SIZE - x)
            tone = rnd.randint(-14, 14)
            d.rectangle([x + 2, y + 2, x + w - 3, y + h - 3],
                        fill=(128 + tone, 130 + tone, 134 + tone))
            x += w
        y += h
    img = img.filter(ImageFilter.GaussianBlur(0.6))
    return noise(img, 14, 12)


def make_metal():
    rnd = random.Random(3)
    img = Image.new("RGB", (SIZE, SIZE), (120, 128, 138))
    d = ImageDraw.Draw(img)
    for y in range(SIZE):
        shade = rnd.randint(-5, 5)
        d.line([(0, y), (SIZE, y)], fill=(122 + shade, 130 + shade, 142 + shade))
    half = SIZE // 2
    for x0 in (0, half):
        d.rectangle([x0, 0, x0 + 2, SIZE], fill=(60, 66, 74))
        d.rectangle([x0 + 3, 0, x0 + 4, SIZE], fill=(170, 178, 190))
        d.rectangle([0, x0, SIZE, x0 + 2], fill=(60, 66, 74))
        d.rectangle([0, x0 + 3, SIZE, x0 + 4], fill=(170, 178, 190))
    for cx in (12, half - 12, half + 12, SIZE - 12):
        for cy in (12, half - 12, half + 12, SIZE - 12):
            d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=(84, 90, 100))
            d.ellipse([cx - 2, cy - 2, cx, cy], fill=(190, 198, 208))
    return noise(img, 5, 13)


def rgb5a3(r, g, b):
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def encode_level(img):
    w, h = img.size
    pw, ph = max(4, (w + 3) // 4 * 4), max(4, (h + 3) // 4 * 4)
    px = img.load()
    out = bytearray()
    for ty in range(0, ph, 4):
        for tx in range(0, pw, 4):
            for y in range(4):
                for x in range(4):
                    r, g, b = px[min(tx + x, w - 1), min(ty + y, h - 1)][:3]
                    out += struct.pack(">H", rgb5a3(r, g, b))
    return bytes(out)


def build_gvrt(img):
    body = bytearray()
    level = img
    while True:
        body += encode_level(level)
        if level.width == 1 and level.height == 1:
            break
        level = level.resize((max(1, level.width // 2), max(1, level.height // 2)),
                             Image.BOX)
    payload = bytes([0, 0, 0x09, 0x05]) + struct.pack(">HH", img.width, img.height) + body
    return b"GVRT" + struct.pack("<I", len(payload)) + payload


def split_gvm(data):
    count = struct.unpack_from(">H", data, 0xA)[0]
    header_len = struct.unpack_from("<I", data, 4)[0] + 8
    entries = [data[HEADER_FIXED + i * ENTRY:HEADER_FIXED + (i + 1) * ENTRY]
               for i in range(count)]
    blocks = []
    pos = header_len
    while pos < len(data):
        if data[pos:pos + 4] != b"GVRT":
            raise ValueError("unexpected data at 0x%x" % pos)
        size = struct.unpack_from("<I", data, pos + 4)[0]
        blocks.append(data[pos:pos + size + 8])
        pos += size + 8
    if len(blocks) != count:
        raise ValueError("entry/texture count mismatch: %d vs %d" % (len(blocks), count))
    return data[:HEADER_FIXED], entries, blocks


def extend(path_in, path_out, textures):
    data = prs.decompress(open(path_in, "rb").read())
    head, entries, blocks = split_gvm(data)
    count = len(entries)
    template = entries[-1]
    gbix_base = struct.unpack_from(">I", template, ENTRY - 4)[0]
    for i, (name, img) in enumerate(zip(NEW_NAMES, textures)):
        entry = bytearray(template)
        struct.pack_into(">H", entry, 0, count + i)
        entry[2:30] = name.encode().ljust(28, b"\0")
        entry[30] = 0x09
        entry[31] = 0x05
        side = (img.width.bit_length() - 1) - 2
        entry[32] = 0
        entry[33] = (side << 4) | side
        struct.pack_into(">I", entry, ENTRY - 4, gbix_base + 0x100 + i)
        entries.append(bytes(entry))
        blocks.append(build_gvrt(img))
    table = b"".join(entries)
    header = bytearray(head) + table
    header = header.ljust((len(header) + 15) // 16 * 16, b"\0")
    struct.pack_into("<I", header, 4, len(header) - 8)
    struct.pack_into(">H", header, 0xA, len(entries))
    gvm = bytes(header) + b"".join(blocks)
    open(path_out, "wb").write(prs.compress(gvm))
    return count


def main(stock_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    textures = (make_brick(), make_stone(), make_metal())
    for variant in VARIANTS:
        name = "al_stg_lobby_%s_tex.prs" % variant
        count = extend(os.path.join(stock_dir, name), os.path.join(out_dir, name), textures)
        print(variant, "stock textures", count, "-> new ids", count, count + 1, count + 2)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
