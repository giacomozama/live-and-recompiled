#pragma once

#include <cstdint>

struct X86Context;

// HUD art redrawn as vectors (src/conker/common/hud_art.cpp): each picture, rasterized from its SVG by
// tools/hud_art.py, drawn by the renderer in place of the game's texture of the same name (gpu::Replacement). Vulkan
// only.
namespace hud_art {

// settings.toml redrawn_hud (on by default): the redrawn pictures in place of the game's textures. Read each frame, so
// the F1 menu applies it live
extern bool g_on;

// hands the renderer the pictures; before boot
void Init();
// game thread, once a frame: finds the textures the game has loaded that have a picture, now and then
void Poll();
// before a DrawVerticesUP of a quad (its arguments at [esp]): a texture with a picture of more than its square drawn
// larger, with that picture, and kept to its own shape on a widescreen display. uv_offset: where the vertex's texture
// coordinates are, -1 its last 8 bytes
void QuadDraw(X86Context& ctx, int uv_offset);
// 0x1CAD90 about to draw its quad through the push buffer (its texture set, the corners' pointers at [esp+0xC]..
// [esp+0x18], the texture at [esp+0x1C]): a texture with a picture of more than its square drawn larger, the corners
// moved out by the picture's margins and the stand-in texture set. Returns the game's texture to set back once the
// quad is drawn, 0 if nothing changed
uint32_t HudQuad(X86Context& ctx);

}  // namespace hud_art
