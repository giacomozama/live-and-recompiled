#pragma once

// Fur shells (src/common/fur.cpp): the vertex programs that draw a character's fur as shells pushed out along the
// normal, one draw per shell, get the renderer's gpu::ProgramShading; with fur_shading off too, for the depth only
// their opaque strands write (a depth of field then blurs the backdrop seen through their faint edges).
namespace fur {

// settings.toml fur_shading: the shells darken toward the roots and catch a highlight along the strands and a rim
// of light, and their cut out strands blend in at the edges instead of stepping. Read on each draw, so the F1 menu
// applies it live.
extern bool g_shading;

// hands the renderer the shading; after the settings load, before boot
void Init();

}  // namespace fur
