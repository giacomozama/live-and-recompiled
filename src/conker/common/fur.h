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
    // % the strands' color before their highlights, against what the game's grey masks leave of the skin under them
    // (some 70 to 90 % of it): above 100 to bring the coat up to the skin's tone
    uint32_t tone = 100;
    uint32_t sway = 0;      // a hair's (HairOf): % how far it sways in the air, its tips most
    uint32_t motion = 100;  // a hair's: % how far it trails behind the character's movement and turns, and swings back
    // strays: % of the strands (one in so many, on a grid of their own) longer than the coat, each leaning its own
    // way, frizz or straws of hay peeking out; 0 none. The renderer draws shells past the game's outermost for them
    // where they reach past it (fur.cpp StrandsBeyond), none without strays
    uint32_t stray = 0;
    uint32_t stray_length = 150;  // % of the coat's length the longest strays reach (each 60 to 100 % of the way there)
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
// shells added between each two of the game's, a model's own (settings.toml [fur_models.<type>] shells)
inline constexpr uint32_t kExtraShells = 1;
inline constexpr uint32_t kMaxExtraShells = 4;
struct ModelLook {
    uint32_t type = kNoModel;
    bool game = false;  // drawn as the game draws it, super fur or not; no extra shells then
    Look look{};
    uint32_t shells = kExtraShells;
};
inline constexpr int kModelLooks = 64;
extern ModelLook g_model_looks[kModelLooks];
ModelLook* FindModelLook(uint32_t type);
// the model's look, made from its defaults if it had none; null when the slots are full. Not the render thread
ModelLook* ModelLookOf(uint32_t type);
// a model's defaults: its kind's look (kCoat, kWeasel, kChipmunk), and whether it keeps the game's own fur
int KindOf(uint32_t type);
// A model's hair, where it has a piece of hair drawn with the fur's shells (fur.cpp kHairPieces: Van Helsing's), is
// listed beside the model under HairOf(model): the hair keeps the game's own fur, or takes a look of its own, while
// the rest of the model has its own. Its draws are told from the model's others by their shells' offset per layer,
// for its look and extra shells; until the menu makes it a look it takes the model's. It moves whichever fur it has:
// sways, and trails behind the character (kMotionRegister), by its sway and motion
inline constexpr uint32_t kHairBit = 0x10000;
inline constexpr uint32_t kHairSway = 40;  // a hair's sway by default
inline constexpr uint32_t HairOf(uint32_t model) { return kHairBit | model; }
inline constexpr bool IsHair(uint32_t type) { return type != ~0u && (type & kHairBit); }
// Conker's crown rim in the intro, a look of its own: its shells' draws carry no tag of theirs (the game tags none
// on that path; they'd take whichever model drew before), so its program's entry (fur.cpp kRigidMatch) tags them
// with this, an id past the model table's and the hairs'
inline constexpr uint32_t kCrown = 0x20000;
// models with defaults of their own rather than their kind's, by type (the model table's id), tuned one by one; the
// renderer draws them with these wherever the menu has no look for them, whatever program draws them. Keys as Look:
// strands, tufts, tufting, droop, thickness, taper, root shade, tuft shade, sheen, back glow, length, fill, tone (100
// when left out)
struct ModelDefault {
    uint32_t type;
    Look look;
    uint32_t shells = kExtraShells;  // extra shells, ModelLook's
};
inline constexpr ModelDefault kModelDefaults[] = {
    {0, {55, 7, 60, 60, 75, 120, 0, 8, 50, 150, 100, 0}},     // Conker, all his models but Van Helsing's and the Heist's
    {150, {20, 7, 100, 140, 75, 70, 0, 0, 50, 50, 100, 0, 110}},  // ConkerMatrix, the Heist's
    {276, {32, 7, 60, 90, 75, 150, 0, 0, 50, 50, 100, 0, 120}},  // ConkerHelsing (his hair its own, below)
    {59, {24, 5, 30, 120, 65, 35, 2, 2, 50, 50, 100, 40}},   // Weasel: every weasel model (fur.cpp kSharedLooks)
    {118, {20, 10, 100, 60, 75, 70, 5, 10, 50, 50, 100, 0}},  // a weasel of the weasels' program
    {88, {32, 16, 0, 100, 75, 120, 20, 2, 50, 50, 100, 0}},  // CombatSquirrel, and CombatSquirrelShot (fur.cpp kSharedLooks)
    {91, {40, 12, 90, 130, 75, 85, 2, 0, 65, 50, 100, 0}},   // CombatSquirrelrodent
    {135, {12, 12, 70, 110, 75, 110, 2, 2, 70, 50, 100, 100, 100, 0, 100, 30, 110}},  // CombatGeneral
    {445, {36, 10, 60, 100, 70, 110, 2, 2, 70, 90, 100, 0}},     // multiplayer's WWII sharpshooter class
    {441, {20, 16, 20, 60, 75, 70, 5, 0, 50, 50, 100, 0, 100, 0, 100, 8, 100}},  // a multiplayer class
    {461, {24, 15, 20, 200, 70, 90, 5, 20, 30, 70, 70, 0, 100, 0, 100, 30, 170}},  // a Sneeker (multiplayer)
    {13, {64, 1, 0, 200, 100, 150, 0, 0, 150, 150, 100, 50, 100, 0, 100, 15, 100}, 0},  // Haystack
    {69, {12, 4, 20, 100, 100, 60, 0, 0, 200, 150, 100, 100, 100, 0, 100, 0, 100}},  // TerminatorHaystack
    {7, {16, 16, 100, 100, 75, 110, 0, 0, 50, 80, 100, 0}},    // QueenBee
    {109, {16, 12, 80, 100, 75, 110, 0, 0, 100, 50, 100, 100, 120}},  // BeeKing
    {114, {24, 16, 100, 200, 100, 120, 0, 0, 30, 70, 100, 10, 150}},  // Berri: BeriMatrix's look, fill 10
    {398, {64, 16, 0, 200, 80, 100, 0, 0, 75, 140, 100, 20, 120}},  // BerriBarmaid
    {151, {64, 16, 0, 90, 100, 70, 0, 0, 30, 70, 100, 60, 150}},  // BeriMatrix
    {162, {16, 7, 60, 120, 75, 70, 4, 2, 30, 50, 100, 0}},       // BatBoss
    {164, {24, 16, 90, 120, 75, 110, 20, 0, 30, 50, 100, 0}},     // BatBossStoker
    {181, {12, 12, 70, 110, 75, 110, 2, 2, 70, 50, 100, 100, 100, 0, 100, 30, 110}},  // Barman
    {34, {48, 12, 60, 110, 75, 40, 0, 0, 15, 50, 100, 0}},       // RatBrown, and RatBrownStitched (kSharedLooks)
    {124, {48, 10, 30, 100, 70, 150, 0, 0, 70, 200, 100, 0, 150}},  // WarBossDoll
    {kCrown, {24, 16, 0, 20, 75, 50, 2, 2, 50, 50, 100, 0, 100, 0, 100, 15, 120}},  // the crown's rim
    // ConkerHelsing's hair (HairOf(276)), a look of its own; then tone, sway, follows motion
    {HairOf(276), {64, 4, 0, 200, 75, 150, 5, 5, 200, 50, 100, 0, 100, 50, 30}},
};
inline const ModelDefault* FindModelDefault(uint32_t type) {
    for (const ModelDefault& d : kModelDefaults)
        if (d.type == type) return &d;
    return nullptr;
}
inline Look DefaultLook(uint32_t type) {
    if (const ModelDefault* d = FindModelDefault(type)) return d->look;
    if (IsHair(type)) {  // a hair's without, its model's, swaying
        Look l = DefaultLook(type & ~kHairBit);
        l.sway = kHairSway;
        return l;
    }
    return kLookDefaults[KindOf(type & ~kHairBit)];
}
inline uint32_t DefaultShells(uint32_t type) {
    if (const ModelDefault* d = FindModelDefault(type)) return d->shells;
    if (IsHair(type)) return DefaultShells(type & ~kHairBit);  // a hair's without, its model's
    return kExtraShells;
}
bool GameFurOf(uint32_t type);
// the model whose look a model takes: a multiplayer class's models share one (Multi\, MultiFrontend\, MultiLOD\,
// MultiLOD2\, MainFrontend\), its full model's id; Conker's (all but Van Helsing's and the Heist's), 0; a few story models another's
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

