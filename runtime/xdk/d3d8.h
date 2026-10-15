#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();

// A title whose library flushes deferred state itself (D3D_DirtyFlags) and has that flush replaced: the D3D states
// the flush would have pushed for these dirty bits take the chip back from the title's own raw writes.
void DeferredFlush(uint32_t dirty);

}  // namespace d3d
