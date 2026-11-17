#pragma once
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/state.h"

// HLSL for the title's shaders and for its fixed function state, per the XDK's definitions of both
namespace gpu {

enum TexKind : uint8_t { kTexNone, kTex2D, kTex3D, kTexCube };

struct TexInfo {
    uint8_t kind = kTexNone;
    uint8_t rect = 0;    // linear texture, coordinates in texels
    uint8_t shadow = 0;  // depth texture: 1 D24, 2 D16
    uint8_t flags = 0;   // kTexWide, kTexDepthBytes; bits 0 and 1 are a backend extension's
    uint8_t sign = 0;    // channels uploaded offset by 128 (OffsetSigned), bits r g b a: the fetch reads back the bytes
};
enum : uint8_t {
    kTexWide = 4,  // an upscaled target shrunk by the pass: each tap averaged over console texels
    // a depth buffer's bytes read as color (stage 0 of a programmable draw: a depth of field's focus mask), bits 5
    // and 6 the DepthByteLayout: nearer pixels around lend the draw their output (DepthSpread)
    kTexDepthBytes = 16,
    // the frame a ProgramShading::glow_capped draw went into this frame, read by a pass with a depth buffer's bytes
    // on stage 1 (the title's glow gather): its alpha read capped at kFurGlowAlpha
    kTexFurGlow = 8,
};
// The title's laser glow (F3 capture 084853_0, 2026-11-16) gathers the frame where the beams' sprites marked it, each
// pixel by the frame's alpha: the sprites' own a quarter or so at most, opaque draws none, but fur's skin pass and
// shells write theirs, near 1, so a tail crossing a beam glows in full. The console's did too; super fur's coat, fuller
// and upscaled, made it a splat of light many times the beam's. Fur reads no higher than the sprites' strongest
constexpr float kFurGlowAlpha = 0.3f;
constexpr int kTexDepthLayoutShift = 5;

struct PixelKey {
    uint32_t programmable = 0;
    // combiners, straight from D3DPIXELSHADERDEF
    uint32_t count = 0, rgb_in[8] = {}, rgb_out[8] = {}, a_in[8] = {}, a_out[8] = {}, final_abcd = 0, final_efg = 0;
    uint32_t tex_modes = 0, dot_mapping = 0, input_texture = 0, compare_mode = 0;
    // fixed function stages: colorop, arg1, arg2, arg0, alphaop, arg1, arg2, arg0, result
    uint32_t ff[4][9] = {};
    TexInfo tex[4];
    uint32_t alphakill = 0;  // bit per stage
    uint32_t flat = 0, two_sided = 0;
    uint32_t proj = 0;       // fixed function: bit per stage sampled at coordinates divided by w
    uint32_t quad = 0;       // a screen quad: QuadHlsl
    uint32_t shading = 0;    // g_program_shading's entry for the draw's vertex program, 1 based, 0 none
    uint32_t depth_pass = 0; // a depth pass: discards what blends in fainter than the float these are the bits of
    // the fog's tint in place of its haze (app::Fog::TintOnly): fogtint on outc by the fog factor, 1 read as D3D's
    // (1 unfogged), 2 as Conker's raw final combiner's (the share of fog color)
    uint32_t fog_tint = 0;
    bool operator==(const PixelKey& o) const;
};

// A game's own shading over vertex programs of the title's, while *on holds: its HLSL goes into the shaders of the
// draws that run one. A draw takes the first entry whose match its program's translation contains, that is on. vs runs at the end of the vertex shader, after o is filled from the
// program's registers, and sets o.x0 and o.x1; ps runs in a programmable pixel shader after the combiner stages,
// before the final combiner, and may change r0 (t0..t3, v0, v1, i.x0, i.x1 and xc are there too). xc.x is 1 over the largest x the frame and the one before it set in vertex
// constant top (hardware order) for the draw's run of the entry's draws, 0 before one did: a draw whose x is 0
// starts the next run, and runs pair up with last frame's by their order. xc.y is the seconds the renderer has run, as of the frame's
// start, for shading that moves. xc.z is 1 when the pixel shader samples stage 3 (texsize fills for a bound texture on a stage that's off too). between more draws go before each draw
// whose x in top is above the one before it in its run, with x stepped evenly from there to the draw's, the rest of
// its state the same: more shells for the fur. soft spreads a greater (or equal) alpha
// test's edge over a pixel: for alpha blended draws, whose cut out edges then blend in instead of stepping. fill, when
// set, writes xs (20 float4, declared only in its shaders) for each of its draws. tess, when set and the GPU
// tessellates, draws the entry's triangles as patches (TessHlsl), each edge split in pieces some 6 console pixels long
// on the screen, up to 16: the domain shader fills o with what the vertex shader handed on, interpolated across the
// triangle, then runs tess, which sets o.pos (and anything else that is no straight blend of the corners'); p holds
// the corners, and the draw's stages are there as Texture2D tex0..tex3 with smp0..smp3. ps sees each stage's
// PS_TEXTUREMODES mode as STAGE0_MODE..STAGE3_MODE, for a program whose draws set their stages up more than one way. field,
// when set, hands a grid of floats its shaders read as Texture2D<float> xfield (t4) with SamplerState xfield_smp (s4),
// linear and clamped; it is uploaded again when its version moves on. functions is HLSL at the top of its shaders,
// after the constants, for vs, ps and tess to call.
// depth_alpha, when above 0, splits each of its draws that blends and writes depth in two: the color without the
// depth write, then a depth pass (no color, no stencil) where the output's alpha reaches depth_alpha, so what blends
// in fainter leaves the depth behind it; with top set, only the draws whose x there is above 0 (the shells, not the
// skin under them). also lists more text the translation must contain as well as match, for an entry that takes only
// one shape of a program; without, text it must not contain, for an entry that leaves one shape out. pre runs in the pixel shader before the combiner stages, after the textures are fetched (before
// the fixed function's stages, which fetch as they go), and may change v0 and v1; PS_TWO_SIDED is 1 there when the back faces take the back colors, PS_PROGRAMMABLE when the draw has a pixel shader and so t0..t3 for the stages in use, STAGE0_MODE..STAGE3_MODE as for ps. plain: the shading reads
// no constants of its own (no clock, fill or top), so a draw in the state of the one before may reuse its pipeline.
// screen, when set, copies the draw's render target just before each of its draws, what the draw blends over, for its
// shaders to read as Texture2D xscreen (t5) with SamplerState xscreen_smp (s5), linear and clamped, at the pixel's
// SV_Position over the copy's size. post runs in the pixel shader after the final combiner, before the alpha test, and
// may change outc. DEPTH_PASS is 1 in all the pixel shader's hooks (coords, pre, ps, post) in depth_alpha's (and a
// g_surface_depth surface's) depth pass, 0 otherwise: what only colors may be left out there.
// coords runs in the pixel shader before the textures are fetched and may change tc0..tc3, which every stage then
// reads by, whatever its kind; what it declares outside braces the later hooks see.
struct ShadingField {
    const float* data = nullptr;  // w x h, rows of w
    uint32_t w = 0, h = 0;
    uint64_t version = 0;
};
struct ProgramShading {
    const char* match = "";
    const bool* on = nullptr;
    const char* vs = "";
    const char* ps = "";
    int top = -1;
    int between = 0;
    bool soft = false;
    // reg: the draw's vertex constant fill_register (hardware order), null when that is -1
    void (*fill)(float (*xs)[4], const float* reg) = nullptr;
    int fill_register = -1;
    const char* tess = "";
    ShadingField (*field)() = nullptr;
    const char* functions = "";
    float depth_alpha = 0;
    const char* also[3] = {};
    const char* without = nullptr;
    const char* pre = "";
    bool plain = false;
    bool screen = false;
    const char* post = "";
    const char* coords = "";
    const uint32_t* between_live = nullptr;  // when set, read on each draw in place of between
    // when set, the draw's shells between from its fill_register (null when that is -1) and its top register (null
    // when top is -1): -1 keeps between
    int (*between_of)(const float* reg, const float* top) = nullptr;
    // when set, top's w is the draw's shell of this many, x growing in proportion to w + 1 at a rate of its model
    // piece's own, as pieces of different lengths take turns in one loop over the shells (a bunny's body and tail): xc.x
    // and between's steps go by that instead of the run's largest x and the draw before's, so each piece reaches 1
    int top_layers = 0;
    // when 0 or more, a vertex constant (hardware order) whose y the shells between set to -1 in their copies, for
    // the shaders to tell them from the draw's own
    int between_mark = -1;
    // when set with top_layers, the shells drawn past a draw that is its piece's outermost (top's w top_layers - 1),
    // after it, its state the same: their number (0 none) from its fill_register and top registers (null as
    // between_of's), and in *reach how far past its own x they go, a share of it, x stepped evenly out to there. Marked
    // as between_mark's, and no depth pass, as the shells between
    int (*beyond_of)(const float* reg, const float* top, float* reach) = nullptr;
    // when set and FSR is on, its draws keep FSR's sharpening off where they cover (fur, soft by design): they write
    // their output alpha into the target's alpha, the largest kept (kWmaskAlphaMax), the frame's otherwise 0 (the
    // title clears it so and blends no draw by it), and EASU upscales those pixels plainly and RCAS leaves them be
    bool unsharpened = false;
    // when set, the title's glow gather reads the alpha of the frame its draws went into capped (kTexFurGlow)
    bool glow_capped = false;
    // when set, its draws with no fog of their own take the level's: the fog constants (fogp) of the last draw that
    // had the chip's fog on, this frame or the one before, zero once a frame had none (or the fog setting is off),
    // for its vs to fog by what the title's own fogged draws would have
    bool world_fog = false;
};
inline std::vector<ProgramShading> g_program_shading;  // filled before the title boots, unchanged after

// The title's depth of field: a composite over the frame whose stage 0 reads the depth buffer's bytes (DepthSpread's
// draw), blending its blurred copy in by its alpha (the blend's destination factor INV_SRC_ALPHA). NoteDepthOfField
// works that alpha out on the CPU from the draw's combiners for each top byte of the depth, the least over the byte's
// span and its neighbours', so no depth reads blurrier than it is; nothing when the alpha hangs on anything but the
// depth and constants. FocusField hands the table to a g_program_shading entry's field: 256 x 1, read at u = the
// host depth (0..1, as the depth buffer holds it), last frame's composite's, 0 throughout once two frames had none
void NoteDepthOfField(const PixelKey& k, const float (*pc)[4], uint32_t dst_blend);
ShadingField FocusField();
// a depth of field composite in one of the last two frames
bool DepthOfFieldLately();

// A game's look at the draws of a vertex program of the title's, on the render thread: seen gets each draw whose
// program's translation contains match (and also, when set), before the draw is made (one in the state of the draw before too), with the
// input registers regs names of its first vertices, or of vertices spread evenly over it; frame runs at each present.
struct WatchedDraw {
    const float (*v[16])[4] = {};  // per input register asked for and supplied: its value at each vertex, else null
    uint32_t vertices = 0;         // up to the watch's
    uint32_t tex_modes = 0;        // PS_TEXTUREMODES, 5 bits per stage, 0 without a pixel shader
    const std::array<float, 4>* c = nullptr;  // the 192 vertex constants, hardware order
    uint64_t program = 0;                     // the program's hash
    const std::string* hlsl = nullptr;        // its translation, the one match was found in
};
struct ProgramWatch {
    const char* match = "";
    void (*seen)(const WatchedDraw&) = nullptr;
    void (*frame)() = nullptr;
    uint16_t regs = 1;       // bit per input register, v0 alone by default
    bool spread = false;     // vertices spread over the draw rather than its first
    uint32_t vertices = 16;  // at most
    const char* also = "";
    const bool* on = nullptr;  // when set, the watch looks only while it holds (render thread)
};
inline std::vector<ProgramWatch> g_program_watch;  // filled before the title boots, unchanged after

// A game's surfaces that blend over what lies under them without writing depth, while *on holds: a floor with the
// reflection under it, whose depth is the reflection's to a depth of field that reads the depth buffer. takes gets
// each draw that blends and tests depth but doesn't write it, with v0 at vertices spread over it (WatchedDraw's v[0],
// the other fields unset); one it takes writes its depth after its color, where its output's alpha reaches alpha.
struct SurfaceDepthHook {
    const bool* on = nullptr;
    bool (*takes)(const WatchedDraw&) = nullptr;
    float alpha = 0.5f;
    uint32_t vertices = 16;  // at most
};
inline SurfaceDepthHook g_surface_depth;  // set before the title boots, unchanged after

// Where a game's vertex programs keep the view, for the ambient occlusion (app::Settings) to read the depth buffer as
// view space positions: the viewport's scale and offset (screen = clip / w * scale + offset) in hardware registers
// viewport and viewport + 1, the projection's four rows from projection, w = -z (the last row 0 0 -1 0). -1: none.
// camera, for the motion blur: the camera's view (the level's space to view space) in camera to camera + 2, a row for
// each of view space's x y z (view = row . (p, 1)), and camera + 3 = (the frame it was handed over in, 0, 0, 1), in
// registers no program of the game reads, which the game sets as its draws go through a camera. -1: none
struct ViewConstants {
    int viewport = -1;
    int projection = -1;
    int camera = -1;
};
inline ViewConstants g_view_constants;  // set before the title boots, unchanged after

// A game's HUD unstretched at a display wider than the frame: while on, the draws the game puts in a HUD scope
// (d3d::HudScope, an element of its HUD) are drawn at the frame's shape as the scope's HudMode says. The game sets it
// each frame, game thread
inline std::atomic<bool> g_hud_split{false};
// Outlines round marked objects (a game's players): while on, the draws that test and write depth write their mark
// (d3d::OutlineMark: 1 green, 2 red, 0 none) into the stencil's top two bits, so a mark stays only where its object is
// still in front, and the effects over the 3D picture draw a line round each marked shape. The game sets it each
// frame, game thread
inline std::atomic<bool> g_outlines{false};
// the outlines' colours: 0 green teammates, red opponents, light blue neutrals; 1 blue, orange, light grey (for red-green
// colour blindness). The game sets it each frame, game thread
inline std::atomic<uint32_t> g_team_colors{0};
// how the draws of a HUD scope (d3d::HudScope) are placed: by the edge the element's box keeps nearest to (held),
// unstretched where it is (about the element's box, or each draw's own), or as the frame has them. kHudInPlaceBars is
// kHudInPlace with black down the sides of a draw that covers the frame (a sniper's scope, no longer to the edges)
enum HudMode : uint8_t { kHudNone, kHudEdge, kHudInPlace, kHudInPlaceEach, kHudStretched, kHudInPlaceBars };

uint64_t Hash(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]);

