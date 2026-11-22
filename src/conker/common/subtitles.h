#pragma once

#include <cstdint>

// Speech bubbles as subtitles (src/conker/game/subtitles.cpp): the game's bubbles are not drawn, and what each one
// says shows at the bottom of the screen instead, a piece at a time as the bubble would scroll to it.
namespace subtitles {

// settings.toml speech_subtitles (off by default). Read each frame, so the F1 menu applies it live
extern bool g_on;

// The bubbles drawn again (src/conker/game/bubbles.cpp), game thread: 0xE1880's hook around a bubble's drawing (the
// bubble, then 0 after), and ConkerTextQuad's for each glyph's quad
void BubbleDrawing(uint32_t obj);
void BubbleGlyph();

// from ui::Draw, render thread with app::UiLock held: the bubbles' current pieces
void Draw();

}  // namespace subtitles
