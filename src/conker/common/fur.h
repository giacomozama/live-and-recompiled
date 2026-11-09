#pragma once

#include <cstdint>

// Fur shells (src/conker/common/fur.cpp): the vertex programs that draw a character's fur as shells pushed out along the
// normal, one draw per shell, get the renderer's gpu::ProgramShading; with fur_shading off too, for the depth only
// their opaque strands write (a depth of field then blurs the backdrop seen through their faint edges).
namespace fur {

// settings.toml fur_shading: the shells darken toward the roots and catch a highlight along the strands and a rim
// of light, and their cut out strands blend in at the edges instead of stepping. Read on each draw, so the F1 menu
// applies it live.
extern bool g_shading;

// settings.toml fur_strands, shown as Super fur: the shells' strands drawn procedurally rather than from the game's
// masks, fine, tapered and gathered in tufts, drooping, and lit as hair. Read on each draw. Each kind of fur has a look
// of its own (kLookNames, numbers fur_strand_* for the coat, fur_weasel_*, fur_chipmunk_*), read on each draw too.
extern bool g_strands;
struct Look {
    uint32_t strands;     // strands across one tile of the mask's coordinates
    uint32_t tufts;       // tufts across a tile
    uint32_t tufting;     // % how far a tuft's strands gather toward its middle at their tips
    uint32_t droop;       // % of the fur's length its tips hang down by
    uint32_t thickness;   // % of a strand's cell its root's radius takes
    uint32_t taper;       // % 100 narrows evenly to a point; lower keeps the strand wide to its tip
    uint32_t root_shade;  // % taken off at the roots
    uint32_t gap_shade;   // % taken off in the crevices between tufts
    uint32_t sheen;       // % the highlights along the strands
    uint32_t back_glow;   // % the tips glowing with the light behind them
    uint32_t length;      // % of the game's shells the strands reach; where a fur map is under half, shorter still
    // % bare skin grows fur anyway: where a fur map is under it, and a vertex shorter than this % of half the longest
    // (Berri's face in the Heist suit). The weasels' hands and feet and the chipmunks' clothes are other meshes, drawn
    // without shells, which this can't reach
    uint32_t fill;
};
enum : int { kCoat, kWeasel, kChipmunk, kLooks };
inline constexpr const char* kLookNames[kLooks] = {"Coat", "Weasels", "Chipmunks"};
// the coat: Conker, the squirrels (grey ones too) and most others; the weasels' program, with a fur map where the coat
// is; the chipmunks (Berri, the female bartender, the warboss's little girl, the SHC Sneeker; their programs the grey
// squirrels', told apart by TagShells): short, dense, untufted, unhanging velvet
inline constexpr Look kLookDefaults[kLooks] = {
    {20, 5, 70, 60, 75, 70, 45, 35, 50, 50, 100, 0},
    {20, 5, 70, 60, 75, 70, 45, 35, 50, 50, 100, 0},
    {36, 6, 20, 0, 70, 90, 25, 10, 30, 70, 70, 0},
};
extern Look g_looks[kLooks];
inline constexpr uint32_t kExtraShells = 2;
extern uint32_t g_extra_shells;  // shells added between each two of the game's, for every look

// settings.toml fur_distant: a furry object past the near range keeps all its shells, instead of losing them with
// distance as on the console until only the skin is left (src/conker/game/hooks.cpp). Read on each object.
extern bool g_distant;

// Who the fur shells drawn next belong to (src/conker/game/hooks.cpp, the object path 0x78460 and the player's
// 0x80358; players are objects too): its type [object+0x68], the byte at [object+0xA1], the level [0x7FA1F8], in
// the vertex constant kTagRegister, which no program of the game reads, for the shaders to pick a look by. F3
// captures list it with the constants that change (= c-94). Game thread
inline constexpr uint32_t kTagRegister = 2;  // hardware order: D3D c-94
void TagShells(uint32_t object);

// hands the renderer the shading; after the settings load, before boot
void Init();

}  // namespace fur
