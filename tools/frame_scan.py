#!/usr/bin/env python3
"""Static scan of the Conker XBE for per-frame updates that ignore the frame's ticks/dt (60 FPS leftovers).

For every function (from the recompiler's mapping, generated/game/x86_func_mapping.cpp) it sweeps linearly with a
crude taint dataflow: registers, the x87 stack, stack slots and globals carry tags of where their value came from. A
store to memory whose value derives from that same memory (an update) is reported unless the value also derives
from a timing global (ticks [0x7FA20C], dt [0x7FA21C] and the rest of TIMING).

Columns: function, site, T if the function reads a timing global anywhere, kind, target, instruction, other globals.
Kinds: const (a fixed step: counters), byglobal/byfield (another value added: velocities), mul (a factor: eases and
decays), call (a call's result), k (a constant from .rdata in the chain).
It finds the shape, not whether the code runs every frame: most hits are one-off events. --hooks narrows that down
at run time (src/conker/game/frame_sites.cpp, XBE_FRAME_SITES).

Run from the repo root (needs python capstone and a generated/game from a build):
  python3 tools/frame_scan.py [--fields] > out.tsv   globals only, or object fields too
  python3 tools/frame_scan.py --hooks                 every global and field site the recompiler emits as an
                                                      instruction: src/conker/game/frame_sites.inc and the
                                                      [[hook]] entries in generated/frame_sites.toml
"""
import re, struct, sys, collections
import capstone
from capstone import x86

ROOT = "."
XBE = f"{ROOT}/assets/default.xbe"
d = open(XBE, "rb").read()
u32 = lambda o: struct.unpack_from("<I", d, o)[0]
base = u32(0x104)
secs = []
for i in range(u32(0x11C)):
    o = u32(0x120) - base + i * 56
    va, vsz, raw, rsz, namep = u32(o + 4), u32(o + 8), u32(o + 12), u32(o + 16), u32(o + 20)
    secs.append((d[namep - base:namep - base + 16].split(b"\0")[0].decode("latin1"), va, vsz, raw, rsz))
text = next(s for s in secs if s[0] == ".text")
TEXT_END = text[1] + text[2]
DATA_LO, DATA_HI = 0x64F980, 0x881A80
RDATA_LO, RDATA_HI = 0x561000, 0x64F980

TIMING = {0x7FA20C, 0x7FA210, 0x7FA214, 0x7FA218, 0x7FA21C, 0x7FA250, 0x7FDB8C}

funcs = sorted(int(m, 16) for m in re.findall(r"\{0x([0-9A-F]+)u, sub_", open(f"{ROOT}/generated/game/x86_func_mapping.cpp").read()))
funcs = [f for f in funcs if text[1] <= f < TEXT_END]

def bytes_at(va, n):
    return d[text[3] + va - text[1]: text[3] + va - text[1] + n]

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

MOVLIKE = {"mov", "movss", "movsd", "movd", "movq", "movaps", "movups", "movapd", "movzx", "movsx", "lea", "movdqa",
           "movdqu", "cvtsi2ss", "cvtss2sd", "cvtsd2ss", "cvttss2si", "cvtss2si", "cvtdq2ps", "cvtps2dq", "cvttps2dq",
           "cvtpi2ps", "sqrtss", "rsqrtss", "rcpss", "pshufd", "shufps", "movlps", "movhps", "movsxd", "setne", "sete",
           "setg", "setl", "seta", "setb", "setae", "setbe", "setge", "setle", "cmove", "cmovne"}
MUL = {"imul", "mul", "mulss", "mulps", "divss", "divps", "div", "idiv", "fmul", "fmulp", "fdiv", "fdivp", "fdivr",
       "fdivrp", "fimul", "fidiv", "sar", "shr", "shl"}
ADD = {"add", "sub", "addss", "subss", "addps", "subps", "fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp", "inc",
       "dec", "fiadd", "fisub", "adc", "sbb"}

def canon(r):
    n = md.reg_name(r)
    for big, smalls in (("eax", "ax al ah"), ("ebx", "bx bl bh"), ("ecx", "cx cl ch"), ("edx", "dx dl dh"),
                        ("esi", "si"), ("edi", "di"), ("ebp", "bp"), ("esp", "sp")):
        if n in smalls.split():
            return big
    return n

def mem_key(op, esp_delta, regver):
    m = op.mem
    if m.base == 0 and m.index == 0:
        return ("G", m.disp & 0xFFFFFFFF)
    b = canon(m.base) if m.base else None
    if b == "esp" and m.index == 0:
        return ("S", m.disp + esp_delta)
    if m.index:
        return ("A", b, regver.get(b, 0), m.disp)  # array: not tracked as the same slot
    return ("F", b, regver.get(b, 0), m.disp)

