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
#include <utility>

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
uint32_t display::g_team_colors = display::kTeamGreenRed;

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

// a colour the game gives by team (red, green, blue): 0 a teammate's, 1 an opponent's, 2 a neutral's, -1 another
int TeamOf(const uint8_t* rgb) {
    static constexpr uint8_t kGame[3][3] = {{0x96, 0xFF, 0x96}, {0xFF, 0x96, 0x96}, {0x96, 0xC8, 0xFF}};
    for (int i = 0; i < 3; i++)
        if (std::memcmp(rgb, kGame[i], 3) == 0) return i;
    return -1;
}

// the team's colour in display::g_team_colors: the game's own, or blue, orange and light grey; solid for a marker
const uint8_t* TeamColour(int team, bool solid) {
    static constexpr uint8_t kColours[2][2][3][3] = {
        {{{0x96, 0xFF, 0x96}, {0xFF, 0x96, 0x96}, {0x96, 0xC8, 0xFF}},    // green / red, as the game draws them
         {{0x30, 0xFF, 0x30}, {0xFF, 0x30, 0x30}, {0x96, 0xC8, 0xFF}}},   // made solid
        {{{0x78, 0xBC, 0xFF}, {0xFF, 0xB8, 0x40}, {0xE0, 0xE0, 0xE0}},    // blue / orange
         {{0x28, 0x8C, 0xFF}, {0xFF, 0x94, 0x00}, {0xE0, 0xE0, 0xE0}}}};  // made solid
    const uint32_t set = std::min<uint32_t>(display::g_team_colors, display::kTeamBlueOrange);
    return kColours[set][solid ? 1 : 0][team];
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00348C30);
X86_FUNC(sub_00348C30) {
    const uint32_t site = kernel::R32(ctx.esp);
    const uint32_t mode = display::g_player_tags;
    const bool plain = site == kTagName || site == kWaitingName;
    if (mode == display::kTagOff && plain) {
        kernel::Return(ctx, ctx.eax, 0);  // cdecl: the caller pops its argument
        return;
    }
    const bool marker = mode == display::kTagMarkers && plain && MarkerText();
    // the tag's colour by team, as the game set it (0x347AE4 / 0x347BDB, a neutral's 0x347C94), in the chosen
    // colours; a marker's made solid. Another colour (a waiting player's, the game's other text) as it is
    uint8_t* colour = base + kTagColour;
    uint8_t was[3];
    std::memcpy(was, colour, 3);
    const int team = TeamOf(was);
    if (team >= 0) std::memcpy(colour, TeamColour(team, marker), 3);
    if (!marker) {
        __imp__sub_00348C30(ctx, base);
    } else {
        const uint32_t name = ctx.esi;
        ctx.esi = g_marker_text;
        g_marking = true;
        __imp__sub_00348C30(ctx, base);
        g_marking = false;
        ctx.esi = name;
    }
    std::memcpy(colour, was, 3);
}

// [[hook]] at 0x1D40A8 in 0x1D3CB0 (from the weapon code at 0x3012E5): the colour 0x3488E0 just picked for the
// target (red [esp+0x3B], green +0x3A, blue +0x39: 0x96 0xFF 0x96 a teammate, 0xFF 0x96 0x96 an opponent) in the chosen
// team colours
void ConkerTargetColour(X86Context& ctx, uint8_t* base) {
    if (display::g_team_colors == display::kTeamGreenRed) return;
    uint8_t* at = base + ctx.esp + 0x39;
    const uint8_t was[3] = {at[2], at[1], at[0]};
    const int team = TeamOf(was);
    if (team < 0) return;
    const uint8_t* c = TeamColour(team, false);
    at[2] = c[0], at[1] = c[1], at[0] = c[2];
}

// The team score (0x1CB330): each team's score by 0x1F25F0 in the text colour (0x77616F blue, 0x776170 green,
// 0x776171 red) the squirrels' green 0x64 0xFF 0x64 or the Tediz' red 0xFF 0x64 0x64, from these calls (their return
// addresses)
namespace {
constexpr uint32_t kTextColour = 0x77616F;
constexpr uint32_t kScoreTexts[] = {0x1CB43B, 0x1CB544, 0x1CB57D, 0x1CB5BE};
}  // namespace

display::TeamText display::TeamTextBegin(uint32_t site) {
    TeamText t;
    if (g_team_colors == kTeamGreenRed || std::find(std::begin(kScoreTexts), std::end(kScoreTexts), site) ==
                                             std::end(kScoreTexts))
        return t;
    uint8_t* c = mem::Base() + kTextColour;
    std::memcpy(t.was, c, 3);
    int team = -1;
    if (c[2] == 0x64 && c[1] == 0xFF && c[0] == 0x64) team = 0;
    else if (c[2] == 0xFF && c[1] == 0x64 && c[0] == 0x64) team = 1;
    if (team < 0) return t;
    const uint8_t* to = TeamColour(team, false);
    c[2] = to[0], c[1] = to[1], c[0] = to[2];
    t.changed = true;
    return t;
}

