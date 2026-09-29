#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "codegen.h"

namespace fs = std::filesystem;

static std::string F(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string F(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// rewriting identical files would force a full rebuild of the generated code
static void WriteIfChanged(const fs::path& p, const std::string& text) {
    std::ifstream in(p, std::ios::binary);
    if (in) {
        std::string old((std::istreambuf_iterator<char>(in)), {});
        if (old == text) return;
    }
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string CodeGen::EmitFunction(uint32_t entry) {
    entry_ = entry;
    pending_.valid = false;
    std::vector<uint32_t> body = an_.Body(entry);
    body_set_.clear();
    body_set_.insert(body.begin(), body.end());
    labels_.clear();
    // branch targets need labels, collect them before emitting so the peephole never spans one
    std::set<uint32_t> targets;
    for (uint32_t va : body) {
        const Insn* in = an_.InsnAt(va);
        if (!in) continue;
        if (in->flow == Flow::Jcc || in->flow == Flow::Jmp) targets.insert(in->target);
        if (in->flow == Flow::JmpIndirect) {
            auto jt = an_.jump_tables.find(va);
            if (jt != an_.jump_tables.end()) targets.insert(jt->second.targets.begin(), jt->second.targets.end());
        }
    }
    for (size_t i = 0; i < body.size(); i++) {
        const Insn* in = an_.InsnAt(body[i]);
        if (!in) continue;
        uint32_t next = body[i] + in->len;
        bool falls = in->flow == Flow::Normal || in->flow == Flow::Call || in->flow == Flow::CallIndirect || in->flow == Flow::Jcc;
        if (falls && (i + 1 >= body.size() || body[i + 1] != next)) targets.insert(next);
    }
    body_.clear();
    if (body.empty()) Line(F("\tx86_trap(ctx, 0x%X, \"function was not decoded\");", entry));
    for (size_t i = 0; i < body.size(); i++) {
        uint32_t va = body[i];
        if ((targets.count(va) && va != entry) || (va == entry && body.front() != entry)) {
            Line(F("loc_%08X:", va));
            pending_.valid = false;
        }
        const Insn* info = an_.InsnAt(va);
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!info || info->flow == Flow::Invalid || !an_.Decode(va, in, ops)) {
            Line(F("\tx86_trap(ctx, 0x%X, \"undecodable instruction\");", va));
            pending_.valid = false;
            continue;
        }
        Line("\t// " + F("%08X ", va) + Text(va, in, ops));
        uint32_t next_body = i + 1 < body.size() ? body[i + 1] : 0;
        EmitInsn(va, in, ops, next_body);
        bool falls = info->flow == Flow::Normal || info->flow == Flow::Call || info->flow == Flow::CallIndirect ||
                     info->flow == Flow::Jcc;
        uint32_t next = va + info->len;
        if (falls && next_body != next) {
            if (an_.InsnAt(next) || an_.functions.count(next))
                Line("\t" + Target(next));
            else
                Line(F("\tx86_trap(ctx, 0x%X, \"fell off decoded code\");", next));
        }
    }
    // a function that registers an SEH frame (mov fs:[0], ...) may be where a C++ catch resumes: the CRT's jump to
    // the continuation throws X86Resume, caught by the innermost such frame whose stack holds the resume esp
    bool seh = false;
    for (uint32_t va : body) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (an_.Decode(va, in, ops) && in.mnemonic == ZYDIS_MNEMONIC_MOV && ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            ops[0].mem.segment == ZYDIS_REGISTER_FS && ops[0].mem.base == ZYDIS_REGISTER_NONE &&
            ops[0].mem.index == ZYDIS_REGISTER_NONE && ops[0].mem.disp.value == 0)
            seh = true;
    }
    std::string out;
    out += F("X86_FUNC(__imp__%s) {\n", FuncName(entry).c_str());
    if (seh) out += "\tconst uint32_t x86_frame = ctx.esp;\n\ttry {\n";
    out += "\tX86_LOAD_FLAGS();\n\tuint32_t ca = 0, cb = 0;\n\t(void)ca; (void)cb;\n";
    // code below the entry (a shared tail reached by a jump) is emitted first, so enter by label
    bool entry_first = !body.empty() && body.front() == entry;
    if (!entry_first) out += F("\tgoto loc_%08X;\n", entry);
    if (labels_.count(entry) && entry_first) out += F("loc_%08X:;\n", entry);
    out += body_;
    if (seh) out += "\t} catch (const X86Resume& r) {\n\t\tif (r.esp >= x86_frame) throw;\n\t\tx86_resume(ctx, base, r);\n\t}\n";
    out += "}\n";
    out += F("X86_WEAK_FUNC(%s, __imp__%s)\n\n", FuncName(entry).c_str(), FuncName(entry).c_str());
    return out;
}

bool CodeGen::Run() {
    std::vector<uint32_t> funcs(an_.functions.begin(), an_.functions.end());
    std::sort(funcs.begin(), funcs.end());

    // symbols the host binds to by name; one missing from this image gets a placeholder address whose
    // recompiled body traps, so the host override still links
    std::string syms = "#pragma once\n";
    syms += F("#define IMAGE_XBE \"%s\"\n", fs::path(an_.cfg.xbe_path).filename().string().c_str());
    std::vector<uint32_t> absent;
    for (auto& [name, va] : an_.cfg.symbols) {
        uint32_t a = va ? va : 0xFFFF0000u + uint32_t(absent.size());
        if (!va) absent.push_back(a);
        syms += F("#define SYM_%s %08X\n#define HAS_%s %d\n", name.c_str(), a, name.c_str(), va ? 1 : 0);
    }
    for (auto& [name, va] : an_.cfg.data) syms += F("#define SYM_%s %08X\n#define HAS_%s %d\n", name.c_str(), va, name.c_str(), va ? 1 : 0);
    WriteIfChanged(out_ / "image_symbols.h", syms);

    // shared declarations
    std::string shared = "#pragma once\n#include \"cpu/x86_ops.h\"\n\n";
    for (uint32_t f : funcs) shared += F("X86_EXTERN_FUNC(%s);\n", FuncName(f).c_str());
    for (uint32_t f : absent) shared += F("X86_EXTERN_FUNC(%s);\n", FuncName(f).c_str());
    WriteIfChanged(out_ / "x86_recomp_shared.h", shared);

    // partition by instruction count so the files compile in similar time
    size_t total = 0;
    std::vector<size_t> weight(funcs.size());
    for (size_t i = 0; i < funcs.size(); i++) {
        weight[i] = an_.Body(funcs[i]).size() + 8;
        total += weight[i];
    }
    uint32_t parts = std::max<uint32_t>(1, an_.cfg.partitions);
    size_t per = total / parts + 1;
    size_t fi = 0;
    for (uint32_t p = 0; p < parts; p++) {
        std::string text = "#include \"x86_recomp_shared.h\"\n\n";
        size_t acc = 0;
        while (fi < funcs.size() && (acc < per || p == parts - 1)) {
            text += EmitFunction(funcs[fi]);
            acc += weight[fi];
            fi++;
        }
        WriteIfChanged(out_ / F("x86_recomp.%u.cpp", p), text);
    }

    std::string map = "#include \"x86_recomp_shared.h\"\n\n";
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (auto& s : an_.img.sections) {
        bool code = false;
        for (auto& n : an_.cfg.code_sections) code |= s.name == n;
        for (auto& pfx : an_.cfg.code_prefixes) code |= s.name.rfind(pfx, 0) == 0;
        if (!code) continue;
        lo = std::min(lo, s.va);
        hi = std::max(hi, s.va + s.rsize);
    }
    map += F("const uint32_t X86CodeBase = 0x%Xu;\nconst uint32_t X86CodeEnd = 0x%Xu;\n", lo, hi);
    map += F("const uint32_t X86ImageBase = 0x%Xu;\nconst uint32_t X86ImageEnd = 0x%Xu;\n", an_.img.base,
             an_.img.base + an_.img.image_size);
    map += F("const uint32_t X86EntryPoint = 0x%Xu;\nconst uint32_t X86KernelThunk = 0x%Xu;\n\n", an_.img.entry,
             an_.img.kernel_thunk);
    map += "const X86FuncMapping X86FuncMappings[] = {\n";
    for (uint32_t f : funcs) map += F("\t{0x%Xu, %s},\n", f, FuncName(f).c_str());
    map += "\t{0, nullptr},\n};\n";
    map += "\nconst uint32_t X86Continuations[] = {\n";
    for (uint32_t c : an_.continuations) map += F("\t0x%Xu,\n", c);
    map += "\t0,\n};\n";
    for (uint32_t f : absent) {
        map += F("\nX86_FUNC(__imp__%s) { x86_trap(ctx, 0x%Xu, \"not in this image\"); }\n", FuncName(f).c_str(), f);
        map += F("X86_WEAK_FUNC(%s, __imp__%s)\n", FuncName(f).c_str(), FuncName(f).c_str());
    }
    WriteIfChanged(out_ / "x86_func_mapping.cpp", map);

    std::string stats = F("functions %zu\ninstructions %zu\n", funcs.size(), insn_count_);
    for (auto& [k, v] : unimpl_) stats += F("unimplemented %s %zu\n", k.c_str(), v);
    WriteIfChanged(out_ / "x86_recomp_stats.txt", stats);
    std::fprintf(stderr, "%s", stats.c_str());
    return true;
}
