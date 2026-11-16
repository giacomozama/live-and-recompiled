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
//
// Outlines (settings.toml player_outlines, gpu::g_outlines): each view's frame (the function round 0x2A5C40, bp the view) draws the
// players by 0x7F4D0 (eax the view, [esp+4] the player's index into the records at [0x84A5F8], 0x6D0 bytes each,
// [esp+8] a kind) from its loop at 0x2A5CA1 (the records are every character-like object, +0x64 their kind: only
// kind 1, the players and bots, is outlined; upgrades and vehicles are other kinds, on team 2); in a match they come from 0x7E39B (a list's entry), 0x7E406 and 0x7E77A
// (logged with XBE_OUTLINE_TRACE=1). Each such draw carries an outline mark (d3d::OutlineMark): 1 for a teammate of the view's
// player, 2 for an opponent, by the game's own test 0x3488E0 (player, viewer), as the tag's colours are; none for the
// view's player itself. The view's player is in the camera table at [0x8470DC] (four entries of 0x770 bytes: the
// player at +0x3E0, the view at the byte +0x2A8), as the tag finds it (0x347465).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "common/display.h"
#include "common/menus.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "core/log.h"
#include "gpu/shaders.h"
#include "xdk/d3d8.h"

uint32_t display::g_player_tags = display::kTagNames;
bool display::g_player_outlines = false;

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

namespace {

constexpr uint32_t kPlayers = 0x84A5F8, kPlayerSize = 0x6D0, kPlayerKind = 0x64, kKindPlayer = 1;
constexpr uint32_t kCameras = 0x8470DC, kCameraSize = 0x770, kCameraPlayer = 0x3E0, kCameraView = 0x2A8;

// XBE_OUTLINE_TRACE=1: a line each time a view's player's outline changes (the viewer found, the mark)
bool OutlineTrace() {
    static const bool on = std::getenv("XBE_OUTLINE_TRACE") != nullptr;
    return on;
}
void TraceOutline(uint32_t site, uint32_t kind, uint32_t view, uint32_t index, uint32_t player, uint32_t viewer,
                  int mark) {
    if (!OutlineTrace()) return;
    static std::unordered_map<uint64_t, uint64_t> last;  // (call, kind, view, index) -> (player, viewer, mark)
    static uint32_t lines = 0;
    const uint64_t key = uint64_t(site) << 40 ^ uint64_t(kind & 0xFF) << 32 ^ uint64_t(view) << 16 ^ index;
    const uint64_t now = uint64_t(player) << 32 ^ uint64_t(viewer) << 4 ^ uint64_t(mark + 1);
    const auto it = last.find(key);
    if ((it != last.end() && it->second == now) || lines >= 300) return;
    last[key] = now;
    lines++;
    LOGR("outlines: from %06X kind %u, view %u draws player %u (%08X), the view's player %08X: %s", site, kind, view,
         index, player, viewer,
         mark < 0 ? "none (no viewer, itself, or not a player)" : mark == 1 ? "teammate, green"
         : mark == 2 ? "opponent, red" : "neutral vehicle, light blue");
}

// a vehicle: its model (+0x68, an index into the model names at 0x6A58F0) under MultiVehicles\, by model
constexpr uint32_t kPlayerModel = 0x68, kModelNames = 0x6A58F0, kModels = 0x205, kPlayerTeam = 0x238, kNeutral = 2;
bool Vehicle(uint32_t model) {
    static std::unordered_map<uint32_t, bool> known;
    if (const auto it = known.find(model); it != known.end()) return it->second;
    bool vehicle = false;
    const uint32_t name = model < kModels ? kernel::R32(kModelNames + 4 * model) : 0;
    if (name >= 0x10000) {
        static constexpr char kPrefix[] = "MultiVehicles\\";
        vehicle = true;
        for (uint32_t i = 0; kPrefix[i]; i++)
            if (kernel::G<char>(name + i) != kPrefix[i]) vehicle = false;
    }
    return known[model] = vehicle;
}

// the view's player, 0 if none
uint32_t Viewer(uint32_t view) {
    const uint32_t table = kernel::R32(kCameras);
    if (!table) return 0;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t cam = table + i * kCameraSize;
        const uint32_t player = kernel::R32(cam + kCameraPlayer);
        if (player && kernel::G<uint8_t>(cam + kCameraView) == view) return player;
    }
    return 0;
}

}  // namespace

X86_EXTERN_FUNC(sub_003488E0);
X86_EXTERN_FUNC(__imp__sub_0007F4D0);
X86_FUNC(sub_0007F4D0) {
    if (!gpu::g_outlines.load(std::memory_order_relaxed)) return __imp__sub_0007F4D0(ctx, base);
    const uint32_t site = kernel::R32(ctx.esp), kind = kernel::R32(ctx.esp + 8);
    const uint32_t view = uint32_t(int16_t(ctx.eax));
    const uint32_t index = kernel::R32(ctx.esp + 4);
    const uint32_t players = kernel::R32(kPlayers);
    const uint32_t player = players ? players + index * kPlayerSize : 0;
    const uint32_t viewer = Viewer(view);
    // the records hold more than the players (+0x64 their kind: 1 a player or bot; upgrades, barrels, sentry guns,
    // vehicles and the like other kinds): the players are outlined, and the vehicles (a neutral one, team 2, light
    // blue), nothing else
    const bool vehicle = player && kernel::R32(player + kPlayerKind) != kKindPlayer &&
                         Vehicle(kernel::R32(player + kPlayerModel));
    if (!player || !viewer || player == viewer || (kernel::R32(player + kPlayerKind) != kKindPlayer && !vehicle)) {
        TraceOutline(site, kind, view, index, player, viewer, -1);
        return __imp__sub_0007F4D0(ctx, base);
    }
    const uint32_t team = kernel::R32(player + kPlayerTeam) ? kernel::R32(kernel::R32(player + kPlayerTeam)) : 0;
    if (vehicle && team == kNeutral) {
        TraceOutline(site, kind, view, index, player, viewer, 3);
        d3d::OutlineMark(3);
        __imp__sub_0007F4D0(ctx, base);
        d3d::OutlineMark(0);
        return;
    }
    // the game's test, cdecl (player, viewer): al set for a teammate
    const X86Context saved = ctx;
    X86_PUSH32(viewer);
    X86_PUSH32(player);
    X86_PUSH32(site);
    sub_003488E0(ctx, base);
    const bool teammate = (ctx.eax & 0xFF) != 0;
    ctx = saved;
    TraceOutline(site, kind, view, index, player, viewer, teammate ? 1 : 2);
    d3d::OutlineMark(teammate ? 1 : 2);
    __imp__sub_0007F4D0(ctx, base);
    d3d::OutlineMark(0);
}