void display::TeamTextEnd(const TeamText& t) {
    if (t.changed) std::memcpy(mem::Base() + kTextColour, t.was, 3);
}

// The team icons (0x1CC500, eax the sprite's name, from the team score at 0x1CB670 / 0x1CB6A6 / 0x1CB6E0 and
// elsewhere): globaleffects\modeIcons\Squirrel_1 a flat green and Tediz_1 a flat red, the shape in their alpha, so a
// tint cannot turn them. In the other team colours each is drawn from a copy (made once a texture) whose colour
// endpoints (DXT) have their colour part (the most of red, green and blue less the least) in the team's colour
namespace {
int g_icon_team = -1;  // 0x1CC500 drawing a team's icon: 0 the squirrels', 1 the Tediz'

bool NameEnds(uint32_t name, const char* tail) {
    if (name < 0x10000) return false;
    uint32_t n = 0;
    while (n < 128 && kernel::G<char>(name + n)) n++;
    const size_t k = std::strlen(tail);
    if (n < k) return false;
    for (size_t i = 0; i < k; i++)
        if (kernel::G<char>(name + n - k + uint32_t(i)) != tail[i]) return false;
    return true;
}

uint16_t Recolour565(uint16_t c, int team) {
    const int r = (c >> 11 & 31) * 255 / 31, g = (c >> 5 & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    const int lo = std::min({r, g, b}), part = std::max({r, g, b}) - lo;
    const uint8_t* t = TeamColour(team, true);
    const int nr = lo + part * t[0] / 255, ng = lo + part * t[1] / 255, nb = lo + part * t[2] / 255;
    return uint16_t((nr * 31 + 127) / 255 << 11 | (ng * 63 + 127) / 255 << 5 | (nb * 31 + 127) / 255);
}

// the icon texture recoloured for the team, 0 if it is not DXT
uint32_t RecolouredIcon(uint32_t tex, int team) {
    const uint32_t data = kernel::R32(tex + 4), format = kernel::R32(tex + 12);
    const uint32_t kind = format >> 8 & 0xFF;  // 0x0C DXT1, 0x0E DXT2/3, 0x0F DXT4/5
    if (kind != 0x0C && kind != 0x0E && kind != 0x0F) return 0;
    struct Made {
        uint32_t data, format, tex;
    };
    static std::unordered_map<uint64_t, Made> made;  // (texture, team) -> its copy
    const uint64_t key = uint64_t(tex) << 1 | uint64_t(team);
    if (const auto it = made.find(key); it != made.end() && it->second.data == data && it->second.format == format)
        return it->second.tex;
    const uint32_t block = kind == 0x0C ? 8 : 16, colour = kind == 0x0C ? 0 : 8;
    const uint32_t levels = std::max<uint32_t>(format >> 16 & 15, 1), lw = format >> 20 & 15, lh = format >> 24 & 15;
    uint32_t bytes = 0;
    for (uint32_t l = 0; l < levels; l++)
        bytes += std::max(1u, (1u << lw >> l) / 4) * std::max(1u, (1u << lh >> l) / 4) * block;
    const uint32_t src = mem::VirtualFromPhysical(data & (mem::kPhysicalSize - 1));
    const uint32_t copy = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 0x1000);
    const uint32_t header = copy ? mem::AllocSystem(20) : 0;
    if (!src || !header) return 0;
    uint8_t* out = mem::Base() + copy;
    std::memcpy(out, mem::Base() + src, bytes);
    for (uint32_t at = colour; at + 4 <= bytes; at += block)
        for (uint32_t e = 0; e < 2; e++) {
            uint16_t c;
            std::memcpy(&c, out + at + 2 * e, 2);
            c = Recolour565(c, team);
            std::memcpy(out + at + 2 * e, &c, 2);
        }
    kernel::W32(header, 0x00040001);  // a texture, one reference
    kernel::W32(header + 4, mem::PhysicalFromVirtual(copy));
    kernel::W32(header + 8, 0);
    kernel::W32(header + 12, format);
    kernel::W32(header + 16, kernel::R32(tex + 16));
    made[key] = {data, format, header};
    LOGI("team colours: icon %08X (%ux%u, format %02X) drawn as %08X for team %d", tex, 1u << lw, 1u << lh, kind,
         header, team);
    return header;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001CC500);
X86_FUNC(sub_001CC500) {
    int team = -1;
    if (display::g_team_colors != display::kTeamGreenRed) {
        if (NameEnds(ctx.eax, "modeIcons\\Squirrel_1")) team = 0;
        else if (NameEnds(ctx.eax, "modeIcons\\Tediz_1")) team = 1;
    }
    const int was = std::exchange(g_icon_team, team);
    __imp__sub_001CC500(ctx, base);
    g_icon_team = was;
}

// The team's icon over a player (0x347330's tag, 0x357CF0 from 0x347E8A, its texture [esp+0x14]): its name, made
// at the tag's [esp+0xF0] (globaleffects\modeIcons\ and the team's), in the caller's frame past the six arguments
X86_EXTERN_FUNC(__imp__sub_00357CF0);
X86_FUNC(sub_00357CF0) {
    constexpr uint32_t kTagIcon = 0x347E8F, kName = 4 + 0x18 + 0xF0;
    if (display::g_team_colors != display::kTeamGreenRed && kernel::R32(ctx.esp) == kTagIcon) {
        const uint32_t name = ctx.esp + kName;
        const int team = NameEnds(name, "modeIcons\\Squirrel_1") ? 0 : NameEnds(name, "modeIcons\\Tediz_1") ? 1 : -1;
        const uint32_t tex = kernel::R32(ctx.esp + 0x14);
        if (team >= 0 && tex)
            if (const uint32_t mine = RecolouredIcon(tex, team)) kernel::W32(ctx.esp + 0x14, mine);
    }
    __imp__sub_00357CF0(ctx, base);
}

// [[hook]] at 0x1CC5FC in 0x1CC500, before its call of 0x1CAD90: the texture ([esp+0x10]) a team's icon is drawn with
void ConkerTeamIcon(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (g_icon_team < 0) return;
    const uint32_t tex = kernel::R32(ctx.esp + 0x10);
    if (!tex) return;
    if (const uint32_t mine = RecolouredIcon(tex, g_icon_team)) kernel::W32(ctx.esp + 0x10, mine);
}

// The flag held (0x1EC8F0, capture the flag): the squirrels' by 0x1EC7F0, its colour esi 0x64FF64 under the alpha,
// the Tediz' by 0x1EC870, 0xFF6464; a white sprite drawn in that colour (0x1EC750), its shadow by the alpha alone
// [[hook]] at 0x1EC84E and 0x1EC8CE, after each colour is made: in the chosen team colours
void ConkerFlagColour(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (display::g_team_colors == display::kTeamGreenRed) return;
    const uint32_t rgb = ctx.esi & 0xFFFFFF;
    const int team = rgb == 0x64FF64 ? 0 : rgb == 0xFF6464 ? 1 : -1;
    if (team < 0) return;
    const uint8_t* c = TeamColour(team, false);
    ctx.esi = (ctx.esi & 0xFF000000) | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
}

// The flag over a player carrying it (0x347330's tag, 0x357FB0 from 0x348281, the white sprite 0x2C7): its colour
// floats by the carrier's team (0x3481D9), red [esp+0x78], green [esp+0x80], blue [esp+0x74]: 0.39 1 0.39 a
// squirrel's, 1 0.39 0.39 a Tediz'.
// [[hook]] at 0x34822F, where the teams' colours meet: in the chosen team colours
void ConkerCarrierFlag(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (display::g_team_colors == display::kTeamGreenRed) return;
    const uint32_t r = ctx.esp + 0x78, g = ctx.esp + 0x80, b = ctx.esp + 0x74;
    const auto near = [](float v, float to) { return std::fabs(v - to) < 0.02f; };
    int team = -1;
    if (near(GetF(r), 0.392f) && near(GetF(g), 1.0f) && near(GetF(b), 0.392f)) team = 0;
    else if (near(GetF(r), 1.0f) && near(GetF(g), 0.392f) && near(GetF(b), 0.392f)) team = 1;
    if (team < 0) return;
    const uint8_t* c = TeamColour(team, false);
    SetF(r, c[0] / 255.0f), SetF(g, c[1] / 255.0f), SetF(b, c[2] / 255.0f);
}

// The message log (kills, the flag taken; 0x1ECF40 draws it by 0x1ECEB0, whose lines take their colour (0x1ECDEB,
// 0x1ECE68) from a table the caller builds on its stack, three bytes red green blue an entry: 0 the HUD's blue, 1
// purple, 2 green 0x00 0xE1 0x0A, 3 red 0xFF 0x32 0x32, 4 white, 5 amber 0xF0 0xB9 0x32, 6 a dim green 0x32 0xB9 0x32.
// [[hook]] at 0x1ED983, the call made (two arguments pushed, the table at [esp+0x4C]): its green and red in the chosen
// team colours, the dim green a dim blue
void ConkerMessageColours(X86Context& ctx, uint8_t* base) {
    if (display::g_team_colors == display::kTeamGreenRed) return;
    uint8_t* table = base + ctx.esp + 0x4C;
    static constexpr uint8_t kGreen[3] = {0x00, 0xE1, 0x0A}, kRed[3] = {0xFF, 0x32, 0x32}, kDim[3] = {0x32, 0xB9, 0x32};
    static constexpr uint8_t kDimBlue[3] = {0x46, 0x8C, 0xD2};
    if (std::memcmp(table + 6, kGreen, 3) == 0) std::memcpy(table + 6, TeamColour(0, true), 3);
    if (std::memcmp(table + 9, kRed, 3) == 0) std::memcpy(table + 9, TeamColour(1, true), 3);
    if (std::memcmp(table + 18, kDim, 3) == 0) std::memcpy(table + 18, kDimBlue, 3);
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