def scan(fva, fend, out):
    reg = collections.defaultdict(frozenset)   # register -> tags
    regver = {}                                  # register -> version (a field key's base identity)
    memt = {}                                    # tracked memory -> tags
    fpu = []                                     # x87 stack of tags
    esp = 0
    reads_timing = False
    sites = []
    code = bytes_at(fva, fend - fva)
    for ins in md.disasm(code, fva):
        mn = ins.mnemonic
        if mn.startswith("int3"):
            break
        ops = ins.operands
        # tags of memory operands read
        def tags_of_mem(op):
            k = mem_key(op, esp, regver)
            t = memt.get(k)
            if k[0] == "G":
                a = k[1]
                if a in TIMING:
                    return frozenset({"T"}) | (t or frozenset())
                if RDATA_LO <= a < RDATA_HI:
                    return frozenset({"K"})  # a constant
                base_t = frozenset({("L",) + k})
            elif k[0] == "S":
                base_t = frozenset({"S"})
            else:
                base_t = frozenset({("L",) + k})
            return (t | base_t) if t else base_t

        if mn == "call":
            for r in ("eax", "ecx", "edx"):
                reg[r] = frozenset({"R"}); regver[r] = regver.get(r, 0) + 1
            for i in range(8):
                reg[f"xmm{i}"] = frozenset({"R"})
            fpu = [frozenset({"R"})] if fpu else []
            memt = {k: v for k, v in memt.items() if k[0] == "S"}
            continue
        if mn == "push":
            esp -= 4
            if ops[0].type == x86.X86_OP_REG:
                memt[("S", esp)] = reg[canon(ops[0].reg)]
            elif ops[0].type == x86.X86_OP_MEM:
                memt[("S", esp)] = tags_of_mem(ops[0])
            else:
                memt[("S", esp)] = frozenset({"C"})
            continue
        if mn == "pop":
            if ops[0].type == x86.X86_OP_REG:
                r = canon(ops[0].reg); reg[r] = memt.get(("S", esp), frozenset({"S"})); regver[r] = regver.get(r, 0) + 1
            esp += 4
            continue
        if mn in ("sub", "add") and ops[0].type == x86.X86_OP_REG and canon(ops[0].reg) == "esp" and ops[1].type == x86.X86_OP_IMM:
            esp += -ops[1].imm if mn == "sub" else ops[1].imm
            continue
        if mn in ("ret", "jmp") and (mn == "ret" or ops[0].type != x86.X86_OP_IMM):
            pass

        # x87
        if mn.startswith("f"):
            memops = [o for o in ops if o.type == x86.X86_OP_MEM]
            mt = tags_of_mem(memops[0]) if memops else None
            if mt is not None and "T" in mt:
                reads_timing = True
            st = lambda i: fpu[-1 - i] if len(fpu) > i else frozenset()
            if mn in ("fld", "fild"):
                fpu.append(mt if mt is not None else st(0))
            elif mn in ("fld1", "fldz", "fldpi"):
                fpu.append(frozenset({"C"}))
            elif mn in ("fst", "fstp", "fistp", "fist", "fisttp"):
                v = st(0)
                if memops:
                    k = mem_key(memops[0], esp, regver)
                    sites.append((ins, k, v))
                    memt[k] = v
                if mn.endswith("p") and fpu:
                    fpu.pop()
            elif mn == "fxch":
                if len(fpu) >= 2:
                    fpu[-1], fpu[-2] = fpu[-2], fpu[-1]
            elif mn in ("faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp"):
                if len(fpu) >= 2:
                    a = fpu.pop()
                    fpu[-1] = fpu[-1] | a | (frozenset({"X"}) if mn in MUL and "T" not in a else frozenset({"A"}))
            elif mn in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr", "fiadd", "fisub", "fimul", "fidiv"):
                src = mt if mt is not None else (st(1) if len(ops) < 2 else st(0) | st(1))
                if fpu:
                    fpu[-1] = fpu[-1] | src | (frozenset({"X"}) if mn in MUL and "T" not in src else (frozenset({"A"}) if mn in ADD else frozenset()))
            elif mn in ("fcomp", "fcompp", "fucompp", "fucomp"):
                if fpu: fpu.pop()
                if mn.endswith("pp") and fpu: fpu.pop()
            elif mn == "fchs" or mn == "fabs":
                pass
            continue

        if not ops:
            continue
        # generic
        try:
            rr, rw = ins.regs_access()
        except capstone.CsError:
            continue
        src = frozenset()
        memw = None
        dst_read = True
        for i, o in enumerate(ops):
            if o.type == x86.X86_OP_MEM:
                if mn == "lea":
                    for rg in (o.mem.base, o.mem.index):
                        if rg: src |= reg[canon(rg)]
                    continue
                if i == 0 and (o.access & capstone.CS_AC_WRITE):
                    memw = o
                    if o.access & capstone.CS_AC_READ:
                        src |= tags_of_mem(o)
                else:
                    src |= tags_of_mem(o)
                for rg in (o.mem.base, o.mem.index):
                    pass
            elif o.type == x86.X86_OP_REG:
                if i == 0 and mn in MOVLIKE:
                    continue
                if i == 0 and mn == "xor" and len(ops) == 2 and ops[1].type == x86.X86_OP_REG and ops[1].reg == o.reg:
                    src = frozenset({"C"})
                    break
                if i == 0 and mn in ("xorps", "pxor") and ops[1].type == x86.X86_OP_REG and ops[1].reg == o.reg:
                    src = frozenset({"C"})
                    break
                src |= reg[canon(o.reg)]
            elif o.type == x86.X86_OP_IMM:
                src |= frozenset({"C"})
        if "T" in src:
            reads_timing = True
        if mn in MUL and "T" not in src:
            src |= frozenset({"X"})
        if mn in ADD:
            src |= frozenset({"A"})
        if mn in ("cmp", "test", "comiss", "ucomiss", "bt") or mn.startswith("j"):
            continue
        if memw is not None:
            k = mem_key(memw, esp, regver)
            sites.append((ins, k, src))
            memt[k] = src
        else:
            for r in rw:
                n = canon(r)
                if n in ("eflags", "esp", "eip"):
                    continue
                reg[n] = src
                regver[n] = regver.get(n, 0) + 1
    for ins, k, v in sites:
        if k[0] not in ("G", "F"):
            continue
        if ("L",) + k not in v:
            continue  # not an update of itself
        if "T" in v:
            continue
        if "A" not in v and "X" not in v:
            continue
        if "R" in v and k[0] == "F":
            continue
        others = sorted({t[1] if t[0] == "L" and t[1] == "G" else None for t in v if isinstance(t, tuple)} - {None})
        gl = sorted({t[2] for t in v if isinstance(t, tuple) and t[0] == "L" and t[1] == "G" and t[2] != k[1]}) if k[0] == "G" else []
        fl = sorted({t[4] for t in v if isinstance(t, tuple) and t[0] == "L" and t[1] == "F" and t[1:] != k}) if True else []
        kind = []
        if "X" in v: kind.append("mul")
        if gl: kind.append("byglobal")
        if fl: kind.append("byfield")
        if "R" in v: kind.append("call")
        if not kind: kind.append("const")
        if "K" in v: kind.append("k")
        out.append((fva, ins.address, ins.mnemonic + " " + ins.op_str, k, ",".join(kind), reads_timing,
                    " ".join(hex(a) for a in gl[:4])))
    return reads_timing

