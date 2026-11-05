import struct, sys, os
from PIL import Image, ImageDraw

def c565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)

def decode(path):
    d = open(path, "rb").read()
    g = d.find(b"GVRT")
    fmt = d[g + 11]
    w, h = struct.unpack(">HH", d[g + 12:g + 16])
    body = d[g + 16:]
    img = Image.new("RGBA", (w, h))
    px = img.load()
    o = 0
    if fmt == 5:
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for y in range(4):
                    for x in range(4):
                        v = struct.unpack(">H", body[o:o + 2])[0]; o += 2
                        if v & 0x8000:
                            p = (((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
                        else:
                            p = (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, 255)
                        px[tx + x, ty + y] = p
        return img
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for sy in range(2):
                for sx in range(2):
                    c0, c1 = struct.unpack(">HH", body[o:o + 4])
                    bits = body[o + 4:o + 8]; o += 8
                    a, b = c565(c0), c565(c1)
                    if c0 > c1:
                        pal = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                               tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
                    else:
                        pal = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                    for y in range(4):
                        for x in range(4):
                            idx = (bits[y] >> (6 - 2 * x)) & 3
                            px[bx + sx * 4 + x, by + sy * 4 + y] = pal[idx]
    return img

d = sys.argv[1]
names = [l.strip() for l in open(os.path.join(d, "index.txt")) if l.strip()]
sheet = Image.new("RGB", (10 * 100, 13 * 118), (60, 60, 60))
dr = ImageDraw.Draw(sheet)
for i, n in enumerate(names):
    im = decode(os.path.join(d, n)).convert("RGB").resize((92, 92))
    x, y = (i % 10) * 100, (i // 10) * 118
    sheet.paste(im, (x + 4, y + 18))
    dr.text((x + 4, y + 3), "%d %s" % (i, n), fill=(255, 255, 255))
sheet.save(sys.argv[2])
