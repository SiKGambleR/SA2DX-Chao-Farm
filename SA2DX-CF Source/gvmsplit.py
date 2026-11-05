import os
import struct
import sys

sys.path.insert(0, r"C:\Users\leg4c\Desktop\SA Test Mod\tools")
import prs

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
d = prs.decompress(open(src, "rb").read())
count = struct.unpack_from(">H", d, 0xA)[0]
names = []
p = 0xC
for i in range(count):
    names.append(d[p + 2:p + 28].split(b"\0")[0].decode("latin1"))
    p += 0x26
pos = []
i = 0
while True:
    i = d.find(b"GVRT", i)
    if i < 0:
        break
    pos.append(i - 16 if d[i - 16:i - 12] == b"GBIX" else i)
    i += 4
pos.append(len(d))
print(count, len(pos) - 1)
with open(os.path.join(out, "index.txt"), "w") as idx:
    for n, (a, b) in zip(names, zip(pos, pos[1:])):
        fn = n + ".gvr"
        open(os.path.join(out, fn), "wb").write(d[a:b])
        idx.write(fn + "\n")
