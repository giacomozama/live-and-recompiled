#pragma once

// Glass (src/conker/common/glass.cpp): the program that draws rigid props by a sphere map of their normal, the beer bottles
// the soldiers hold in the menu's tavern, gets the renderer's gpu::ProgramShading.
namespace glass {

// settings.toml glass_shading: tinted props of that program shade as glass, what was drawn behind them seen through
// it, bent and tinted, darker and deeper in colour at the edges, a Fresnel reflection and the key light's sharp
// highlight. Read on each draw, so the F1 menu applies it live.
extern bool g_shading;

// hands the renderer the shading; after the settings load, before boot
void Init();

}  // namespace glass
