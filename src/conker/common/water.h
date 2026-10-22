#pragma once

// Water (src/conker/common/water.cpp): the vertex program that draws rivers and ponds over a copy of the screen gets the
// renderer's gpu::ProgramShading.
namespace water {

// settings.toml dynamic_water: the surface flows with the current and ripples where drops fall and bodies wade,
// bending what shows through it and catching the light, instead of lying flat as on the console. Read on each draw, so
// the F1 menu applies it live.
extern bool g_dynamic;

// hands the renderer the water's shading; after the settings load, before boot
void Init();

}  // namespace water
