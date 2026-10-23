#pragma once

// Fur shells (src/conker/common/fur.cpp): the vertex programs that draw a character's fur as shells pushed out along the
// normal, one draw per shell, get the renderer's gpu::ProgramShading; with fur_shading off too, for the depth only
// their opaque strands write (a depth of field then blurs the backdrop seen through their faint edges).
namespace fur {

// settings.toml fur_shading: the shells darken toward the roots and catch a highlight along the strands and a rim
// of light, and their cut out strands blend in at the edges instead of stepping. Read on each draw, so the F1 menu
// applies it live.
extern bool g_shading;

// settings.toml fur_distant: a furry object past the near range keeps all its shells, instead of losing them with
// distance as on the console until only the skin is left (src/conker/game/hooks.cpp). Read on each object.
extern bool g_distant;

// hands the renderer the shading; after the settings load, before boot
void Init();

}  // namespace fur
