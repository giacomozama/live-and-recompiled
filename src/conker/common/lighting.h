#pragma once

// Characters and props lit per pixel (src/conker/common/lighting.cpp): the vertex programs that light by the room's
// ambient, the key light and the blob shadows get the renderer's gpu::ProgramShading, which works that lighting out
// again at each pixel from the normal and position, with the game's own constants.
namespace lighting {

// settings.toml per_pixel_lighting: the lighting the game works out at each vertex, worked out at each pixel instead,
// so the key light's edge and the blob shadows lie smooth across long triangles rather than crawling along them. Read
// on each draw, so the F1 menu applies it live.
extern bool g_per_pixel;

// hands the renderer the shading; after the settings load, before boot, after fur::Init and glass::Init (their
// entries go first)
void Init();

}  // namespace lighting
