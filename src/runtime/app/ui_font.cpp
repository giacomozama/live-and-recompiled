// the overlay's font: Roboto Regular, at a size that follows the window's height
#include <algorithm>
#include <cmath>
#include <cstddef>

#include <imgui.h>

#include "app/app.h"

namespace app {

// the .ttf's bytes, generated from Roboto's release (cmake/XbeRecomp.cmake)
extern const unsigned char kUiFont[];
extern const size_t kUiFontSize;

namespace {

// a line of text a 67.5th of the window's height, 16 px at 1080p; never under ImGui's own 13
constexpr float kLinesPerHeight = 67.5f, kMinPx = 13.0f, kDefaultPx = 13.0f;

ImGuiStyle g_base;  // ImGui's style at its own 13 px font
float g_px = 0.0f;

}  // namespace

// ImGui rasterizes the font's glyphs as each size first needs them
void UiFont() {
    g_base = ImGui::GetStyle();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // the bytes are the executable's
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kUiFont), int(kUiFontSize), 0.0f, &cfg);
}

void UiFontResize() {
    const float px = std::max(std::round(ImGui::GetIO().DisplaySize.y / kLinesPerHeight), kMinPx);
    if (px == g_px) return;
    g_px = px;
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_base;
    style.ScaleAllSizes(px / kDefaultPx);
    style.FontSizeBase = px;
}

}  // namespace app
