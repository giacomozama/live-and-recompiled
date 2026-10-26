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
// A draw's identity as the title knows it (RT64's interpolation ids): the object drawing it (object: the instance data
// a Conker model's renderers are handed), the piece of its model (piece: a scenegraph node), and key, those with the
// draw's place in the piece's push buffer and its turn among the frame's draws of that place. A draw is found by its
// key in the frame before, ahead of any guess by distance. key 0: the title said nothing (particles, text)
struct Tag {
    uint64_t key = 0;
    uint32_t object = 0, piece = 0;
};
// the tag of the draw the title's thread hands the backend next (d3d::SetDrawTag), taken with it as it is recorded
inline Tag g_draw_tag;
// a draw of the frame as it is drawn the first time, in order; indices as Backend::Draw takes them. same: nothing
// changed since the draw before it but vertex sources and constants (Backend::Changed)
void Draw(const State& s, bool same, const Tag& tag, uint32_t prim, uint32_t start, uint32_t count,
          const uint8_t* indices);
// the frame's draws are all in: each looked for in the frame before, before the first Blend (EndFrame does it if not)
void Match();
// the frame's Present showed the surface whose memory is at data: a back buffer, whose memory rotates, so its draws
// are found by the target's shape alone, other targets' by their address too
void Presented(uint32_t data);
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
    // a stream MoveVertices pointed at vertices moved part way, and the base vertex it had (stream 16: none)
    uint32_t stream = 16, vb = 0, base_vertex = 0;
    void Restore(State& s) {
        for (const auto& [r, v] : vsc) s.vsc.v[r] = v;
        for (const auto& [i, m] : xf) s.transforms[i] = m;
        vsc.clear();
        xf.clear();
        if (stream < 16) {
            s.streams[stream].vb = vb;
            s.base_vertex = base_vertex;
            stream = 16;
        }
    }
};
enum class Blended { kNotFound, kJumped, kBorrowed, kYes };
Blended Blend(uint32_t n, float t, State& s, Undo& undo);
// A 2D draw the game places on the CPU (a speech bubble, a marker over a character) moves in its vertices, not its
// constants: the frame's draw n, after Blend came out how, drawn with its positions at t from the draw of the frame
// before it was found as, when both are small, untagged and laid out alike. s's stream then reads a copy in the ring
// MoveRing gave (start, or s's base vertex for an indexed draw, made to fit it); undo puts them back. False: untouched
void MoveRing(uint32_t data, uint32_t data_size, uint32_t headers, uint32_t headers_size);
bool MoveVertices(uint32_t n, float t, Blended how, State& s, uint32_t& start, Undo& undo);
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
