#!/usr/bin/env python3
"""List the D3D-section functions the game code (.text) calls directly.

Conker was built with LTCG and links the D3D8 library internals straight into the game, so .text calls many
non-exported D3D functions that xbe_symbols cannot name (and the runtime therefore does not host-replace).
Each one is a candidate for a hook in game/src/game/hooks.cpp.

usage: xbe_d3d_internals.py <default.xbe> <config/game.toml>
"""
import re
import struct
import sys

def main(xbe_path, config_path):
    d = open(xbe_path, "rb").read()

    def u32(o):
        return struct.unpack_from("<I", d, o)[0]

    base = u32(0x104)
    nsec = u32(0x11C)
    sec_addr = u32(0x120)
    secs = {}
    for i in range(nsec):
        o = sec_addr - base + i * 56
        name = d[u32(o + 20) - base:u32(o + 20) + 20 - base].split(b"\0")[0].decode("latin1")
        secs[name] = (u32(o + 4), u32(o + 8), u32(o + 12), u32(o + 16))

    def section_of(va):
        for name, (v, vs, raw, rs) in secs.items():
            if v <= va < v + vs:
                return name
        return None

    sym = set()
    for line in open(config_path):
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*$", line.split("#")[0].strip())
        if m and int(m.group(2), 16):
            sym.add(int(m.group(2), 16))

    tva, tvs, traw, trs = secs[".text"]
    blob = d[traw:traw + trs]
    calls = {}
    i = 0
    while i < len(blob) - 5:
        if blob[i] == 0xE8:
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            tgt = (tva + i + 5 + rel) & 0xFFFFFFFF
            if section_of(tgt) == "D3D":
                calls[tgt] = calls.get(tgt, 0) + 1
            i += 5
        else:
            i += 1

    mapped = sorted(t for t in calls if t in sym)
    unmapped = sorted(t for t in calls if t not in sym)
    print(f"# D3D functions called directly from .text: {len(calls)} ({len(mapped)} mapped, {len(unmapped)} not)")
    print("\n# not in [symbols] (hook candidates), by call count")
    for t in sorted(unmapped, key=lambda t: -calls[t]):
        print(f"{t:#010x}  calls={calls[t]}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2])
