#!/usr/bin/env python3
"""List the direct calls and jumps (E8/E9 rel32) in .text to the given addresses: xbe_callers.py <address>..."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from xbe_disasm import d, sections

targets = {int(a, 0) for a in sys.argv[1:]}
for nm, va0, vsz, raw, rsz in sections:
    if nm != ".text":
        continue
    blob = d[raw:raw + rsz]
    for i in range(len(blob) - 5):
        if blob[i] in (0xE8, 0xE9):
            t = va0 + i + 5 + struct.unpack_from("<i", blob, i + 1)[0]
            if t in targets:
                print(f"{t:#x}\t{va0 + i:#x}\t{'call' if blob[i] == 0xE8 else 'jmp'}")
