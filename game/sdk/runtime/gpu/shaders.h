#pragma once
#include <array>
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
    uint8_t flags = 0;   // kTexWide, kTexOffset; bits 0 and 1 are a backend extension's
};
enum : uint8_t {
    kTexWide = 4,    // an upscaled target shrunk by the pass: each tap averaged over console texels
    kTexOffset = 8,  // uploaded offset by 128 (OffsetSigned): the fetch reads back the console's bytes
};

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
    bool operator==(const PixelKey& o) const;
};

// A game's own shading over vertex programs of the title's, while *on holds: its HLSL goes into the shaders of the
// draws that run one. A program takes the first entry whose match its translation contains. vs runs at the end of
// the vertex shader, after o is filled from the program's registers, and sets o.x0 and o.x1; ps runs in a
// programmable pixel shader after the combiner stages, before the final combiner, and may change r0 (t0..t3, v0, v1,
// i.x0, i.x1 and xc are there too). xc.x is 1 over the largest x the frame and the one before it set in vertex
// constant top (hardware order) for the draw's run of the entry's draws, 0 before one did: a draw whose x is 0
// starts the next run, and runs pair up with last frame's by their order. xc.y is the seconds the renderer has run, as of the frame's
// start, for shading that moves. between more draws go before each draw
// whose x in top is above the one before it in its run, with x stepped evenly from there to the draw's, the rest of
// its state the same: more shells for the fur. soft spreads a greater (or equal) alpha
// test's edge over a pixel: for alpha blended draws, whose cut out edges then blend in instead of stepping. fill, when
// set, writes xs (16 float4, declared only in its shaders) for each of its draws. tess, when set and the GPU
// tessellates, draws the entry's triangles as patches (TessHlsl), each edge split in pieces some 6 console pixels long
// on the screen, up to 16: the domain shader fills o with what the vertex shader handed on, interpolated across the
// triangle, then runs tess, which sets o.pos (and anything else that is no straight blend of the corners'); p holds
// the corners, and the draw's stages are there as Texture2D tex0..tex3 with smp0..smp3. field,
// when set, hands a grid of floats its shaders read as Texture2D<float> xfield (t4) with SamplerState xfield_smp (s4),
// linear and clamped; it is uploaded again when its version moves on. functions is HLSL at the top of its shaders,
// after the constants, for vs, ps and tess to call.
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
    void (*fill)(float (*xs)[4]) = nullptr;
    const char* tess = "";
    ShadingField (*field)() = nullptr;
    const char* functions = "";
};
inline std::vector<ProgramShading> g_program_shading;  // filled before the title boots, unchanged after

// A game's look at the draws of a vertex program of the title's, on the render thread: seen gets each draw whose
// program's translation contains match, before the draw is made (one in the state of the draw before too), with the
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
};
inline std::vector<ProgramWatch> g_program_watch;  // filled before the title boots, unchanged after

uint64_t Hash(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]);

// vertex program microcode to HLSL, regs is the bit mask of v registers the draw supplies
// decode: VertexDecode per register, 4 bits each, for elements the input assembler hands over raw
// shading: a g_program_shading entry, 1 based, 0 none
std::string VertexProgramHlsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode = 0,
                              uint32_t shading = 0);
// the g_program_shading entry a program matches, 1 based, 0 none, whether it is on or not
uint32_t MatchProgramShading(const std::vector<uint32_t>& program);
// the g_program_watch entry a program matches, 1 based, 0 none
uint32_t MatchProgramWatch(const std::vector<uint32_t>& program);
// where a vertex program's clip w comes from: the constant register (hardware order) a dp4 or dph into oPos.w reads,
// kPosWInput when it is moved from an input (vertices already on the screen), kPosWUnknown otherwise
constexpr int kPosWInput = -1;
constexpr int kPosWUnknown = -2;
int PositionWSource(const std::vector<uint32_t>& program);
// fixed function vertex state a shader is built for
struct FixedKey {
    uint32_t light = 0;    // bit 0 lighting, 1 specular, 2 local viewer, 3 normalize, then D3DMCS 2 bits each from bit 8:
                           // diffuse, specular, ambient, emissive (material when the vertex lacks that colour)
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
    // what spreadtap scales a render target's colour by
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
    float surf[4];      // target width, height, 1 / depth scale, 0
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
    float xc[4];         // a ProgramShading's, declared only in its shaders
    float xs[16][4];     // a ProgramShading's fill, declared only in its shaders
};

}  // namespace gpu
