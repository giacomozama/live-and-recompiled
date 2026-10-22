// the overlay's font: Roboto Regular, rasterized again at a size that follows the window's height
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

void UiFont() { g_base = ImGui::GetStyle(); }

bool UiFontResize() {
    ImGuiIO& io = ImGui::GetIO();
    const float px = std::max(std::round(io.DisplaySize.y / kLinesPerHeight), kMinPx);
    if (px == g_px) return false;
    g_px = px;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // the bytes are the executable's
    io.Fonts->Clear();
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kUiFont), int(kUiFontSize), px, &cfg);
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_base;
    style.ScaleAllSizes(px / kDefaultPx);
    return true;
}

}  // namespace app
