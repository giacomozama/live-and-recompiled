#pragma once

// The game's two fonts drawn from TrueType fonts (src/conker/game/game_fonts.cpp): the frontend's title font
// (aid/font/FrontendTitle, the pub's picture labels and the menus' titles) from Reloaded Serif Italic, and the text font
// (aid/font/ConkerFont, the menus, the HUD, subtitles) from Trebuchet MS Bold. The glyphs keep the game's sizes (the
// font's capitals as tall as the game's) and take the font's spacing. The Vulkan renderer only; under the others the
// game's fonts stay. The Japanese fonts stay the game's.
namespace game_fonts {

// settings.toml hq_title_font and hq_text_font (both on by default). The F1 menu applies them live: the game takes
// them the next time it picks the font
extern bool g_title, g_text;

// whether Trebuchet MS Bold is there: without it the text font stays the game's
bool TextInstalled();

// from 0x3D3D00, a string about to be drawn, and the hook before 0x3D3981, a glyph's quad about to be sent: the
// texture the glyph draws from, for the renderer (gpu::SetFontAtlas). Game thread
void StringStart();
void GlyphQuad();

}  // namespace game_fonts
