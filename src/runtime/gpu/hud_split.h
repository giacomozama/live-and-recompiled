#pragma once
// g_hud_split for the renderers (vulkan.cpp, d3d11.cpp): which of a frame's draws are the HUD's (interp::g_run_tag's
// scopes), the box each element takes on the guest frame, and the x remap a draw of one is drawn with
#include <cstdint>
#include <unordered_map>

#include "gpu/draw.h"

namespace gpu {

// a box on the guest frame, and the edge an element keeps to (kHudEdge)
enum HudAnchor : uint8_t { kHudNoEdge, kHudLeft, kHudMiddle, kHudRight };
struct HudBox {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool Empty() const { return x1 < x0; }
    void Add(const HudBox& o) {
        x0 = std::min(x0, o.x0), y0 = std::min(y0, o.y0);
        x1 = std::max(x1, o.x1), y1 = std::max(y1, o.y1);
    }
    float Mid() const { return (x0 + x1) * 0.5f; }
};
// one HUD element (a scope's id) as the frames draw it: its draws' box this frame and the last, the box of its first
// draw in each (how far it moved), and the edge it keeps to
struct HudElement {
    HudBox now, last, first_now, first_last;
    HudAnchor anchor = kHudNoEdge;
    uint32_t against = 0;  // frames in a row its box has been nearer another edge
    uint32_t unseen = 0;   // frames since it last drew
};
constexpr uint32_t kHudVertices = 256;  // a draw of more isn't measured (it is drawn with its element's place)
// an element keeps its edge until its box has been nearer another this many frames in a row
constexpr uint32_t kHudTurnFrames = 30;
constexpr uint32_t kHudForget = 120;  // an element not drawn for this many frames is forgotten

// the edge a box keeps nearest to: its left side to the frame's left, its right side to the right, or its middle to
// the middle. An element grows away from the edge it keeps to (a kill message longer than the last, a sniper's marks
// left of the weapon panel), which moves its middle but not its near side
HudAnchor HudEdge(const HudBox& b, float gw);

// a draw's box on the guest frame (guest_w across), worked out as the console's would be: pretransformed as they are; a
// program's that moves its position from the input (Conker's sprites: oPos = v0, already on the screen) as they are
// too, one's that takes w from a dp4 with c[n] through c[n-3]..c[n] and the viewport's scale and offset (c-38, c-37);
// the fixed function's through world, view, projection and the viewport. pos_w: the program's PositionWSource
// (kPosWUnknown without one). False for a draw not in screen space
bool HudMeasure(const State& s, const Assembled& a, uint32_t start, uint32_t count, const uint8_t* indices, int pos_w,
                float guest_w, HudBox& b);

// one renderer's HUD elements and the remap of the draw being drawn
struct HudSplit {
    // the draw's x on the guest frame drawn at x * scale + shift; bars: and black down the sides it no longer covers
    // (kHudInPlaceBars)
    float scale = 1, shift = 0;
    bool bars = false;
    HudBox box;       // the last placed draw's box, and the place it took (for captures)
    char took = 'S';
    bool rolled = false;  // the frame's boxes are its elements' last already, for the frame's drawing again
    std::unordered_map<uint32_t, HudElement> elements;  // by their scope's id

    // each draw starts as the frame has it
    void Reset() {
        scale = 1, shift = 0;
        bars = false;
    }
    // whether the draw is one of the HUD's (in a HUD scope, onto the frame: target_gw by target_gh the guest size of
    // the target it is drawn on, guest_w by guest_h the frame's), and if so the remap it is drawn with, by its scope's
    // HudMode (interp::g_run_tag):
    // - kHudEdge: the element's edge (the one its box keeps nearest to, held kHudTurnFrames: Roll), the frame's shape there
    // - kHudInPlace: the frame's shape about the middle of the element's box, its box of the frame before moved as its
    //   first draw moved since (a crosshair carried to a target); in its first frame, its first draw's box
    // - kHudInPlaceBars: kHudInPlace, and a draw covering the frame's width gets black down the sides it leaves
    // - kHudInPlaceEach: the frame's shape about the middle of each draw's own box
    // - kHudStretched: as the frame has it
    // - kHudCentred: every draw (2D and models alike) in the frame's shape about the screen's middle, so a 4:3 frame's
    //   screen of 2D sits unstretched there; with g_hud_split off too
    // A draw not in screen space (a model) is drawn with its element's place but adds nothing to its box. replay: the
    // frame drawn again (its own boxes are last already, Roll)
    bool Place(const State& s, const Assembled& a, uint32_t start, uint32_t count, const uint8_t* indices, int pos_w,
               uint32_t target_gw, uint32_t target_gh, uint32_t guest_w, uint32_t guest_h, bool replay);
    // the frame's HUD elements: this frame's boxes become their last, and each keeps its edge until its box has been in
    // another third kHudTurnFrames frames in a row; at the frame's end, or before it is drawn again
    void Roll(uint32_t guest_w);
};

}  // namespace gpu
