#pragma once
// Shader records made from the game's disc instead of a playthrough: the static push buffers in its asset packages
// (gpu/disc_programs.h unpacks them) hold most of what the title draws, each model a block of commands opened by
// 144 no-operations (the 576 bytes the title patches to a jump when the model is culled) that bind its vertex arrays,
// upload its vertex program and set its materials' combiners, textures and blending before their draws. Each block
// is read by d3d::DryWalk, the way the title's own commands are, into a backend that draws nothing: it makes the
// records the Vulkan renderer would for each draw (VulkanBackend::Draw's keys, the vertex layout by AssembleLayout),
// under the state the title's code sets around those blocks that a pipeline keeps (the color write mask). A block
// that sets up its vertex program or its material more than one way, each behind a no-operation the title patches
// into a jump, is read each way (Ways, MaterialWays).
//
// It runs in a process of its own (the game started with --make-shader-records): the walk uses the device's state,
// which in the game is the title's.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gpu/shader_record.h"

namespace gpu::shader_gen {

// what the records depend on beyond the commands: the renderer's own state at the draws
struct Context {
    uint32_t rtfmt = 87;    // DXGI format of the frame's color target (B8G8R8A8_UNORM)
    uint32_t samples = 1;   // its multisampling
    bool tess = false;      // the device tessellates (TessellatedDraw's patches)
    bool vrs = false;       // and shades coarsely (ProgramShading::coarse)
};

// The records for game's files (xbe already loaded: kernel::LoadTitleImage) written to out, in shaders.rec's format
// with Stamp's layout; the process exit code, 0 when written
int Run(const std::filesystem::path& out, const Context& ctx);

// what the records were made for: RecordLayout, ctx, the settings MakePixelKey reads (the fog) and which shading
// entries are on (the game's own settings)
uint64_t Stamp(const Context& ctx);

// The records for ctx, kept in app::CacheDir as disc_shaders.rec: true with them when the file there is for this
// Stamp. make: else this executable is run to make it (Run, its log game-shadergen.log), waited for unless stop is
// set, and the file read again (a second instance on this PC only reads). Off the title's thread
bool Load(const Context& ctx, bool make, const std::atomic<bool>& stop, std::vector<shader_record::Entry>& out);

}  // namespace gpu::shader_gen
