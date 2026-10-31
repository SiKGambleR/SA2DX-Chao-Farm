import struct, sys
from PIL import Image


def rgb5a3(v):
    if v & 0x8000:
        return (((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
    return (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 // 7)


def rgb565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)


def decode(d, off, fmt, w, h):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    if fmt == 5:
        p = off
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for y in range(4):
                    for x in range(4):
                        px[tx + x, ty + y] = rgb5a3(struct.unpack_from(">H", d, p)[0])
                        p += 2
    elif fmt == 0xE:
        p = off
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for by in range(2):
                    for bx in range(2):
                        c0, c1 = struct.unpack_from(">HH", d, p)
                        bits = d[p + 4:p + 8]
                        p += 8
                        a, b = rgb565(c0), rgb565(c1)
                        if c0 > c1:
                            cols = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                                    tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
                        else:
                            cols = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                        for y in range(4):
                            row = bits[y]
                            for x in range(4):
                                px[tx + bx * 4 + x, ty + by * 4 + y] = cols[(row >> (6 - 2 * x)) & 3]
    else:
        raise ValueError(fmt)
    return img


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    i = 0
    n = 0
    while True:
        i = d.find(b"GVRT", i)
        if i < 0:
            break
        fmt = d[i + 11]
        w, h = struct.unpack_from(">HH", d, i + 12)
        try:
            decode(d, i + 16, fmt, w, h).save("%s_%d.png" % (sys.argv[2], n))
        except Exception as e:
            print(n, "fail", e)
        n += 1
        i += 4
