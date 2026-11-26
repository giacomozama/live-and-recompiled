#include "common/reflection.h"

#include <algorithm>

#include "gpu/shaders.h"

bool reflection::g_floor_depth;

namespace {

// Heist mirrors the room in its floor the cheap way: the pillars and walls are in the level again upside down under it
// (y 0 down to -20231), Berri drawn again through bones mirrored in it, and the floor laid over them blended, alpha
// from its texture (0.8), without writing depth. Its draws lie flat in the level's space, at 0 and sloping down to -14
// or -23 at its drains: their heights span at most an eighth of their breadth. A laser, a light shaft or a wall stands
// up. Water's ripples lie flat too, but bend a reflection through a bump map (stage mode 6 or 7): the rings where the
// dam's stream falls into the river (F3 capture 020546_0, fans 0272..0299) sit on the river, which writes its own
// depth rippled by dynamic water, so theirs, flat, stepped some 8 units in and out of it ring by ring, and the depth of
// field and ambient occlusion drew the rings' edges
bool Bumped(uint32_t tex_modes) {
    for (int i = 1; i < 4; i++) {
        uint32_t m = (tex_modes >> (i * 5)) & 0x1F;
        if (m == 6 || m == 7) return true;
    }
    return false;
}

bool Flat(const gpu::WatchedDraw& d) {
    const float (*v)[4] = d.v[0];
    if (!v || d.vertices < 3 || Bumped(d.tex_modes)) return false;
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++) lo[k] = hi[k] = v[0][k];
    for (uint32_t i = 1; i < d.vertices; i++) {
        for (int k = 0; k < 3; k++) {
            lo[k] = std::min(lo[k], v[i][k]);
            hi[k] = std::max(hi[k], v[i][k]);
        }
    }
    float x = hi[0] - lo[0], y = hi[1] - lo[1], z = hi[2] - lo[2];
    return x > 0 && z > 0 && y * 8 <= std::max(x, z);
}

}  // namespace

void reflection::Init() { gpu::g_surface_depth = {.on = &g_floor_depth, .takes = Flat}; }
