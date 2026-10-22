#!/usr/bin/env python3
"""Disassemble a range of the Conker default.xbe by virtual address, using ndisasm."""
import struct, subprocess, sys

XBE = "assets/default.xbe"
d = open(XBE, "rb").read()

def u32(o): return struct.unpack_from("<I", d, o)[0]

base = u32(0x104)
hsz = u32(0x108)
nsec = u32(0x11C)
sec_addr = u32(0x120)
sections = []
for i in range(nsec):
    o = sec_addr - base + i * 56
    flags = u32(o); va = u32(o + 4); vsz = u32(o + 8)
    raw = u32(o + 12); rsz = u32(o + 16); namep = u32(o + 20)
    nm = d[namep - base:namep - base + 16].split(b"\0")[0].decode("latin1")
    sections.append((nm, va, vsz, raw, rsz))

def to_off(va):
    for nm, va0, vsz, raw, rsz in sections:
        if va0 <= va < va0 + vsz:
            off = raw + (va - va0)
            if off < raw + rsz:
                return off, nm
    return None, None

def disasm(va, length=160, count=0):
    off, nm = to_off(va)
    if off is None:
        print(f"{va:#x}: not mapped")
        return
    blob = d[off:off + length]
    args = ["ndisasm", "-b32", "-o", hex(va), "-"]
    out = subprocess.run(args, input=blob, capture_output=True)
    print(f"; {va:#x} in {nm} (file offset {off:#x})")
    print(out.stdout.decode("latin1"))

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: xbe_disasm.py <va> [len] [count]")
        sys.exit(1)
    va = int(sys.argv[1], 0)
    ln = int(sys.argv[2], 0) if len(sys.argv) > 2 else 160
    cnt = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
    disasm(va, ln, cnt)
