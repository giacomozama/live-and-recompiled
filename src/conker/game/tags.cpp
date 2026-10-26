// Frame generation's draw tags (gpu::interp::Tag): which object and which piece of its model each draw is, as the game
// knows it, so the draw is found again in the frame before by that rather than by how alike it looks.
//
// Conker draws every model through its scenegraph library. A model (a "scenegraph" resource) is a tree of nodes: the
// node's type at +0x10 (a 0x54-byte entry of the registry at 0x822670, name then the type's functions at +0x50), its
// first child at +0x14, its next sibling at +0x18. DrawModel(scenegraph, root, flags, data), called through
// [0x81BD70] (0x3F0270; the wrapper 0x3ED380 and some 29 copies of it inlined in the game's draw code), runs the tree
// in 0x3F01F0, a loop over a stack of (render function, node) at 0x81BE70, calling fn(node, data) for each node with
// the same data: the instance's (the bones ndMtxArray, 0x3EF6C0, builds its palette from). The nodes are the loaded
// model's, the same every frame; data is the object's. The types whose renderers push draws (the rest set state and
// pick material blocks) are hooked below, each tagging what it pushes with (data, node).
#include "cpu/x86_ops.h"

#include <cstdlib>

#include "app/app.h"
#include "gpu/interp.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace {

// with frame generation on, or its record or trace; XBE_INTERP_TAGS=0 leaves the draws untagged (the matching by
// distance alone, as before), to compare
bool Tagging() {
    static const bool off = [] {
        const char* e = std::getenv("XBE_INTERP_TAGS");
        return e && *e == '0';
    }();
    return !off && (app::Get().frame_generation > 1 || gpu::interp::Recording() || gpu::interp::Tracing());
}

// a node renderer fn(node, data), cdecl: what it pushes tagged with its node and the instance's data
void Tagged(X86Context& ctx, uint8_t* base, X86Func* original) {
    if (!Tagging()) {
        original(ctx, base);
        return;
    }
    uint32_t node = kernel::R32(ctx.esp + 4), data = kernel::R32(ctx.esp + 8);
    d3d::DrawTag before = d3d::SetDrawTag({data, node});
    original(ctx, base);
    d3d::SetDrawTag(before);
}

}  // namespace

#define CONKER_TAGGED(addr)                       \
    X86_EXTERN_FUNC(__imp__sub_##addr);           \
    X86_FUNC(sub_##addr) { Tagged(ctx, base, __imp__sub_##addr); }

CONKER_TAGGED(003F3510)  // ndStaticPushBuffer: runs the model's push buffer (0x53AE30)
CONKER_TAGGED(003F19E0)  // ndPushBuffer: runs a push buffer too
CONKER_TAGGED(003F31A0)  // ndResource: runs one from 0x3F31D7
CONKER_TAGGED(003F1DF0)  // ndVertexBuffer
CONKER_TAGGED(003F15C0)  // ndPatchPushBuffer
CONKER_TAGGED(003F2F90)  // ndBGPushBuffer: a level's
