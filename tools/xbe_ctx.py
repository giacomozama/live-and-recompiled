#!/usr/bin/env python3
"""Disassemble around one instruction of default.xbe: xbe_ctx.py <address> [before] [after] (capstone).
Resyncs on the address, so the lines before it decode as the code that reaches it."""
import sys
import capstone
sys.path.insert(0, __import__("os").path.dirname(__file__))
from xbe_disasm import d, to_off

site = int(sys.argv[1], 0)
before = int(sys.argv[2]) if len(sys.argv) > 2 else 25
after = int(sys.argv[3]) if len(sys.argv) > 3 else 5
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for skew in range(16):
    start = site - before * 6 + skew
    off, _ = to_off(start)
    ins = list(md.disasm(d[off:off + before * 6 + after * 8], start))
    at = [i for i, x in enumerate(ins) if x.address == site]
    if at:
        break
i = at[0] if at else before
for x in ins[max(0, i - before):i + after + 1]:
    print(f"{x.address:08X}{'>' if x.address == site else ' '} {x.mnemonic} {x.op_str}")
