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

// 0x313C20 (eax the player; the object's shadow state, its place, the object, ... on the stack): draws an object's
// shadow from the light (program 5BA33134, the frame's first draws), one draw a call. Its callers hand it a state of
// the object's own (object + 0x110 from 0x69141, character + 0x5A0 from 0x81067). Untagged, the characters' were found
// by distance, and in a fast turn one took another's: Conker's shadow was gone for a picture (capture 222234 pic 74).
// Some objects' shadows are drawn only every other frame; those are not found, and drawn as the frame has them
X86_EXTERN_FUNC(__imp__sub_00313C20);
X86_FUNC(sub_00313C20) {
    if (!Tagging()) {
        __imp__sub_00313C20(ctx, base);
        return;
    }
    d3d::DrawTag before = d3d::SetDrawTag({kernel::R32(ctx.esp + 4), 0x313C20});
    __imp__sub_00313C20(ctx, base);
    d3d::SetDrawTag(before);
}

// The shadows' receivers: the ground under an object, built again each frame (program 4A0744A9, new buffers and a
// count of its own every frame) and drawn with its shadow from the light (0x313C20's texture). 0x409300 works out the
// shadow's record (esi) and draws it in 0x408410 (called at 0x40935F). Untagged, a receiver whose count changed was
// not found and drawn as the frame has it, under the ground taken part way: most of Conker's shadow gone for a
// picture, the rest dimmed (captures 222752, 223340, 224344, about one picture in eight)
namespace {
d3d::DrawTag g_before_receiver;
bool g_in_receiver = false;
}  // namespace

// [[hook]] at 0x40935F, before the receiver's draw
void ConkerShadowReceiverDraw(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!Tagging()) return;
    g_before_receiver = d3d::SetDrawTag({ctx.esi, 0x408410});
    g_in_receiver = true;
}

// [[hook]] at 0x409364, after it
void ConkerShadowReceiverDrawn(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (!g_in_receiver) return;
    g_in_receiver = false;
    d3d::SetDrawTag(g_before_receiver);
}

// Character effects: the particles, sprites, splats and flares. A table of 127 types at 0x590650, 0x70 bytes each (the
// type's name at +8, CHAREFFECTS_TYPE_APD_FLOATSPRITE and the like; its draw at +0x50); the live effects of each type
// in a list at 0x7714E0 + type * 4 (the next at +0xC). The draw loop (0xD9A53) calls draw(effect, view, pass) for each
// at 0xD9AAF, the effect in esi, the draw in ebp. What it pushes is tagged as the effect's, a group (DrawTag::group):
// its particles come in a new order each frame (dust puffs), and are found among the effect's draws of the frame before
// by how near they are. A model an effect draws is tagged by its own nodes inside, as any model
namespace {
d3d::DrawTag g_before_effect;
bool g_in_effect = false;
}  // namespace

// [[hook]] at 0xD9AAF, before the effect's draw
void ConkerEffectDraw(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!Tagging()) return;
    g_before_effect = d3d::SetDrawTag({ctx.esi, ctx.ebp, true});
    g_in_effect = true;
}

// [[hook]] at 0xD9AB1, after it
void ConkerEffectDrawn(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (!g_in_effect) return;
    g_in_effect = false;
    d3d::SetDrawTag(g_before_effect);
}
