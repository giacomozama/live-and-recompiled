#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Fur shells (src/conker/common/fur.cpp): the vertex programs that draw a character's fur as shells pushed out along the
// normal, one draw per shell, get the renderer's gpu::ProgramShading; with fur_shading off too, for the depth only
// their opaque strands write (a depth of field then blurs the backdrop seen through their faint edges).
namespace fur {

// settings.toml fur_shading: the shells darken toward the roots and catch a highlight along the strands and a rim
// of light, and their cut out strands blend in at the edges instead of stepping. Read on each draw, so the F1 menu
// applies it live.
extern bool g_shading;

// settings.toml fur_strands, shown as Super fur: the shells' strands drawn procedurally rather than from the game's
// masks, fine, tapered and gathered in tufts, drooping, and lit as hair. Read on each draw. Each model has a look of
// its own (ModelLook), read on each draw too.
extern bool g_strands;
inline constexpr uint32_t kMaxShade = 20;
struct Look {
    uint32_t strands;     // strands across one tile of the mask's coordinates
    uint32_t tufts;       // tufts across a tile
    uint32_t tufting;     // % how far a tuft's strands gather toward its middle at their tips
    uint32_t droop;       // % of the fur's length its tips hang down by
    uint32_t thickness;   // % of a strand's cell its root's radius takes
    uint32_t taper;       // % 100 narrows evenly to a point; lower keeps the strand wide to its tip
    uint32_t root_shade;  // % taken off at the roots (at most kMaxShade)
    uint32_t gap_shade;   // % taken off in the crevices between tufts (at most kMaxShade)
    uint32_t sheen;       // % the highlights along the strands
    uint32_t back_glow;   // % the tips glowing with the light behind them
    uint32_t length;      // % of the game's shells the strands reach; where a fur map is under half, shorter still
    // % bare skin grows fur anyway: where a fur map is under it, and a vertex shorter than this % of half the longest
    // (Berri's face in the Heist suit). The weasels' hands and feet and the chipmunks' clothes are other meshes, drawn
    // without shells, which this can't reach
    uint32_t fill;
    bool operator==(const Look&) const = default;
};
// A model's look starts from its kind's: the coat (Conker, the squirrels, grey ones too, and most others); the
// weasels' (their program, with a fur map where the coat is); the chipmunks' (Berri, the female bartender, the
// warboss's little girl, the SHC Sneeker; their programs the grey squirrels', told apart by TagShells): short, dense,
// untufted, unhanging velvet
enum : int { kCoat, kWeasel, kChipmunk, kLooks };
inline constexpr Look kLookDefaults[kLooks] = {
    {20, 5, 70, 60, 75, 70, 5, 5, 50, 50, 100, 0},
    {20, 5, 70, 60, 75, 70, 5, 5, 50, 50, 100, 0},
    {36, 6, 20, 0, 70, 90, 5, 5, 30, 70, 70, 0},
};

// Each model's look: by the type TagShells tags its shells with ([object+0x68], the model's id; its name at
// 0x6A58F0). settings.toml [fur_models.<type>], the Look's keys, and game = the game's own fur for it; only those
// changed from their defaults (DefaultLook, GameFurOf) are saved. The renderer reads them per draw; a model without
// one is drawn with its defaults. kModelLooks at most; a type of kNoModel is a free slot (Conker is 0). The F1 menu changes them live
inline constexpr uint32_t kNoModel = ~0u;
struct ModelLook {
    uint32_t type = kNoModel;
    bool game = false;  // drawn as the game draws it, super fur or not
    Look look{};
};
inline constexpr int kModelLooks = 64;
extern ModelLook g_model_looks[kModelLooks];
ModelLook* FindModelLook(uint32_t type);
// the model's look, made from its defaults if it had none; null when the slots are full. Not the render thread
ModelLook* ModelLookOf(uint32_t type);
// a model's defaults: its kind's look (kCoat, kWeasel, kChipmunk), and whether it keeps the game's own fur
int KindOf(uint32_t type);
// A model's hair, where it has a piece of hair drawn with the fur's shells (fur.cpp kHairPieces: Van Helsing's), is
// listed beside the model under HairOf(model), its look's game alone used: the hair keeps the game's own fur while the
// rest of the model has super fur. Otherwise the hair takes the model's look
inline constexpr uint32_t kHairBit = 0x10000;
inline constexpr uint32_t HairOf(uint32_t model) { return kHairBit | model; }
inline constexpr bool IsHair(uint32_t type) { return type != ~0u && (type & kHairBit); }
// models with defaults of their own rather than their kind's, by type (the model table's id), tuned one by one; the
// renderer draws them with these wherever the menu has no look for them, whatever program draws them. Keys as Look:
// strands, tufts, tufting, droop, thickness, taper, root shade, tuft shade, sheen, back glow, length, fill
struct ModelDefault {
    uint32_t type;
    Look look;
};
inline constexpr ModelDefault kModelDefaults[] = {
    {17, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},   // WeaselMinion1
    {20, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},   // WeaselMinion2
    {59, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},   // Weasel
    {134, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},  // WeaselSuit
    {152, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},  // WeaselSwat
    {153, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},  // WeaselScientist
    {183, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},  // WeaselBass
    {184, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},  // WeaselClarinet
    {185, {24, 5, 30, 120, 65, 35, 0, 0, 50, 50, 100, 40}},  // WeaselDrum
    {88, {32, 10, 70, 100, 75, 120, 2, 2, 50, 50, 100, 0}},  // CombatSquirrel, and CombatSquirrelShot (fur.cpp kSharedLooks)
    {7, {16, 16, 100, 100, 75, 110, 0, 0, 50, 80, 100, 0}},    // QueenBee
    {109, {16, 12, 80, 100, 75, 110, 0, 0, 100, 50, 100, 100}},  // BeeKing
    {114, {24, 9, 100, 120, 70, 110, 0, 0, 60, 140, 100, 20}},   // Berri
    {398, {12, 7, 100, 200, 70, 110, 0, 0, 75, 140, 100, 20}},   // BerriBarmaid
    {151, {24, 16, 100, 200, 100, 120, 0, 0, 30, 70, 100, 40}},  // BeriMatrix
    {162, {16, 7, 60, 120, 75, 70, 4, 2, 30, 50, 100, 0}},       // BatBoss
    {164, {24, 16, 90, 120, 75, 110, 0, 0, 30, 50, 100, 0}},     // BatBossStoker
    {181, {12, 12, 70, 110, 75, 110, 2, 2, 70, 50, 100, 100}},   // Barman
};
inline const ModelDefault* FindModelDefault(uint32_t type) {
    for (const ModelDefault& d : kModelDefaults)
        if (d.type == type) return &d;
    return nullptr;
}
inline const Look& DefaultLook(uint32_t type) {
    if (const ModelDefault* d = FindModelDefault(type)) return d->look;
    return kLookDefaults[KindOf(type & ~kHairBit)];
}
bool GameFurOf(uint32_t type);
// the model whose look a model takes: a multiplayer class's models share one (Multi\, MultiFrontend\, MultiLOD\,
// MultiLOD2\, MainFrontend\), its full model's id; Conker's (all but Van Helsing's), 0; a few story models another's
// (fur.cpp kSharedLooks: CombatSquirrelShot CombatSquirrel's); any other model its own. Any thread
uint32_t Canonical(uint32_t type);
// the types whose shells super fur drew, this session or before (settings.toml fur_seen), oldest first
std::vector<uint32_t> SeenModels();
// the furry models the game's model table has now (fur.cpp kFurryModels, from a soak test survey; multiplayer's
// squirrels and furry Tediz by name).
// XBE_FUR_SURVEY=1 logs each model's first draw with super fur, its type and name, to add to them. UI thread
std::vector<uint32_t> KnownModels();
// a model super fur has no business with, listed or not: the WWII Tediz classes but the grunt, which have no fur (a
// shell drawn under their tag, or a look saved for them, came from another model)
bool Furless(uint32_t type);
// the game's image is in memory: the model table can be read (app::GameInfo::loaded)
void ImageLoaded();
// the model's name in the game's model table and its type, "Berri (114)"; a multiplayer class's, "CombatSquirrelMultiGrunt (multiplayer)", "Model (469)" when the table has none
std::string ModelName(uint32_t type);
// the [fur_models] table of settings.toml (a toml::table)
void Load(const void* table);
void Save(void* table);

inline constexpr uint32_t kExtraShells = 1;
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
