#pragma once
#include <string>
#include <vector>

// The game's two fonts drawn from fonts of the player's (src/conker/game/game_fonts.cpp): the frontend's title font
// (aid/font/FrontendTitle, the pub's picture labels and the menus' titles) and the text font (aid/font/ConkerFont,
// the menus, the HUD, subtitles), each from a TrueType or OpenType file the settings name, or the game's own. The
// glyphs keep the game's sizes (the font's capitals as tall as the game's) and take the font's spacing. The Vulkan
// renderer only; under the others the game's fonts stay. The Japanese fonts stay the game's.
namespace game_fonts {

enum Kind { kTitle, kText, kKinds };

// settings.toml title_font and text_font: the font file's path, "" the game's own. Set from the F1 menu, live: the
// game takes it the next time it picks the font. Any thread
std::string Path(Kind k);
void SetPath(Kind k, const std::string& path);
// a file the player picked: copied into ConfigDir/fonts (kept there under its name, numbered beside another of the
// same name) and that copy set, so the setting outlives the file being moved; Clear back to the game's own. A copy
// no setting names any more is deleted
void Choose(Kind k, const std::string& picked);
void Clear(Kind k);
std::string Name(Kind k);   // the font's name, as the file gives it ("" for the game's)
std::string Error(Kind k);  // why the file can't be used ("" when it can or none is set)
// the file's styles (a collection's faces, a variable font's named instances) by name, and the one in use; picking
// another sets its own weight again. settings.toml title_font_style / text_font_style: its face index
std::vector<std::string> Styles(Kind k);
int StyleIndex(Kind k);
void SetStyle(Kind k, int style);
// the style's weight axis (wght): its range, false without one; the weight drawn (the style's own until set), and
// whether one is set (0 the style's own). settings.toml title_font_weight / text_font_weight
bool WeightRange(Kind k, int& min, int& max);
int Weight(Kind k);
bool WeightSet(Kind k);
void SetWeight(Kind k, int weight);

void Load(const void* table);  // a toml::table
void Save(void* table);

// from 0x3D3D00, a string about to be drawn, and the hook before 0x3D3981, a glyph's quad about to be sent: the
// texture the glyph draws from, for the renderer (gpu::SetFontAtlas). Game thread
void StringStart();
void GlyphQuad();

}  // namespace game_fonts
