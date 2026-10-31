import sys


def decompress(src):
    out = bytearray()
    pos = 0
    ctl = 0
    bits = 0

    def getbit():
        nonlocal ctl, bits, pos
        if bits == 0:
            ctl = src[pos]
            pos += 1
            bits = 8
        b = ctl & 1
        ctl >>= 1
        bits -= 1
        return b

    while True:
        if getbit():
            out.append(src[pos])
            pos += 1
            continue
        if getbit():
            lo = src[pos]
            hi = src[pos + 1]
            pos += 2
            if lo == 0 and hi == 0:
                break
            size = lo & 7
            off = ((hi << 8) | lo) >> 3 | -0x2000
            if size == 0:
                size = src[pos] + 1
                pos += 1
            else:
                size += 2
        else:
            size = (getbit() << 1) | getbit()
            size += 2
            off = src[pos] | -0x100
            pos += 1
        for _ in range(size):
            out.append(out[len(out) + off])
    return bytes(out)


def compress(data):
    # Literal-only PRS stream (valid, no size savings).
    out = bytearray()
    ctl_pos = None
    nbits = 0

    def bit(b):
        nonlocal ctl_pos, nbits
        if ctl_pos is None or nbits == 8:
            ctl_pos = len(out)
            out.append(0)
            nbits = 0
        out[ctl_pos] |= b << nbits
        nbits += 1

    for byte in data:
        bit(1)
        out.append(byte)
    bit(0)
    bit(1)
    out += b"\x00\x00"
    return bytes(out)

if __name__ == "__main__":
    d = decompress(open(sys.argv[1], "rb").read())
    open(sys.argv[2], "wb").write(d)
    print(len(d))