// vertex program microcode to HLSL, regs is the bit mask of v registers the draw supplies
// decode: VertexDecode per register, 4 bits each, for elements the input assembler hands over raw
// shading: a g_program_shading entry, 1 based, 0 none
std::string VertexProgramHlsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode = 0,
                              uint32_t shading = 0);
// the g_program_shading entries a program matches, bit i for entry i + 1 (the first 64), whether they are on or not
uint64_t MatchProgramShading(const std::vector<uint32_t>& program);
// the g_program_watch entries a program matches, bit i for entry i (the first 64): each sees the draw
uint64_t MatchProgramWatch(const std::vector<uint32_t>& program);
// where a vertex program's clip w comes from: the constant register (hardware order) a dp4 or dph into oPos.w reads,
// kPosWInput when it is moved from an input (vertices already on the screen), kPosWUnknown otherwise
constexpr int kPosWInput = -1;
constexpr int kPosWUnknown = -2;
int PositionWSource(const std::vector<uint32_t>& program);
// the constant registers (hardware order) a vertex program reads; one read relative to a0 marks its base and every
// register above it
std::bitset<192> ProgramReads(const std::vector<uint32_t>& program);
// every texture coordinate a vertex program writes is an input moved as it is (x and y in place, not negated)
bool ProgramCopiesTexcoords(const std::vector<uint32_t>& program);
// the lowest register a vertex program reads relative to a0 (a bone palette's base), 192 for none
uint32_t ProgramRelativeBase(const std::vector<uint32_t>& program);
// fixed function vertex state a shader is built for
struct FixedKey {
    uint32_t light = 0;    // bit 0 lighting, 1 specular, 2 local viewer, 3 normalize, then D3DMCS 2 bits each from bit 8:
                           // diffuse, specular, ambient, emissive (material when the vertex lacks that color)
    uint32_t types = 0;    // D3DLIGHTTYPE per light, 2 bits each, 0 off
    uint32_t tex[4] = {};  // per stage: coordinate set, texgen at bit 4, D3DTTFF count at bit 8, projected bit 11, set size at bit 12
};
FixedKey MakeFixedKey(const State& s);
std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode = 0, const FixedKey& f = {});
std::string PixelHlsl(const PixelKey& k);
// a screen quad's vertex or pixel shader: texture coordinates as the quad was drawn, before its far edges were pulled out
std::string QuadHlsl(std::string s);
// the geometry shader that draws a point as a square of its size, a point sprite with stage 3 spanning it
std::string PointHlsl();
// a g_program_shading entry's tessellation: the hull shader (entry hs) and domain shader (ds)
std::string TessHlsl(uint32_t shading);

