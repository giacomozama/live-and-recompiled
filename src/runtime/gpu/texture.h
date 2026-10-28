#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "gpu/state.h"

namespace gpu {

// What the renderer uploads a console texture format as
enum class Host : uint8_t { BGRA8, BC1, BC2, BC3, R32F, R16Unorm, None };

struct FormatInfo {
    const char* name;
    uint8_t bits;      // per texel, 4 or 8 bytes per block for the DXT formats
    bool swizzled;     // Morton order in memory
    bool compressed;   // 4x4 blocks
    bool depth;
    Host host;
};

const FormatInfo& Format(uint32_t xbox_format);
// the channels filtered as signed, bits 0 to 3 r g b a: V8U8 and LIN_V8U8's two's complement du dv, else those the
// stage's filter marks signed (FILTER bits 31:28 b g r a, kTssColorSign). They go up offset by 128 so the filter blends
// them as signed (+1 next to -1 blended as unsigned bytes reads -1, an outline along every zero of a bump map, a cross
// through a raindrop on the lens); the shader takes the 128 off again
uint8_t OffsetSigned(uint32_t xbox_format, uint32_t color_sign);

// a D3DPixelContainer decoded
struct TextureDesc {
    uint32_t data = 0;  // guest virtual address of level 0
    uint32_t format = 0;
    uint32_t width = 1, height = 1, depth = 1, levels = 1;
    uint32_t pitch = 0;  // linear formats only
    bool cube = false;
    bool linear = false;
};

bool Describe(uint32_t header, TextureDesc& out);
bool DescribeSurface(const Surface& s, TextureDesc& out);
uint32_t LevelSize(const TextureDesc& d, uint32_t level);
uint32_t LevelOffset(const TextureDesc& d, uint32_t level);
uint32_t FaceSize(const TextureDesc& d);  // cube face stride
uint32_t TotalSize(const TextureDesc& d);

// converts one level (one face) from guest memory into what Host describes, rows tightly packed
void DecodeLevel(const TextureDesc& d, uint32_t level, uint32_t face, const uint8_t* guest, const uint32_t* palette,
                 std::vector<uint8_t>& out, uint32_t& row_pitch);

// DXT blocks (BC1, BC2 or BC3, as DecodeLevel left them: rows of blocks pitch bytes apart, the slices one after the
// other) as BGRA texels, w by h a slice, rows tightly packed; for GPUs without BC textures (most phones')
void DecodeDxt(Host kind, uint32_t w, uint32_t h, const std::vector<uint8_t>& blocks, uint32_t pitch,
               std::vector<uint8_t>& out, uint32_t& row_pitch);

uint32_t LinearColorFormat(uint32_t fmt);
uint32_t LinearDepthFormat(uint32_t fmt);
void SurfaceSize(const Surface& s, uint32_t& w, uint32_t& h);

// CopyRects done in guest memory, false when the formats do not allow it
bool CopySurfaceMemory(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points);

// the title locked a resource, the next bind re-uploads it
void MarkDirty(uint32_t resource);
uint64_t DirtyEpoch(uint32_t data_va);

// A level's baked lightmap (lightmap.cpp), w by h BGRA texels (rows w * 4 bytes), for the Vulkan renderer's lightmap
// smoother, which makes it larger with each texel averaged along the light's edges through it (a gaussian of along
// texels), so the steps of an aliased shadow edge come out a straight line: the texels as RGBA floats (B, G, R, A as
// the texels have them) and their blurred structure tensor (xx, xy, yy, 0 a texel), with the walk along the edges
struct LightmapField {
    std::vector<float> img, tensor;
    float along = 0, step = 0;
    int steps = 0;
};
void PrepareLightmap(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, float along, LightmapField& f);
// DXT1, DXT3 or DXT5 blocks (rows of blocks, as DecodeLevel gives them) to w by h BGRA texels
void DecodeBlocks(Host host, const uint8_t* blocks, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

// A glyph atlas drawn again from a font of the player's (the game's custom fonts): the title's atlas at data_va
// keeps its size as the game sees it (w by h texels, which its glyphs' texture coordinates are in) and is sampled
// from a picture scale times as wide and tall, with levels down to below its own size. coverage is that picture's
// ink, 0 to 255, rows w * scale; each copy is a glyph the font has no shape for, taken from the title's own atlas
// (src, in its texels) into dst (in the picture's texels) as it was. The renderer turns coverage into the atlas's
// own texels through what its channels hold (ComposeFontAtlas). id differs for each atlas made.
struct FontAtlas {
    struct Copy {
        float sx0, sy0, sx1, sy1;
        uint32_t dx0, dy0, dx1, dy1;
    };
    uint32_t w = 0, h = 0, scale = 1;
    uint64_t id = 0;
    std::vector<uint8_t> coverage;
    std::vector<Copy> copies;
};
// the game's side: an atlas for the texture at data_va, or none (null) to sample the title's own again; any thread
void SetFontAtlas(uint32_t data_va, std::shared_ptr<const FontAtlas> atlas);
std::shared_ptr<const FontAtlas> FontAtlasFor(uint32_t data_va);  // null without one
bool AnyFontAtlas();  // cheap, a bind's first test
// set by a renderer that draws them (Vulkan): the game patches its fonts only then
extern std::atomic<bool> g_font_atlases;
// the picture at a.w * a.scale by a.h * a.scale BGRA texels: the title's atlas level 0 (px, a.w by a.h BGRA, rows
// a.w * 4) gives each channel's value without ink and with full ink (a channel that varies over it goes from its least
// to its most with the coverage, one that doesn't keeps its value), and the copies its texels, filtered linear
void ComposeFontAtlas(const FontAtlas& a, const std::vector<uint8_t>& px, std::vector<uint8_t>& out);
// the levels for such a picture: each a box over the one before, from out (level 0) down to below the title's size
uint32_t FontAtlasLevels(const FontAtlas& a);

// vertex declarations and pixel shader constant routing, both follow the XDK's definitions
void ParseDeclaration(VertexShader& vs);
// SetPixelShaderConstant: the stages of the current shader mapped to these registers take the values
void ApplyPixelConstants(State& s, uint32_t first, uint32_t count);
uint32_t PackColor(const std::array<float, 4>& c);

}  // namespace gpu
