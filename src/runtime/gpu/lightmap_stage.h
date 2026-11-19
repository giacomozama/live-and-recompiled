#pragma once

#include <algorithm>

#include "app/app.h"
#include "gpu/state.h"

namespace gpu {

// the settings a smoothed lightmap is made with, one number: a change makes them again
inline uint32_t LightmapParams(const app::Settings& set) { return set.lightmap_smoothing << 16 | set.lightmap_size; }

// the lightmap stage of a level draw: a combiner (or fixed function stage) that multiplies what is drawn so far by
// the stage's texture, doubled, and nothing else: Conker's level push buffers (r0 * t1) * 2 into r0
inline bool LightmapStage(const State& s, int stage) {
    if (stage < 1) return false;
    if (!s.ps) {
        const auto& t = s.tss[size_t(stage)];
        auto arg = [](uint32_t a) { return a & 0x3F; };  // D3DTA_TEXTURE 2, D3DTA_CURRENT 1, no modifiers
        return t[kTssColorOp] == 5 && ((arg(t[kTssColorArg1]) == 2 && arg(t[kTssColorArg2]) == 1) ||
                                       (arg(t[kTssColorArg1]) == 1 && arg(t[kTssColorArg2]) == 2));
    }
    auto m = [&](uint32_t method) { return s.simple[method >> 2]; };
    const uint32_t mode = (s.rs[kRsPsTextureModes] >> (stage * 5)) & 0x1F;
    if (mode != 1 && mode != 2) return false;  // PROJECT2D, PROJECT3D (which a 2D texture reads as 2D)
    const uint32_t tex = 8u + uint32_t(stage), r0 = 0xC;
    const uint32_t count = std::min(m(kMPsCombinerCount) & 0xFF, 8u);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t in = m(kMPsRgbInputs + i * 4), out = m(kMPsRgbOutputs + i * 4);
        // an rgb input read as it is: unsigned or signed identity (Conker's levels take the latter), the register
        // number, or 0 for one that isn't
        auto reg = [](uint32_t v) { uint32_t mod = v & 0xE0; return (v & 0x10) || (mod != 0x00 && mod != 0xC0) ? 0xFFu : v & 0xF; };
        uint32_t a = reg(in >> 24), b = reg((in >> 16) & 0xFF), c = reg((in >> 8) & 0xFF), d = reg(in & 0xFF);
        bool ab = (a == tex && (b == r0 || b == 8)) || (b == tex && (a == r0 || a == 8));
        if (!ab || (c != 0 && d != 0)) continue;  // C * D zero
        uint32_t ab_dst = (out >> 4) & 0xF, sum_dst = (out >> 8) & 0xF, fl = out >> 12;
        if ((fl & 0x3F) == 0x10 && (ab_dst == r0 || sum_dst == r0)) return true;  // * 2, no dots or mux
    }
    return false;
}

}  // namespace gpu
