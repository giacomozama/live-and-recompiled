#!/usr/bin/env python3
"""Read the game's CAFF packages (aid/**/default.bin, .rbm): unpack the sections, list the assets, dump one.

  caff.py list FILE                 index, type, data offset/size, gpu offset/size
  caff.py dump FILE INDEX [OUT]     the asset's .data part (pointers resolved to .data offsets), to OUT or hexdump

The decompressor is a port of src/runtime/gpu/disc_programs.cpp (UnpackCaff, the game's 0x54AE40); the table of
contents and relocations follow clr_unpack (github birdytsc/clr_unpack, a copy in clr_unpack/, gitignored).
Import it for scripts: caff.Package(path).assets, .data(asset), .gpu(asset).
"""
import struct
import sys


def _table(lens):
    count = [0] * 16
    for b in lens:
        count[b & 15] += 1
        count[b >> 4] += 1
    if count[0] == 512:
        return [0] * 0x800
    if count[0] == 511:
        return None
    per_length = count[:]
    left = 0
    for l in range(15, 0, -1):
        left += count[l]
        if left & 1:
            return None
        left >>= 1
    if left != 1:
        return None
    for l in range(2, 16):
        count[l] += count[l - 1]
    n = count[15]
    srt = [0] * 512
    for sym in range(511, -1, -1):
        ln = lens[sym >> 1] >> 4 if sym & 1 else lens[sym >> 1] & 15
        if ln:
            count[ln] -= 1
            srt[count[ln]] = sym << 4 | ln
    table = [0] * 0x800
    cur = top = 0x800
    for l in range(15, 10, -1):
        mark = cur
        while top > mark:
            top -= 2
            cur -= 1
            table[cur] = -0x8000 | top  # negative: a node
        for _ in range(per_length[l]):
            cur -= 1
            n -= 1
            table[cur] = srt[n]
        top = mark
    p = 0x400
    while top > cur:
        top -= 2
        p -= 1
        table[p] = -0x8000 | top
    while n > 0:
        n -= 1
        e = srt[n]
        for _ in range(0x400 >> (e & 15)):
            p -= 1
            table[p] = e
    return table


def _chunk(inp, out_size):
    if out_size <= 0x105 or out_size > 0x10000 or len(inp) < 0x105 or out_size < len(inp):
        return bytes(out_size)
    table = _table(inp[:0x100])
    if table is None:
        return bytes(out_size)
    rd16 = lambda o: inp[o] | inp[o + 1] << 8
    p, end = 0x104, len(inp)
    bits = (rd16(0x100) << 16 | rd16(0x102)) & 0xFFFFFFFF
    spare = 16
    out = bytearray()
    while True:
        e = table[bits >> 22]
        if e >= 0:
            bits = (bits << (e & 15)) & 0xFFFFFFFF
        else:
            bits = (bits << 10) & 0xFFFFFFFF
            while e < 0:
                e = table[(e & 0x7FFF) + (bits >> 31)]
                bits = (bits << 1) & 0xFFFFFFFF
        spare -= e & 15
        sym = e >> 4
        if spare < 0:
            if p + 2 > end:
                break
            bits = (bits + (rd16(p) << -spare)) & 0xFFFFFFFF
            p += 2
            spare += 16
        if len(out) >= out_size:
            break
        if sym < 0x100:
            out.append(sym)
            continue
        m = sym - 0x100
        nb, lc = m >> 4, m & 15
        dist = (1 << nb) | ((bits >> (32 - nb)) if nb else 0)
        bits = (bits << nb) & 0xFFFFFFFF
        spare -= nb
        n = lc
        if lc == 15:
            n = inp[p] + 15
            p += 1
            if n == 0xFF + 15:
                n = rd16(p)
                p += 2
        n += 3
        if dist > len(out) or n > out_size - len(out):
            break
        for _ in range(n):
            out.append(out[-dist])
        if spare < 0:
            if p + 2 > end:
                break
            bits = (bits + (rd16(p) << -spare)) & 0xFFFFFFFF
            p += 2
            spare += 16
    out += bytes(out_size - len(out))
    return bytes(out[:out_size])