res = []
for i, f in enumerate(funcs):
    end = funcs[i + 1] if i + 1 < len(funcs) else TEXT_END
    if end - f > 0x20000:
        end = f + 0x20000
    out = []
    try:
        rt = scan(f, end, out)
    except Exception as e:
        print(f"# {f:#x}: {e}", file=sys.stderr)
        continue
    if out:
        for r in out:
            res.append(r[:5] + (rt,) + r[6:])
def keep(k, txt):
    if txt.split()[0] in ("or", "and", "shr", "shl", "xor", "adc", "sbb", "sar"):
        return False  # flags and bit packing
    return k[0] == "F" or DATA_LO <= k[1] < DATA_HI


def target(k):
    return f"{k[1]:#x}" if k[0] == "G" else f"[{k[1]}+{k[3]:#x}]"


if "--hooks" in sys.argv:
    import glob, os
    emitted = set()
    for path in glob.glob(f"{ROOT}/generated/game/x86_recomp.*.cpp"):
        emitted.update(int(m, 16) for m in re.findall(r"^\t// ([0-9A-F]{8}) ", open(path).read(), re.M))
    sites = {}
    for fva, at, txt, k, kind, rt, gl in res:
        if keep(k, txt) and at in emitted and at not in sites:
            sites[at] = (fva, kind, target(k), txt)
    with open(f"{ROOT}/src/conker/game/frame_sites.inc", "w") as f:
        f.write("// written by tools/frame_scan.py --hooks: SITE(index, address, function, kind, target, instruction)\n")
        for i, (at, (fva, kind, tgt, txt)) in enumerate(sorted(sites.items())):
            f.write(f'SITE({i}, 0x{at:X}, 0x{fva:X}, "{kind}", "{tgt}", "{txt}")\n')
    os.makedirs(f"{ROOT}/generated", exist_ok=True)
    with open(f"{ROOT}/generated/frame_sites.toml", "w") as f:
        for at in sorted(sites):
            f.write(f'\n[[hook]]\nat = 0x{at:X}\ncall = "ConkerSite_0x{at:X}"\n')
    print(f"{len(sites)} sites", file=sys.stderr)
    sys.exit(0)

fields = "--fields" in sys.argv
for fva, at, txt, k, kind, rt, gl in res:
    if k[0] == "F" and not fields:
        continue
    if not keep(k, txt):
        continue
    print(f"{fva:#x}\t{at:#x}\t{'T' if rt else '-'}\t{kind}\t{target(k)}\t{txt}\t{gl}")
