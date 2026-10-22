#pragma once

// Metal (src/conker/common/metal.cpp): the character programs that light a material by the character's lighting map too, the
// game's shiny parts (the goblins' armour), get the renderer's gpu::ProgramShading.
namespace metal {

// settings.toml metal_shading: grey, bright texels of those materials shade as metal, with a key light highlight, a
// reflection of the character's lights that turns with the view and dents raised from the texture. Read on each
// draw, so the F1 menu applies it live.
extern bool g_shading;

// hands the renderer the shading; after the settings load, before boot
void Init();

}  // namespace metal
