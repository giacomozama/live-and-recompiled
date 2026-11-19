#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cpu/x86_ops.h"

X86_EXTERN_FUNC(sub_00364130);
X86_EXTERN_FUNC(sub_002EDDD0);
X86_EXTERN_FUNC(sub_002DE9A0);

namespace {
constexpr uint32_t kPlayer = 0x20000, kEnemy = 0x30000, kProgress = 0x10000;
constexpr uint32_t kStack = 0x40000, kArgs = 0x41000;
constexpr uint32_t kHealth = 0x3AE;
void Check(bool ok, const char* message) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}
}

X86_FUNC(__imp__sub_00364130) {
    (void)base;
    ctx.eax = 0x4001;
    ctx.esp += 4;
}
X86_FUNC(__imp__sub_002EDDD0) {
    --X86_MEM16(ctx.eax + kHealth);
    ctx.esp += 4;
}
X86_FUNC(__imp__sub_002DE9A0) {
    X86_MEM16(ctx.eax + kHealth) = 0;
    ctx.esp += 4;
}

int main() {
    std::vector<uint8_t> mem(0x850000);
    uint8_t* base = mem.data();
    X86Context ctx{};
    X86_MEM32(0x7FA1F8) = X86_MEM32(0x7FA1F4) = 4;
    X86_MEM32(0x8018FC) = 140;
    X86_MEM32(0x84A5F8) = kPlayer;
    X86_MEM32(0x84B290) = kProgress;
    X86_MEM32(kStack + 8) = 0x1B;
    X86_MEM32(kStack + 12) = kArgs;
    X86_MEM32(kArgs + 8) = 0x4027;
    auto exit_query = [&] {
        ctx.esp = kStack;
        sub_00364130(ctx, base);
        Check(ctx.esp == kStack + 4, "trigger query must keep the original stack convention");
        return ctx.eax;
    };
    auto hit = [&](auto original, uint32_t actor) {
        ctx.eax = actor;
        ctx.esp = kStack;
        original(ctx, base);
        Check(ctx.esp == kStack + 4, "damage must keep the original stack convention");
        return X86_MEM16(actor + kHealth);
    };

    Check(exit_query() == 0, "a dead player cannot complete the escape");
    X86_MEM16(kPlayer + kHealth) = 3;
    X86_MEM32(kPlayer + 0x200) = 0x200;
    Check(exit_query() == 0, "a player already dying cannot complete the escape");
    X86_MEM32(kPlayer + 0x200) = 0;
    Check(exit_query() == 0x4001, "living players can finish normally");
    Check(hit(sub_002EDDD0, kPlayer) == 2, "ordinary beach combat still deals damage");
    Check(hit(sub_002DE9A0, kPlayer) == 0, "death before the exit still takes effect");
    X86_MEM32(kArgs + 8) = 0x402A;
    Check(exit_query() == 0x4001, "the guard applies only to the departure trigger");

    X86_MEM16(kPlayer + kHealth) = X86_MEM16(kEnemy + kHealth) = 3;
    X86_MEM32(0x7FA1F4) = 19;
    X86_MEM8(kProgress + 140 / 8) = 1u << (140 % 8);
    Check(hit(sub_002EDDD0, kPlayer) == 3, "damage cannot interrupt the committed departure");
    Check(hit(sub_002DE9A0, kPlayer) == 3, "the reproduced death cannot interrupt the committed departure");
    Check(hit(sub_002EDDD0, kEnemy) == 2, "enemies retain original damage handling");
    X86_MEM8(0x7819D5) = 1;
    Check(hit(sub_002EDDD0, kPlayer) == 2, "multiplayer damage is unaffected");
    X86_MEM8(0x7819D5) = 0;
    X86_MEM32(0x7FA1F8) = 19;
    Check(hit(sub_002DE9A0, kPlayer) == 0, "normal death handling resumes in the harbour");
    std::puts("PASS: death before exit, damage after exit, and unaffected paths.");
}
