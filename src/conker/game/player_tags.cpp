// What a match shows over the other players (settings.toml player_tags, display::g_player_tags).
//
// Over another player the game draws a tag (0x347330, from 0x2A5D48 once a frame for each player's view, while
// [0x7819D5], a match, is set; its loop over the players): their name, coloured by 0x3488E0 (the player, the viewer)
// green for a teammate and red for an opponent (the colour at 0x800EF4..6: 0x96 0xFF 0x96 or 0xFF 0x96 0x96; a
// neutral's light blue 0x96 0xC8 0xFF), at times with icons over it (the team's, Repair_1, UPgrade_1, by 0x357CF0).
// The name is drawn by 0x348C30 (esi the string, edi its place, which it centres on its x by the string's width,
// [esp+4] its y; the colour copied from 0x800EF4..6, the alpha the tag's fade left at 0x807667, the size at
// 0x80765C), from 0x347FDF, or from 0x3472F6 in 0x347090, which a waiting player's tag goes through: the name with
// the time to the respawn (0x3471B5) and the pulsing one (0x3472B0) stay as they are.
//
// Off, the name's call is skipped (it raises and lowers [0x8072F0] itself, 0x3D30A6 / 0x348D18). Markers draw the
// glyph I in the name's place, in the tag's colour made solid, and its quad, as 0x3D3D00 sends it (PlayerMarkerQuad),
// becomes a triangle pointing down: the four texture coordinates at the glyph's middle (on I's stem), the top corners
// 0.9 of the glyph's height apart, the bottom two met under the middle. So the marker moves, fades and scales with the
// tag as the game places it, and goes through the frame's 2D as the name did.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "common/display.h"
#include "common/menus.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

uint32_t display::g_player_tags = display::kTagNames;

namespace {

constexpr uint32_t kTagName = 0x347FE4, kWaitingName = 0x3472FB;  // 0x348C30's return addresses for a plain name
constexpr uint32_t kTagColour = 0x800EF4;                          // the text's red, green, blue
constexpr uint32_t kQuad = 0x807230, kVertex = 0x1C;               // a glyph's quad: x y z rhw, diffuse, u v

bool g_marking = false;    // a marker's glyph is drawing, game thread
uint32_t g_marker_text = 0;  // the guest string L"I"

uint32_t MarkerText() {
    if (!g_marker_text) {
        const uint32_t va = mem::AllocSystem(4);
        if (!va) return 0;
        const char16_t text[2] = {u'I', 0};
        std::memcpy(mem::Base() + va, text, sizeof text);
        g_marker_text = va;
    }
    return g_marker_text;
}

float GetF(uint32_t va) {
    float f;
    std::memcpy(&f, mem::Base() + va, 4);
    return f;
}

void SetF(uint32_t va, float f) { std::memcpy(mem::Base() + va, &f, 4); }

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00348C30);
X86_FUNC(sub_00348C30) {
    const uint32_t site = kernel::R32(ctx.esp);
    const uint32_t mode = display::g_player_tags;
    if (mode == display::kTagNames || (site != kTagName && site != kWaitingName)) return __imp__sub_00348C30(ctx, base);
    if (mode != display::kTagMarkers) {
        kernel::Return(ctx, ctx.eax, 0);  // cdecl: the caller pops its argument
        return;
    }
    const uint32_t text = MarkerText();
    if (!text) return __imp__sub_00348C30(ctx, base);
    // the tag's green or red made solid; another colour (a neutral's) as it is
    uint8_t* colour = base + kTagColour;
    uint8_t was[3];
    std::memcpy(was, colour, 3);
    if (was[0] == 0x96 && was[1] == 0xFF && was[2] == 0x96) colour[0] = 0x30, colour[2] = 0x30;
    else if (was[0] == 0xFF && was[1] == 0x96 && was[2] == 0x96) colour[1] = 0x30, colour[2] = 0x30;
    const uint32_t name = ctx.esi;
    ctx.esi = text;
    g_marking = true;
    __imp__sub_00348C30(ctx, base);
    g_marking = false;
    ctx.esi = name;
    std::memcpy(colour, was, 3);
}

void display::PlayerMarkerQuad() {
    if (!g_marking) return;
    float sx[4], sy[4];
    if (!menus::GlyphScreen(sx, sy)) return;
    // the corners by where they land: the top two, then the bottom two, each left then right
    int at[4] = {0, 1, 2, 3};
    std::sort(at, at + 4, [&](int a, int b) { return sy[a] < sy[b]; });
    if (sx[at[0]] > sx[at[1]]) std::swap(at[0], at[1]);
    if (sx[at[2]] > sx[at[3]]) std::swap(at[2], at[3]);
    const uint32_t tl = kQuad + uint32_t(at[0]) * kVertex, tr = kQuad + uint32_t(at[1]) * kVertex;
    const uint32_t bl = kQuad + uint32_t(at[2]) * kVertex, br = kQuad + uint32_t(at[3]) * kVertex;
    // in the quad's own units (its x and y as the screen's, each maybe flipped)
    const float top = (GetF(tl + 4) + GetF(tr + 4)) * 0.5f, bottom = (GetF(bl + 4) + GetF(br + 4)) * 0.5f;
    const float h = bottom - top;
    const float mid = (GetF(tl) + GetF(tr) + GetF(bl) + GetF(br)) * 0.25f;
    const float half = 0.45f * std::fabs(h) * (GetF(tr) >= GetF(tl) ? 1.0f : -1.0f);
    SetF(tl, mid - half), SetF(tl + 4, top);
    SetF(tr, mid + half), SetF(tr + 4, top);
    SetF(bl, mid), SetF(bl + 4, top + 0.8f * h);
    SetF(br, mid), SetF(br + 4, top + 0.8f * h);
    // every corner samples the glyph's middle
    float u = 0, v = 0;
    for (uint32_t i = 0; i < 4; i++) u += GetF(kQuad + i * kVertex + 0x14), v += GetF(kQuad + i * kVertex + 0x18);
    for (uint32_t i = 0; i < 4; i++) SetF(kQuad + i * kVertex + 0x14, u * 0.25f), SetF(kQuad + i * kVertex + 0x18, v * 0.25f);
}