// HLSL a backend extension adds; the defaults write the console path
struct ShaderHooks {
    virtual ~ShaderHooks() = default;
    // the cbuffer line for DrawConstants::ext
    virtual std::string Constants() const;
    // shadow(t, s, uv, z, fmt, px, size), float4, t a Texture2D<float> and s its SamplerComparisonState: a macro, since
    // the Vulkan renderer's SPIR-V can't take a texture as a function's argument (a hook may define a function)
    virtual std::string Shadow() const;
    // what spreadtap scales a render target's color by
    virtual std::string ReadScale(const TexInfo&) const { return "1"; }
    // a cube sampled along dir at stage i, empty for a plain sample
    virtual std::string Cube(const TexInfo&, int, const std::string&) const { return {}; }
    // after stage i's fetch
    virtual std::string Fetched(const TexInfo&, int) const { return {}; }
    // outc to the target, after the alpha test
    virtual std::string Output() const;
};
inline const ShaderHooks* g_shader_hooks = nullptr;

// the constant buffer layout both stages share, register b1
struct DrawConstants {
    float surf[4];      // target width, height, 1 / depth scale, 0 (1 for points)
    float fog[4];       // start, end, density, mode
    float fog_color[4];
    float misc[4];      // alpha ref, alpha func, shadow func, specular
    float pc[18][4];    // combiner c0 c1 per stage, then final c0 c1
    float tfactor[4];
    float texsize[4][4];
    float bump[4][4];   // 00 01 10 11
    float bumpl[4][4];  // scale offset
    float wvp[16];      // fixed function
    float ext[4];       // a backend extension's, declared by ShaderHooks::Constants
    float spread[4][4]; // kTexWide: tap step in uv, taps per axis
    // fixed function, lit in view space
    float wv[16];
    float wvn[16];       // normals: inverse transpose of wv
    float tm[4][16];     // texture transforms
    float mat[5][4];     // material diffuse, ambient, specular, emissive, power
    float amb[4];        // D3DRS_AMBIENT
    float lt[8][7][4];   // per light: diffuse, specular, ambient, position range, direction falloff, attenuation, cos theta/2 phi/2
    // points: fixed function size, viewport height when scaled by distance (else 0), smallest and largest size
    float pts[4];
    float ptsk[4];       // the distance scale's A, B, C, then 1 for point sprites
    float ptpx[4];       // points: the target's host pixels per console pixel across and down
    float xc[4];         // a ProgramShading's, declared only in its shaders
    float xs[20][4];     // a ProgramShading's fill, declared only in its shaders
};

}  // namespace gpu
