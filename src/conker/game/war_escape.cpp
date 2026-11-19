// BeachEscape (context 140) changes the respawn destination to Harbour, then
// marks the escape complete. BeachSailAway starts its cutscene on the next
// update. Dying in between saves the Harbour destination without the departure
// and prevents the Windmill/BankEntrance story tasks from being started.
#include "cpu/x86_ops.h"

namespace {
constexpr uint32_t kLevel = 0x7FA1F8;
constexpr uint32_t kNextLevel = 0x7FA1F4;
constexpr uint32_t kMultiplayer = 0x7819D5;
constexpr uint32_t kCurrentTask = 0x8018FC;
constexpr uint32_t kProgress = 0x84B290;
constexpr uint32_t kPlayer = 0x84A5F8;
constexpr uint32_t kHealth = 0x3AE;
constexpr uint32_t kActorFlags = 0x200;
constexpr uint32_t kDying = 0x200;
constexpr uint32_t kBeachRunup = 4;
constexpr uint32_t kHarbour = 19;
constexpr uint32_t kBeachEscape = 140;
constexpr uint32_t kTouchesTrigger = 0x1B;
constexpr uint32_t kDepartureTrigger = 0x4027;

bool AtBeach(uint8_t* base) {
    return X86_MEM32(kLevel) == kBeachRunup && !X86_MEM8(kMultiplayer);
}

bool LeavingBeach(uint8_t* base, uint32_t actor) {
    if (!AtBeach(base) || !actor || actor != X86_MEM32(kPlayer) || X86_MEM32(kNextLevel) != kHarbour)
        return false;
    const uint32_t progress = X86_MEM32(kProgress);
    return progress && (X86_MEM8(progress + kBeachEscape / 8) & (1u << (kBeachEscape % 8)));
}
}

// Let the existing death/respawn path win if Conker was already dying when
// BeachEscape checked the exit. Other trigger queries use the original result.
X86_EXTERN_FUNC(__imp__sub_00364130);
X86_FUNC(sub_00364130) {
    if (AtBeach(base) && X86_MEM32(kCurrentTask) == kBeachEscape &&
        X86_MEM32(ctx.esp + 4) == 0 && X86_MEM32(ctx.esp + 8) == kTouchesTrigger) {
        const uint32_t args = X86_MEM32(ctx.esp + 12);
        const uint32_t player = X86_MEM32(kPlayer);
        if (args && X86_MEM32(args + 8) == kDepartureTrigger &&
            (!player || !X86_MEM16(player + kHealth) || (X86_MEM32(player + kActorFlags) & kDying))) {
            ctx.eax = 0;
            ctx.esp += 4;
            return;
        }
    }
    __imp__sub_00364130(ctx, base);
}

// Once the escape has been committed, finish the scripted departure before
// accepting further damage. Protect only the local player in this transition.
X86_EXTERN_FUNC(__imp__sub_002EDDD0);
X86_FUNC(sub_002EDDD0) {
    if (LeavingBeach(base, ctx.eax)) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_002EDDD0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_002DE9A0);
X86_FUNC(sub_002DE9A0) {
    if (LeavingBeach(base, ctx.eax)) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_002DE9A0(ctx, base);
}
