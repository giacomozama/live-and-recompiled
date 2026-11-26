#pragma once
// The OpenGL renderer's shaders (gpu/opengl.cpp): the HLSL every renderer shares, as GLSL
#include <cstdint>
#include <string>
#include <vector>

namespace gpu {

// the GL targets a sampler reads (GL_TEXTURE_2D, _CUBE_MAP, _3D, _2D_MULTISAMPLE), without GL's headers
constexpr uint32_t kGlTexture2D = 0x0DE1, kGlTextureCube = 0x8513, kGlTexture3D = 0x806F, kGlTexture2DMs = 0x9100;

// a texture unit's (texture register, sampler register) pair, the same unit in every stage and program: a texture read
// with its own sampler (or none, Load and GetDimensions) at its register's unit, one of the full-screen passes'
// other pairs (t0-t3 with s0 or s1) past them. 0xFF: no sampler
inline constexpr uint8_t kNoSampler = 0xFF;
inline constexpr int kUnits = 16;
inline int Unit(uint32_t t, uint32_t s) {
    if (s == kNoSampler || s == t) return t < 8 ? int(t) : -1;
    if (t < 4 && s < 2) return 8 + int(t * 2 + s);
    return -1;
}
struct Bind {
    uint8_t unit, t, s, pad;
    uint32_t target;  // the GL target the stage's sampler type reads
};
static_assert(sizeof(Bind) == 8);

// glslang started, once for the process (any thread, before HlslToGlsl)
void GlslInit();
// the HLSL to GLSL #version version, any thread: glslang's SPIR-V (registers bN, tN, sN at bindings N, a vertex input
// at its register's location, VK_LOC) through SPIRV-Cross. Each separate texture and sampler pair becomes one sampler
// at its Unit (binds: what the stage reads); uniform blocks and samplers are named after the stage, so two stages'
// views of one cbuffer needn't match to link. err: why not, on false
bool HlslToGlsl(const std::string& src, const char* entry, const char* profile, uint32_t version, std::string& glsl,
                std::vector<Bind>& binds, std::string& err, bool flip = false);
// flip: for a context without glClipControl, the last vertex stage ('v', 'd' or 'g') mirrors the position's y and
// takes depth 0 to 1 to GL's -1 to 1 itself (the front face to be swapped with it)
// kDepthBytesHlsl multisampled as GL reads it: the stencil texture's value in red, and the depth's size from the pass
// constants (float4 at 8: width, height), as a multisampled texture's GetDimensions asks its sample count too, which
// GLSL before 4.50 asks only with ARB_texture_query_samples
const std::string& GlDepthBytesMultisampled();

}  // namespace gpu
