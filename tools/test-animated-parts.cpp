// Exercises the production hooks with the original initializer's cache contract.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cpu/x86_ops.h"

X86_EXTERN_FUNC(sub_00089B70);
X86_EXTERN_FUNC(sub_002A96E0);

namespace {
int g_resolves = 0, g_loads = 0;
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
}

X86_FUNC(__imp__sub_00089B70) {
    if (!X86_MEM8(ctx.esi + 0x10)) {
        const uint32_t scene = X86_MEM32(ctx.esp + 4);
        const uint32_t names = X86_MEM32(scene + 0x10);
        X86_MEM16(ctx.esi + 0x14) = X86_MEM16(names);
        X86_MEM8(ctx.esi + 0x10) = 1;
        ++g_resolves;
    }
    ctx.esp += 4;
}

X86_FUNC(__imp__sub_002A96E0) {
    ++g_loads;
    ctx.esp += 4;
}

int main() {
    std::vector<uint8_t> memory(0x10000);
    uint8_t* base = memory.data();
    X86Context ctx{};
    constexpr uint32_t descriptor = 0x1000, other = 0x1100;
    constexpr uint32_t scene = 0x2000, mp = 0x3000, sp = 0x3100;
    X86_MEM16(mp) = 5;
    X86_MEM16(sp) = 6;
    auto bind = [&](uint32_t record, uint32_t names) {
        ctx.esp = 0x4000;
        ctx.esi = record;
        X86_MEM32(ctx.esp + 4) = scene;
        X86_MEM32(scene + 0x10) = names;
        sub_00089B70(ctx, base);
        Check(ctx.esp == 0x4004, "original initializer returns normally");
        return X86_MEM16(record + 0x14);
    };
    X86_MEM8(descriptor + 0x10) = 1;
    X86_MEM16(descriptor + 0x14) = 5;
    Check(bind(descriptor, sp) == 6, "stale MP bone is rebound for the beehive");
    Check(g_resolves == 1, "first model is resolved once");
    Check(bind(descriptor, sp) == 6 && g_resolves == 1, "same model keeps its cache");
    Check(bind(descriptor, mp) == 5 && g_resolves == 2, "SP -> MP rebinds");
    Check(bind(descriptor, sp) == 6 && g_resolves == 3, "MP -> SP rebinds");
    Check(bind(other, mp) == 5 && g_resolves == 4, "descriptors have independent caches");
    Check(bind(descriptor, sp) == 6 && g_resolves == 4, "another descriptor cannot evict the model");
    // A level reload can put a different model at the same guest address.
    ctx.esp = 0x4000;
    sub_002A96E0(ctx, base);
    Check(g_loads == 1 && ctx.esp == 0x4004, "level loading still calls the original function");
    X86_MEM16(sp) = 9;
    Check(bind(descriptor, sp) == 9 && g_resolves == 5, "reload invalidates reused model addresses");
    Check(bind(descriptor, sp) == 9 && g_resolves == 5, "reloaded model is cached again");
    std::puts("PASS: animated-part cache follows model changes and level reloads.");
}
