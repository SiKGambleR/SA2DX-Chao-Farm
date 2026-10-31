import struct, sys


def parse_gvm(d):
    # returns list of (name, gbixStart, gvrtStart, fmt, w, h, dataOffset, entryEnd)
    hdr = struct.unpack_from("<I", d, 4)[0]
    n = 0
    p = 8 + 0
    ents = []
    # entry table starts at 0x0C after flags/count
    count = struct.unpack_from(">H", d, 0xA)[0]
    p = 0xC
    for i in range(count):
        idx = struct.unpack_from(">H", d, p)[0]
        name = d[p + 2:p + 28].split(b"\0")[0].decode("latin1")
        ents.append((idx, name))
        p += 0x2 + 0x1C
    return count, ents


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    print(parse_gvm(d))
    i = 0
    while True:
        i = d.find(b"GVRT", i)
        if i < 0:
            break
        size = struct.unpack_from("<I", d, i + 4)[0]
        pf, df = d[i + 9], d[i + 10] if False else (d[i + 8], d[i + 9])
        w, h = struct.unpack_from(">HH", d, i + 12)
        print(hex(i), "size", size, "bytes8-11", d[i + 8:i + 12].hex(), "w,h", w, h)
        i += 4
