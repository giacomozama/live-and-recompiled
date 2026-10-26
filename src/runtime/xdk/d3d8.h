#pragma once
#include <mutex>

#include "gpu/state.h"

struct X86Context;

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();

// A title whose library flushes deferred state itself (D3D_DirtyFlags) and has that flush replaced: the D3D states
// the flush would have pushed for these dirty bits take the chip back from the title's own raw writes.
void DeferredFlush(uint32_t dirty);

// Reads every command the title has pushed so far. A title that patches a push buffer it already queued (the
// library's RunPushBuffer fixups) calls this first: the chip ran the earlier runs before the patch landed.
void FlushPending();

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
struct DrawTag {
    uint32_t object = 0, piece = 0;
    bool group = false;
};
DrawTag SetDrawTag(DrawTag tag);

}  // namespace d3d