class Asset:
    def __init__(self, index):
        self.index = index
        self.type = ""
        self.data_off = self.data_size = None
        self.gpu_off = self.gpu_size = None


class Package:
    def __init__(self, path):
        f = open(path, "rb").read()
        if f[:4] != b"CAFF":
            raise ValueError(f"{path}: not a CAFF file")
        u32 = lambda o: struct.unpack_from("<I", f, o)[0]
        num_files, num_with_gpu, t1, t2, num_relocs = (u32(0x18 + 4 * i) for i in range(5))
        toc, data_at = u32(0x34), u32(0x38)
        sections, compressed = f[0x3D], f[0x3E] != 0
        self.sections = []
        src = data_at
        for i in range(sections):
            info = 0x40 + i * 0x28
            size = u32(info + 0x10)
            stored = u32(info + 0x24) if compressed else size
            if not compressed:
                sec = f[src:src + size]
            else:
                parts, s, s_end = [], src, src + stored
                while s + 8 <= s_end:
                    osz, isz = struct.unpack_from("<II", f, s)
                    s += 8
                    parts.append(f[s:s + osz] if osz == isz else _chunk(f[s:s + isz], osz))
                    s += isz
                sec = b"".join(parts)
            self.sections.append(bytearray(sec))
            src += stored
        d = self.sections[0]
        r32 = lambda o: struct.unpack_from("<I", d, o)[0]
        names = r32(toc)
        p = toc + 4 + (4 * num_files if names else 0) + names
        self.assets = {}
        for i in range(num_with_gpu):
            idx, off, size, part, _hdr = struct.unpack_from("<IIIBB", d, p + 14 * i)
            a = self.assets.setdefault(idx, Asset(idx))
            if part == 1:
                a.data_off, a.data_size = off, size
            elif part == 2:
                a.gpu_off, a.gpu_size = off, size
        p += 14 * num_with_gpu + 12 * t1 + 4 * t2
        relocs = [struct.unpack_from("<III", d, p + 12 * i) for i in range(num_relocs)]
        q = p + 12 * num_relocs
        # pointers inside .data parts: offsets into the section, the target asset's (data part) offset added
        for srcidx, dstidx, cnt in relocs:
            s, t = self.assets.get(srcidx), self.assets.get(dstidx)
            for j in range(cnt):
                o = r32(q + 4 * j)
                if s and t and s.data_off is not None and t.data_off is not None and o < s.data_size:
                    at = s.data_off + o
                    struct.pack_into("<I", d, at, (r32(at) + t.data_off) & 0xFFFFFFFF)
            q += 4 * cnt
        for a in self.assets.values():
            if a.data_off is not None:
                raw = bytes(d[a.data_off:a.data_off + 32])
                a.type = raw.split(b"\0")[0].decode("latin1", "replace")

    def data(self, a):
        return bytes(self.sections[0][a.data_off:a.data_off + a.data_size])

    def gpu(self, a):
        return bytes(self.sections[1][a.gpu_off:a.gpu_off + a.gpu_size]) if a.gpu_off is not None else b""


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pkg = Package(sys.argv[2])
    if sys.argv[1] == "list":
        for i, a in sorted(pkg.assets.items()):
            print(f"{i:5} {a.type:16} data {a.data_off}+{a.data_size} gpu {a.gpu_off}+{a.gpu_size}")
    elif sys.argv[1] == "dump":
        a = pkg.assets[int(sys.argv[3])]
        b = pkg.data(a)
        if len(sys.argv) > 4:
            open(sys.argv[4], "wb").write(b)
        else:
            for o in range(0, len(b), 16):
                row = b[o:o + 16]
                print(f"{o:08x}: {row.hex(' '):48} {''.join(chr(c) if 32 <= c < 127 else '.' for c in row)}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
