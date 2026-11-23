#pragma once

#include <cstdint>

struct X86Context;

// Speech bubbles as subtitles (src/conker/game/subtitles.cpp): the game's bubbles are not drawn, and what each one
// says shows at the bottom of the screen instead, a piece at a time as the bubble would scroll to it.
namespace subtitles {

// settings.toml speech_subtitles (off by default). Read each frame, so the F1 menu applies it live
extern bool g_on;

// settings.toml redrawn_bubbles (on by default; the F1 menu turns it off with subtitles on, which come first anyway):
// the bubbles drawn again by the runtime (bubbles.cpp), else the game's own. Read as each bubble draws
extern bool g_redraw;

// The bubbles drawn again (src/conker/game/bubbles.cpp), game thread: 0xE1880's hook around a bubble's drawing (the
// bubble, then 0 after; BubbleMeasure between the first and the drawing), and ConkerTextQuad's for each glyph's quad
void BubbleDrawing(uint32_t obj);
void BubbleMeasure(X86Context& ctx);
void BubbleGlyph();
// a key-paced bubble (a hint: shown until the player presses on) is placed by the game's own drawing with nothing
// drawn: BubblePlacing(obj) around 0xE1880, then BubblePlaced gives its body's box at full scale (the middle, the
// width and height, on the 640 by 480 frame), false when the body wasn't placed
void BubblePlacing(uint32_t obj);
bool BubblePlaced(float& x, float& y, float& w, float& h);

// from ui::Draw, render thread with app::UiLock held: the bubbles' current pieces, and a hint's page whole in a panel
void Draw();

}  // namespace subtitles
