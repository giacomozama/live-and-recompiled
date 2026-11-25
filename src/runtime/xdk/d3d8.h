#pragma once
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "gpu/state.h"

struct X86Context;
namespace gpu {
class Backend;
}

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();

// A title whose library flushes deferred state itself (D3D_DirtyFlags) and has that flush replaced: the D3D states
// the flush would have pushed for these dirty bits take the chip back from the title's own raw writes.
void DeferredFlush(uint32_t dirty);

// Reads every command the title has pushed so far. A title that patches a push buffer it already queued (the
// library's RunPushBuffer fixups) calls this first: the chip ran the earlier runs before the patch landed.
void FlushPending();

// The next DrawVerticesUP or DrawIndexedVerticesUP draws with tex on the stage in place of the texture bound, which is
// bound again after it (a title's icon drawn as another image); tex 0 takes an earlier call back. Game thread
void TextureForNextDraw(uint32_t stage, uint32_t tex);

// XBE_LOD_TRACE set: draws that switch detail between frames are logged with the push buffer or caller they came
// from. A title calls LodTraceRun as it runs a push buffer (its data address, as the command stream jumps to it, and
// whatever it knows of the object drawing it), to log the call chain that first ran it and the push buffers that
// stop and start running from one frame to the next.
bool LodTrace();
void LodTraceRun(X86Context& ctx, uint32_t data, uint32_t owner = 0);

// The object and the piece of its model whose draws the title pushes next, for frame generation to find each draw
// again by (gpu::interp::Tag): every draw read off the commands pushed from here until the next SetDrawTag carries
// them, the commands pushed before are read first. {0, 0} tags nothing. Returns the tag it replaces. group: the
// object's draws come in no fixed order (an effect's particles), so its tag says only whose draw it is, and its draws
// are told apart among themselves by how near they are (gpu::interp::Tag::group)
// label (XBE_PERF only, not part of the tag's key): the guest address of a name the draws are counted under, logged
// once a second as 'perf: labelled'; a tag set with none inside a labelled one keeps its label (an effect's models)
struct DrawTag {
    uint32_t object = 0, piece = 0;
    bool group = false;
    uint32_t label = 0;
};
DrawTag SetDrawTag(DrawTag tag);
// XBE_PERF: one more of what label names (an effect drawn), beside the draws counted under it
void CountLabel(uint32_t label);

// The HUD element whose draws the title pushes next (gpu::g_hud_split): scopes nest, and every draw read off the
// commands pushed inside one carries the innermost's id and gpu::HudMode, and the scopes around it for the trace
// (gpu::interp::Tag); the commands pushed before are read first. Game thread
void HudScope(uint32_t id, uint8_t mode);
void HudScopeEnd();
// the innermost scope's id and mode, {0, 0} outside any
std::pair<uint32_t, uint8_t> HudScopeNow();
// a line in the frame capture, in order with the draws (the HUD trace)
void HudNote(const char* text);

// A title's pause screen from one picture (gpu::Backend::PausePicture): keep, the frame being pushed keeps its
// picture as the 2D comes to draw over it; !keep, that picture written back into the back buffer (the device's now),
// in order with the commands pushed before. PausePictures: whether the renderer does it. Game thread
void PausePicture(bool keep);
bool PausePictures();

// One vertex program constant (hardware order, D3D register + 96) for the draws the title pushes next, as
// SetVertexShaderConstant would: the commands pushed before are read first. For a game's own data handed to its
// shading hooks in a register no program of the title reads. Game thread
void SetVertexConstant(uint32_t reg, const std::array<float, 4>& v);

// The draws the title pushes next, until a false, are drawn solid: no blending, depth written (a model's piece the
// title draws see-through, wanted opaque). The commands pushed before are read first. Game thread
void OpaqueDraws(bool on);

// While gpu::g_outlines is on, the draws the title pushes next carry this outline mark (1 green, 2 red, 3 light
// blue, 0 none): each that tests and writes depth writes it into the stencil's top two bits where it passes, and every
// other one that writes depth writes 0 there; see-through ones (no depth written) leave the marks. A draw testing the
// stencil itself keeps its test, on the low six bits. The commands pushed before are read first. Game thread
void OutlineMark(uint8_t mark);

// While gpu::g_outlines is on: one object's draws begin (true) or end (false). Once a draw of the object writes the
// title's brightest glow into the stencil (bits 4..5 both set, as 0xDB6F0 does for a bullet's hit flash), it and the
// object's later draws leave the outline marks as they are when they carry none, as see-through ones do, so a hit
// flash on a player or vehicle doesn't cut holes in its outline. Returns the scope it replaced, to be passed back at
// the end. The commands pushed before are read first. Game thread
uint8_t OutlineObject(uint8_t scope);

// While gpu::g_outlines is on: the outlines drawn over the frame again, by the marks the stencil holds, before the
// next draw the title pushes (the MP Sneeker drone's overlay covers the whole view, the outlines drawn under it). The
// commands pushed before are read first. Game thread
void OutlinesAgain();

// Whether a title's loading screen, about to be drawn into the front buffer, should be drawn. Each one shown is a
// Present the loader waits on, and those drawn unshown still cost the GPU their whole picture in the first frame
// after, so none is: the first ask since the last Swap fades the picture to black, later ones keep the window black,
// and the next Swap fades back in. A false leaves the front buffer unpresented when the title moves its target off
// it. XBE_LOADING_SCREEN set: always true, every screen drawn and shown as on the console.
bool LoadingScreenWanted();
// milliseconds since the title last presented (steady clock); dsound shortens release envelopes the title waits on
// with the picture stopped
int64_t MsSincePresent();
// the title's Swaps so far
uint32_t Presents();

// The commands of a static push buffer read without the title (gpu/shader_gen.h): at va, size bytes of guest memory,
// as Walk reads the title's, their draws going to backend (the device's own put back after). fresh: the device's
// state, the chip's program memory and the inline textures reset first; setup then sets what the title's code would
// have before the buffer ran. Never while the title runs: the device's state is the title's
void DryWalk(std::unique_ptr<gpu::Backend>& backend, const std::function<void(gpu::State&)>& setup, bool fresh,
             uint32_t va, uint32_t size);

}  // namespace d3d
