import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"G:\SteamLibrary\steamapps\common\Sonic Adventure 2\sonic2app.exe"
pe = pefile.PE(EXE, fast_load=True)
data = open(EXE, "rb").read()
md = Cs(CS_ARCH_X86, CS_MODE_32)


DUMP = open(__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'sa2_text_dump.bin'), 'rb').read()


def read(va, n):
    return DUMP[va - 0x401000:va - 0x401000 + n]


def dis(va, n):
    for i in md.disasm(read(va, n), va):
        print("%08X  %-6s %s" % (i.address, i.mnemonic, i.op_str))


if __name__ == "__main__":
    dis(int(sys.argv[1], 16), int(sys.argv[2], 0))

