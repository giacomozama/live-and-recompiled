#!/usr/bin/env python3
"""Disassemble NV2A vertex programs dumped by XBE_DUMP_PROGRAM (CHIPPROG blocks of 4 hex words a line).
usage: vsh_disasm.py run.out [hash-prefix]"""
import sys

MAC = ['nop', 'mov', 'mul', 'add', 'mad', 'dp3', 'dph', 'dp4', 'dst', 'min', 'max', 'slt', 'sge', 'arl']
ILU = ['nop', 'mov', 'rcp', 'rcc', 'rsq', 'exp', 'log', 'lit']
OUT = ['oPos', '?1', '?2', 'oD0', 'oD1', 'oFog', 'oPts', 'oB0', 'oB1', 'oT0', 'oT1', 'oT2', 'oT3', '?13', '?14', '?15']
MASK = ['', '.w', '.z', '.zw', '.y', '.yw', '.yz', '.yzw', '.x', '.xw', '.xz', '.xzw', '.xy', '.xyw', '.xyz', '']


def f(t, sub, bit, n):
    return (t[sub] >> bit) & ((1 << n) - 1)


def src(t, which):
    if which == 0:
        neg, sw, reg, mux = f(t, 1, 8, 1), [f(t, 1, 6, 2), f(t, 1, 4, 2), f(t, 1, 2, 2), f(t, 1, 0, 2)], f(t, 2, 28, 4), f(t, 2, 26, 2)
    elif which == 1:
        neg, sw, reg, mux = f(t, 2, 25, 1), [f(t, 2, 23, 2), f(t, 2, 21, 2), f(t, 2, 19, 2), f(t, 2, 17, 2)], f(t, 2, 13, 4), f(t, 2, 11, 2)
    else:
        neg, sw = f(t, 2, 10, 1), [f(t, 2, 8, 2), f(t, 2, 6, 2), f(t, 2, 4, 2), f(t, 2, 2, 2)]
        reg, mux = f(t, 2, 0, 2) << 2 | f(t, 3, 30, 2), f(t, 3, 28, 2)
    if mux == 1:
        base = 'R%d' % reg
    elif mux == 2:
        base = 'v%d' % f(t, 1, 9, 4)
    elif mux == 3:
        c = f(t, 1, 13, 8)
        base = 'c[a0+%d]' % (c - 96) if f(t, 3, 1, 1) else 'c%d' % (c - 96)
    else:
        base = '?mux0'
    s = ''.join('xyzw'[i] for i in sw)
    return ('-' if neg else '') + base + ('' if s == 'xyzw' else '.' + s)


def dis(t):
    mac, ilu = f(t, 1, 21, 4), f(t, 1, 25, 3)
    r = f(t, 3, 20, 4)
    out = []
    omask, orb, oaddr, omux = f(t, 3, 12, 4), f(t, 3, 11, 1), f(t, 3, 3, 8), f(t, 3, 2, 1)
    def dests(m, regmask, isilu):
        d = []
        if regmask:
            rr = 1 if (isilu and mac) else r
            d.append(('oPos' if rr == 12 else 'R%d' % rr) + MASK[regmask])
        if omask and omux == isilu:
            d.append((OUT[oaddr & 15] if orb else 'c%d' % (oaddr - 96)) + MASK[omask])
        return d
    if mac:
        a, b, c = src(t, 0), src(t, 1), src(t, 2)
        args = {1: [a], 2: [a, b], 3: [a, c], 4: [a, b, c], 13: [a]}.get(mac, [a, b])
        d = dests(mac, f(t, 3, 24, 4), 0) if mac != 13 else ['a0.x']
        out.append('%s %s, %s' % (MAC[mac], ' & '.join(d) or '(none)', ', '.join(args)))
    if ilu:
        d = dests(ilu, f(t, 3, 16, 4), 1)
        out.append('%s %s, %s' % (ILU[ilu], ' & '.join(d) or '(none)', src(t, 2)))
    return ' : '.join(out) + (' [final]' if t[3] & 1 else '')


def main():
    want = sys.argv[2] if len(sys.argv) > 2 else ''
    cur = None
    for line in open(sys.argv[1], errors='replace'):
        if line.startswith('CHIPPROG'):
            cur = line.split()[1]
            if cur.startswith(want):
                print(line.rstrip())
            continue
        if cur and line.startswith('  ') and cur.startswith(want):
            w = line.split()
            if len(w) == 4:
                try:
                    print('   ', dis([int(x, 16) for x in w]))
                except ValueError:
                    cur = None
        elif not line.startswith('  '):
            cur = None


main()
