// Animated model parts (0x89B70) cache named bone indices in a shared callback
// descriptor. The cache's initialized byte alone cannot identify the model:
// multiplayer turrets and the single-player beehive reuse descriptor 0x6A7B14,
// but BARREL_ROTATE is bone 5 in the former and bone 6 in the latter. After an
// MP -> SP switch the barrel callback otherwise rotates FOOTREST (bone 5).
// Resolve the names again when the model changes, and after a level load even
// if the allocator reuses a model's address. Leave the game's rotation and
// animation timing unchanged.
#include <unordered_map>

#include "cpu/x86_ops.h"

namespace {
std::unordered_map<uint32_t, uint32_t> g_part_models;
}

X86_EXTERN_FUNC(__imp__sub_00089B70);
X86_FUNC(sub_00089B70) {
    const uint32_t descriptor = ctx.esi;
    const uint32_t scene = X86_MEM32(ctx.esp + 4);
    const uint32_t names = X86_MEM32(scene + 0x10);
    auto [entry, inserted] = g_part_models.try_emplace(descriptor, names);
    if (inserted || entry->second != names) {
        X86_MEM8(descriptor + 0x10) = 0;
        entry->second = names;
    }
    __imp__sub_00089B70(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_002A96E0);
X86_FUNC(sub_002A96E0) {
    g_part_models.clear();
    __imp__sub_002A96E0(ctx, base);
}
