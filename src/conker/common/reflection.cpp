#include "common/reflection.h"

#include <algorithm>

#include "gpu/shaders.h"

bool reflection::g_floor_depth = true;

namespace {

// Heist mirrors the room in its floor the cheap way: the pillars and walls are in the level again upside down under it
// (y 0 down to -20231), Berri drawn again through bones mirrored in it, and the floor laid over them blended, alpha
// from its texture (0.8), without writing depth. Its draws lie flat in the level's space, at 0 and sloping down to -14
// or -23 at its drains: their heights span at most an eighth of their breadth. A laser, a light shaft or a wall stands
// up.
bool Flat(const gpu::WatchedDraw& d) {
    const float (*v)[4] = d.v[0];
    if (!v || d.vertices < 3) return false;
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
