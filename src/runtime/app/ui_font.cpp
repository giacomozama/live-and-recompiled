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

// flat, rounded and neutral: greys for everything at rest, the accent only where a control is selected, focused or held
static void Look() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowBorderSize = st.ChildBorderSize = st.PopupBorderSize = st.FrameBorderSize = st.TabBorderSize = 0.0f;
    st.WindowRounding = 16.0f;
    st.PopupRounding = 12.0f;
    st.ChildRounding = 10.0f;
    st.FrameRounding = 8.0f;
    st.GrabRounding = 8.0f;
    st.TabRounding = 8.0f;
    st.TabBarBorderSize = 0.0f;   // no line under the tabs
    st.TabBarOverlineSize = 0.0f;
    st.ScrollbarRounding = 12.0f;
    st.ScrollbarSize = 10.0f;
    st.WindowPadding = ImVec2(20.0f, 16.0f);
    st.SeparatorTextBorderSize = 1.0f;
    st.SeparatorTextPadding = ImVec2(10.0f, 4.0f);

    auto grey = [](float v, float a = 1.0f) { return ImVec4(v, v, v + 0.01f, a); };
    const ImVec4 accent(0.30f, 0.62f, 1.00f, 1.00f);
    auto tint = [&](float a) { return ImVec4(accent.x, accent.y, accent.z, a); };
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = grey(0.92f);
    c[ImGuiCol_TextDisabled] = grey(0.52f);
    c[ImGuiCol_WindowBg] = grey(0.075f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = grey(0.105f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = grey(0.14f);
    c[ImGuiCol_FrameBgHovered] = grey(0.185f);
    c[ImGuiCol_FrameBgActive] = grey(0.22f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_MenuBarBg] = grey(0.11f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = grey(0.28f);
    c[ImGuiCol_ScrollbarGrabHovered] = grey(0.38f);
    c[ImGuiCol_ScrollbarGrabActive] = grey(0.48f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = tint(0.85f);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = grey(0.16f);
    c[ImGuiCol_ButtonHovered] = grey(0.22f);
    c[ImGuiCol_ButtonActive] = tint(0.55f);
    c[ImGuiCol_Header] = tint(0.22f);
    c[ImGuiCol_HeaderHovered] = tint(0.32f);
    c[ImGuiCol_HeaderActive] = tint(0.45f);
    c[ImGuiCol_Separator] = grey(1.0f, 0.07f);
    c[ImGuiCol_SeparatorHovered] = tint(0.6f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = tint(0.5f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = ImVec4(0, 0, 0, 0);  // ui.cpp draws it, as the selected tab, with all four corners
    c[ImGuiCol_TabSelected] = ImVec4(0, 0, 0, 0);  // ui.cpp pills the selected tab: ImGui rounds a tab's top alone
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelectedOverline] = tint(0.5f);
    c[ImGuiCol_TextSelectedBg] = tint(0.35f);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_NavWindowingHighlight] = grey(1.0f, 0.5f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}

// ImGui rasterizes the font's glyphs as each size first needs them
void UiFont() {
    Look();
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