// settings.toml fur_distant: a furry object past the near range keeps all its shells, instead of losing them with
// distance as on the console until only the skin is left (src/conker/game/hooks.cpp). Read on each object.
extern bool g_distant;

// Who the fur shells drawn next belong to (src/conker/game/hooks.cpp, the object path 0x78460 and the player's
// 0x80358; players are objects too): its type [object+0x68], the byte at [object+0xA1], the level [0x7FA1F8], in
// the vertex constant kTagRegister, which no program of the game reads, for the shaders to pick a look by. F3
// captures list it with the constants that change (= c-94). Game thread. The shells super fur adds between the game's
// read -1 for its y (ProgramShading::between_mark)
inline constexpr uint32_t kTagRegister = 2;  // hardware order: D3D c-94
// Beside the tag, for a model with hair (HairOf), in this register and the four after: how far the hair trails behind
// its movement, five springs stiffer or looser (xyz, the view's space: x right, y up, z toward the eye, a fraction of
// the hair's length), w 1 when set in the first, the turn behind the facing in the other four's w (radians, four
// springs). Driven by the character's position [object+0x78] and facing [object+0x171] (TagShells)
inline constexpr uint32_t kMotionRegister = 3;  // hardware order: D3D c-93 to c-89
void TagShells(uint32_t object);

// hands the renderer the shading; after the settings load, before boot
void Init();

}  // namespace fur
