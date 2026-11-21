// A character's face (Conker's mouth) left in a shape it had moved on from, at any frame rate: the game's own blend.
//
// The face is a set of named blend weights on the head model: visemes EEE AAA OOO FFF MMM and expressions HAPPY SAD
// SHOCK MAD GRIN BREATH QUIZICAL DOH ILL WHISTLE (table 0x6B3FD0 for character types 0, 0x111, 0x114, 0x96; the
// fire head's at 0x6B4010; picked through 0x59581C: u16 type, byte count, table), shape 0 the rest face (no name, no
// weight). 0xFEA00 (cdecl: target shape, blend ticks, player index), from the player's update 0x7E970 with
// [player+0x248] / [player+0x249], eases the blend [player+0x46C] (+0 t, +4 step a tick, byte +8 the shape it comes
// from, +9 the one it goes to, +A frames left to keep writing once there) by [0x7FA20C] ticks a frame, writing t to
// the target's weight and 1 - t to the previous one (0xFEE20: model, node, shape name, weight). Only those two: a
// shape changed again before its blend ends leaves the shape before at its last weight, and nothing but a cutscene's
// start (0x53AA0 -> 0xFED10) zeroes them all. Three frames after t reaches 1 it stops writing; the weights stay.
//
// The faces come from 0x2CEFC0 / 0x2CED40 ([player+0x140], its record in [0x743010 + type] giving the mouth), the
// lip sync among them. Conker's idle faces (set from 0x304E5E) go AAA, back to rest over 10 ticks, then DOH: when DOH
// came 6 ticks into the ease back, the blend went rest -> DOH and AAA kept 0.4-0.5, the mouth half open under every
// face that followed until AAA came round again.
//
// Here, when a blend starts, every shape it does not drive that still has weight fades out with it: that weight times
// 1 - t, written as the game writes its two (0xFEE20, on the frames the game writes), so nothing jumps and all of it
// is gone when t reaches 1. XBE_FACE_STALE=1 leaves the game's blend as it was.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "core/diag.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kPlayers = 0x84A5F8, kPlayerSize = 0x6D0;
constexpr uint32_t kShapeTables = 0x59581C;  // 5 of 8 bytes: u16 character type, byte count, names
constexpr uint32_t kSetWeight = 0xFEE20;

using kernel::R32;
uint8_t U8(uint32_t va) { return kernel::g_base[va]; }
uint16_t U16(uint32_t va) {
    uint16_t v;
    std::memcpy(&v, kernel::g_base + va, 2);
    return v;
}
float F32(uint32_t va) {
    float v;
    std::memcpy(&v, kernel::g_base + va, 4);
    return v;
}

bool KeepStale() {
    static const bool on = [] {
        const char* e = diag::Env("XBE_FACE_STALE");
        return e && *e && *e != '0';
    }();
    return on;
}

// the character's shape table, as 0xFEA00 picks it, 0 for none
uint32_t ShapeTable(uint32_t player) {
    const uint8_t sel = U8(player + 0x3A9);
    const uint32_t list = R32(player + 0x50C);
    const uint32_t type = sel ? R32(list - 4 + sel * 4) : R32(list);
    for (uint32_t i = 0; i < 5; i++)
        if (U16(kShapeTables + i * 8) == type) return kShapeTables + i * 8;
    return 0;
}

// the last weight 0xFEE20 wrote, by (node, shape name); its writes counted
std::map<std::pair<uint32_t, uint32_t>, float> g_weights;
uint32_t g_writes = 0;

// shapes fading out with a player's blend: name, weight at its start
struct Fade {
    uint32_t name;
    float from;
};
std::map<uint32_t, std::vector<Fade>> g_fades;  // by player index

// a player's face nodes as 0xFEA00 walks them: model ([[player+0x4B0+4k]+0x24]+8) and node, for each it writes
template <class F>
void ForNodes(uint32_t index, F&& f) {
    const uint32_t players = R32(kPlayers), player = players + index * kPlayerSize;
    uint32_t nodes = U8(player + 0x511);
    if (!nodes) nodes = 1;
    for (uint32_t k = 0; k < nodes; k++) {
        const uint32_t inst = R32(R32(player + 0x4B0 + 4 * k) + 0x24);
        if (!inst) continue;
        f(R32(inst + 8), R32(players + 0x4C4 + (index * 0x1B4 + k) * 4));
    }
}

}  // namespace

// 0xFEE20 (cdecl model, node, shape name, weight): one shape's weight
X86_EXTERN_FUNC(__imp__sub_000FEE20);
X86_FUNC(sub_000FEE20) {
    if (R32(ctx.esp + 12)) g_weights[{R32(ctx.esp + 8), R32(ctx.esp + 12)}] = F32(ctx.esp + 16);
    g_writes++;
    __imp__sub_000FEE20(ctx, base);
}

// 0xFEA00 (cdecl target, ticks, player index): the face blend, once a frame
X86_EXTERN_FUNC(__imp__sub_000FEA00);
X86_FUNC(sub_000FEA00) {
    if (KeepStale()) {
        __imp__sub_000FEA00(ctx, base);
        return;
    }
    const uint32_t index = R32(ctx.esp + 12);
    const uint32_t player = R32(kPlayers) + index * kPlayerSize, blend = R32(player + 0x46C);
    const uint8_t to0 = blend ? U8(blend + 9) : 0;
    const uint32_t writes = g_writes;
    __imp__sub_000FEA00(ctx, base);
    if (!blend) return;
    const uint32_t table = ShapeTable(player);
    if (!table) return;
    const uint8_t from = U8(blend + 8), to = U8(blend + 9);
    const uint32_t names = R32(table + 4), count = U8(table + 2);
    const uint32_t from_name = from < count ? R32(names + from * 4) : 0, to_name = to < count ? R32(names + to * 4) : 0;

    std::vector<Fade>& fades = g_fades[index];
    // a blend started: what has weight besides the two it drives fades out from where it is
    if (to != to0) {
        fades.clear();
        for (uint32_t s = 1; s < count; s++) {
            const uint32_t name = R32(names + s * 4);
            if (!name || name == from_name || name == to_name) continue;
            float most = 0.0f;
            ForNodes(index, [&](uint32_t, uint32_t node) {
                auto it = g_weights.find({node, name});
                if (it != g_weights.end()) most = std::max(most, it->second);
            });
            if (most > 0.0f) fades.push_back({name, most});
        }
    }
    // on the frames the game wrote its two
    if (!fades.empty() && g_writes != writes) {
        const float t = std::clamp(F32(blend), 0.0f, 1.0f);
        for (const Fade& f : fades) {
            const float w = f.from * (1.0f - t);
            ForNodes(index, [&](uint32_t model, uint32_t node) {
                kernel::CallGuest(ctx, kSetWeight, {model, node, f.name, std::bit_cast<uint32_t>(w)});
            });
        }
        if (t >= 1.0f) fades.clear();
    }
}
