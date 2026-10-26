#pragma once

// Reflective floors (src/conker/common/reflection.cpp): the renderer's gpu::g_surface_depth.
namespace reflection {

// settings.toml floor_depth: a see-through floor over a reflection writes its own depth, so the depth of field blurs
// it by its distance, the reflection with it, instead of by what lies under it as on the console: the floor sharp in
// the outline of Berri's reflection, or of the pillars', and the rest of it blurred as the far background. Read on each
// draw, so the F1 menu applies it live.
extern bool g_floor_depth;

// hands the renderer the floor's test; after the settings load, before boot
void Init();

}  // namespace reflection
