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
// from. A title calls LodTraceRun as it runs a push buffer (its data address, as the command stream jumps to it), to
// log the call chain that first ran it.
bool LodTrace();
void LodTraceRun(X86Context& ctx, uint32_t data);

}  // namespace d3d
