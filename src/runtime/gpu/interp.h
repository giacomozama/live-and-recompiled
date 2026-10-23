#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gpu/state.h"

// Frame interpolation by drawing a frame's draws again with their transforms taken part way from the frame before's
// needs each draw found again in the frame before. A draw is known by what it draws (program, combiner, the shape of
// its targets, the vertex streams and textures it reads, its shape), and among equal ones by its turn in the frame;
// one whose streams or textures moved is found by the rest alone.
//
// XBE_INTERP_TRACE=1 measures how well that holds: every 120 frames it logs the draws found in the frame before, the
// ones only their vertex or texture addresses kept apart (dynamic buffers), the new ones, and which vertex program
// constants and fixed function transforms moved between the draws found.
namespace gpu::interp {

bool Tracing();
// a draw of the frame as it is drawn the first time, in order; indices as Backend::Draw takes them. same: nothing
// changed since the draw before it but vertex sources and constants (Backend::Changed)
void Draw(const State& s, bool same, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);
// the frame's draws are all in: each looked for in the frame before, before the first Blend (EndFrame does it if not)
void Match();
// the frame's draws are done: it becomes the frame before
void EndFrame();
// the frame's draw n drawn again at t between the frame before (0) and this one (1): its vertex constants and
// transforms in s taken that far from the draw it was found as (or that a neighbour found, of its program or
// another, moved from: kBorrowed).
// s untouched for a draw not found, or one with anything that jumped (a cut, a teleport). What it changed goes into
// undo, which puts it back
struct Undo {
    std::vector<std::pair<uint32_t, std::array<float, 4>>> vsc;
    std::vector<std::pair<uint32_t, std::array<float, 16>>> xf;
    void Restore(State& s) {
        for (const auto& [r, v] : vsc) s.vsc.v[r] = v;
        for (const auto& [i, m] : xf) s.transforms[i] = m;
        vsc.clear();
        xf.clear();
    }
};
enum class Blended { kNotFound, kJumped, kBorrowed, kYes };
Blended Blend(uint32_t n, float t, State& s, Undo& undo);
// XBE_INTERP_HIDE: whether a draw that came out so is left out of the picture drawn again; 16 leaves out the
// suspects too (likely found wrong or missed, interp.cpp's Suspicion), of the draw Blend did last
bool Hidden(Blended how);

// XBE_INTERP_RECORD=<pictures>: the pictures last shown are kept (at the console's size) with a note each, and F4
// (gpu::g_record_dump) saves them; 0 when off
uint32_t Recording();
// a picture's drawing again, when recording: Blend counts what it did between these, EndPass sums it up as the note
// for the picture's slot (Note). With XBE_INTERP_SUSPECT too: the picture's suspects, logged, noted, and saved with
// XBE_INTERP_AUTO
void BeginPass(float t);
void EndPass(uint32_t slot);
const std::string& Note(uint32_t slot);
// recording: whether the frame's draw n, about to be drawn again, still finds the vertices it was drawn with
void CheckVertices(uint32_t n, const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);

}  // namespace gpu::interp
