#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();

// A title whose library flushes deferred state itself (D3D_DirtyFlags) and has that flush replaced: the D3D states
// the flush would have pushed for these dirty bits take the chip back from the title's own raw writes.
void DeferredFlush(uint32_t dirty);

// Reads every command the title has pushed so far. A title that patches a push buffer it already queued (the
// library's RunPushBuffer fixups) calls this first: the chip ran the earlier runs before the patch landed.
void FlushPending();

}  // namespace d3d
