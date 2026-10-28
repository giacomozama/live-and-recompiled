#include "gpu/interp.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <emmintrin.h>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu::interp {

namespace {

constexpr uint32_t kResData = 4;           // a resource header's data address
constexpr uint32_t kLogEvery = 120;        // frames
constexpr uint32_t kDataHashBytes = 4096;  // of each stream, to tell vertices the CPU wrote again
constexpr uint32_t kIndicesHashed = 64;
constexpr int kTransforms[] = {2, 3, 256};  // view, projection, world
constexpr size_t kXf = std::size(kTransforms);

uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

uint64_t HashBytes(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, b + i, 8);
        h = Mix(h, v);
    }
    for (; i < n; i++) h = Mix(h, b[i]);
    return h;
}

// what a draw's keys are made of, to tell why one was not found
enum Part { kCombiner, kTarget, kDepth, kShape, kStart, kIndices, kAddresses, kParts };
constexpr const char* kPartNames[kParts] = {"combiner", "target", "depth target", "primitive or count", "start",
                                            "indices", "addresses"};

struct Keys {
    uint64_t loose = 0;  // program, combiner, targets' shape, the draw's shape
    uint64_t lod = 0;    // all but the draw's count, its addresses too: the same mesh at any detail
    uint64_t mesh = 0;   // all but its count, start and indices: the same buffers drawn with other indices
    uint64_t data = 0;   // the start of each stream's bytes, when tracing or recording
    uint64_t program = 0;
    std::array<uint64_t, kParts> parts{};
    std::array<uint32_t, 4> textures{};  // the memory of the textures it samples, by stage, for the watch lines
};

// the back buffers' memory, as presented (Presented): their surfaces stay put, their memory rotates every Swap
std::vector<uint32_t> g_presented;

Keys MakeKeys(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    Keys k;
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    if (prog) EnsureProgramHash(s);
    k.program = prog ? s.vs->hash : s.vs_handle;
    k.parts[kCombiner] = s.ps ? s.ps->handle : 0;
    // the back buffers by shape, not address: their memory rotates every Swap (d3d8.cpp). Other targets by address
    // too: each character's 32x32 lighting map (21751DC8, four lights a draw) is drawn by turns, one character's
    // some frames and another's the next, and found as the other's its lights blended toward another's, dulling a
    // goblin's armour in every picture drawn again
    bool back = std::find(g_presented.begin(), g_presented.end(), s.rt.data) != g_presented.end();
    k.parts[kTarget] = Mix(uint64_t(s.rt.format) << 32 | s.rt.size, back ? 0 : s.rt.data);
    k.parts[kDepth] = uint64_t(s.zs.format) << 32 | s.zs.size;
    k.parts[kShape] = uint64_t(prim) << 32 | count;
    k.parts[kStart] = start;
    k.parts[kIndices] = indices ? HashBytes(0, indices, size_t(std::min(count, kIndicesHashed)) * 2) : 0;
    uint64_t h = Mix(0, k.program);
    for (int i = 0; i < kAddresses; i++) h = Mix(h, k.parts[size_t(i)]);
    k.loose = h;
    uint64_t addresses = 0;

    uint32_t used = StreamsRead(s);
    // the vertices it covers: from start, or the indices' from the base vertex to the highest
    uint32_t first = start, verts = count;
    if (indices && (Tracing() || Recording())) {
        uint16_t top = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t v;
            std::memcpy(&v, indices + i * 2, 2);
            top = std::max(top, v);
        }
        first = s.base_vertex;
        verts = count ? top + 1u : 0u;
    }
    uint64_t d = 0;
    for (uint32_t i = 0; i < 16; i++) {
        const State::Stream& st = s.streams[i];
        if (!(used >> i & 1) || !st.vb) continue;
        uint32_t va = mem::VirtualFromPhysical(kernel::R32(st.vb + kResData));
        addresses = Mix(addresses, uint64_t(va) << 32 | st.stride);
        if (!Tracing() && !Recording()) continue;
        uint32_t n = std::min(kDataHashBytes, verts * st.stride);
        d = HashBytes(Mix(d, i), kernel::HostPtr(va + first * st.stride), n);
    }
    uint32_t sampled = SampledStages(s);
    for (uint32_t i = 0; i < 4; i++) {
        TextureDesc td;
        if ((sampled >> i & 1) && s.textures[i] && Describe(s.textures[i], td)) {
            addresses = Mix(addresses, uint64_t(td.data) << 2 | i);
            k.textures[i] = td.data;
        }
    }
    k.parts[kAddresses] = addresses;
    uint64_t l = Mix(0, k.program);
    for (int i = 0; i < kParts; i++) l = Mix(l, i == kShape ? k.parts[size_t(i)] >> 32 : k.parts[size_t(i)]);
    k.lod = l;
    uint64_t m = Mix(0, k.program);
    for (int i : {kCombiner, kTarget, kDepth, kAddresses}) m = Mix(m, k.parts[size_t(i)]);
    k.mesh = Mix(m, k.parts[kShape] >> 32);
    k.data = d;
    return k;
}

uint64_t ExactKey(uint64_t program, const std::array<uint64_t, kParts>& parts) {
    uint64_t h = Mix(0, program);
    for (uint64_t p : parts) h = Mix(h, p);
    return h;
}

// the constant registers a program reads (ProgramReads), as a list; from relative up, a palette read relative to a0
// (ProgramRelativeBase), of which a draw uses the entries its vertices pick
struct Reads {
    std::bitset<192> bits;
    std::vector<uint8_t> regs;
    uint32_t relative = 192;
};

const Reads* ReadsOf(const State& s) {
    if (!((s.vs_handle & 1) && s.vs && !s.vs->program.empty())) return nullptr;
    static std::unordered_map<uint64_t, Reads> reads;
    auto [it, fresh] = reads.try_emplace(s.vs->hash);
    if (fresh) {
        it->second.bits = ProgramReads(s.vs->program);
        it->second.relative = ProgramRelativeBase(s.vs->program);
        for (size_t r = 0; r < 192; r++)
            if (it->second.bits[r]) it->second.regs.push_back(uint8_t(r));
    }
    return &it->second;
}

// what may move between frames: the constants its program reads, kept in the frame's pool at at, one for each of
// reads->regs; or the fixed function's transforms
struct Snap {
    int32_t leader = -1;  // a draw in the state of the one before it, with its constants: the draw it follows
    const Reads* reads = nullptr;  // none for the fixed function, which has xf instead
    uint32_t at = 0;
    std::array<std::array<float, 16>, kXf> xf;
    uint64_t data = 0;
    uint64_t program = 0;
    std::array<uint64_t, kParts> parts{};
    std::bitset<192> stale;  // palette registers (Reads::relative up) another draw left: none of this one's (Stale)
    int32_t found = -1;  // the draw of the frame before it was found as
    float distance = 0;  // from it, per value read (Distance)
    float next = INFINITY;  // the runner-up's, per value: as near as distance, the draw was a toss-up (Suspect)
    float gone = -1;        // not found: the nearest draw of its program no draw found, per value; -1 not yet weighed
    uint32_t candidates = 0;  // draws it was found among
    bool by_exact = false;
    bool second = false;  // found only as another detail's draw (Match)
    bool by_tag = false;  // found by its tag (Match)
    bool grouped = false;  // of a group the frame before had too: found among its draws or not at all (Match)
    uint64_t lod = 0;     // Keys::lod
    uint64_t mesh = 0;    // Keys::mesh
    Tag tag;
    uint32_t target = 0;  // its target's memory, for the watch lines
    std::array<uint32_t, 4> textures{};  // Keys::textures
    // a small untagged draw's vertices as it read them (Frame::raw, from raw_at): verts of them from vertex first of
    // its stream, its position (v0) at offset in each (MoveVertices); verts 0 for none kept
    uint32_t raw_at = 0, verts = 0, first = 0;
    uint16_t stride = 0, offset = 0;
    uint8_t stream = 0, type = 0;
    bool indexed = false;
    // why its kept vertices are drawn as the frame has them and not moved part way (Settle): null to move them, ""
    // for a reason not worth a watch line
    const char* still = "";
};

static_assert(std::is_trivially_destructible_v<Snap>, "a frame's snaps cleared without a destructor each");

// a frame's draws of each key, kept from the last time the frame was used: a battle's frame made and freed tens of
// thousands of entries each time. Each list cleared, a key no draw had that time taken out
using Lists = std::unordered_map<uint64_t, std::vector<uint32_t>>;
void Reuse(Lists& m) {
    for (auto it = m.begin(); it != m.end();) {
        if (it->second.empty()) {
            it = m.erase(it);
        } else {
            it->second.clear();
            ++it;
        }
    }
}

struct Frame {
    // the draws of each key. loose made again each time: Match goes through it in its order, which a kept map's
    // history would change
    std::unordered_map<uint64_t, std::vector<uint32_t>> loose, programs, lod, mesh;
    std::unordered_map<uint64_t, std::vector<uint32_t>> exact;  // of all its parts (Measure)
    // the tagged draws (Tag::key), a follower's as its leader: the draw with its constants
    TagTable tags;
    std::unordered_map<uint64_t, std::vector<uint32_t>> groups;  // the draws of each group's tag (Tag::group)
    // once matched, the found draws Borrow and BorrowAcross go out to: of each program, their turns in programs; of
    // the frame, those with a program. A run of draws not found (a battle's blood, in new buffers each frame) went
    // through each other draw of the run for each, in every picture drawn again
    std::unordered_map<uint64_t, std::vector<uint32_t>> found_turns;
    std::vector<uint32_t> found_programmed;
    std::vector<Snap> snaps;  // with nothing to free: a frame's tens of thousands cleared at once
    std::vector<std::array<float, 4>> values;  // the snaps' constants
    std::vector<uint8_t> raw;                  // their vertices, of those kept (Snap::raw_at)
    // of each draw not found, when recording or tracing: what keeps it from the nearest draw of its program (Why)
    std::unordered_map<uint32_t, std::string> why;
    const std::string& WhyNot(uint32_t n) const {
        static const std::string none;
        auto it = why.find(n);
        return it != why.end() ? it->second : none;
    }
    std::vector<bool> taken;  // found by a draw of the frame after
    bool matched = false;     // its draws looked for in the frame before (Match)
    const std::array<float, 4>* Values(const Snap& sn) const { return values.data() + sn.at; }
    void Clear() {
        matched = false;
        loose.clear();
        Reuse(programs);
        Reuse(lod);
        Reuse(mesh);
        Reuse(exact);
        tags.Clear();
        Reuse(groups);
        Reuse(found_turns);
        found_programmed.clear();
        snaps.clear();
        values.clear();
        raw.clear();
        why.clear();
        taken.clear();
    }
};

// how far apart two draws' values are, each difference against its size: the same object a frame later is near,
// another object drawn alike (a second soldier, sorted into another turn) is not. Only the constants the program reads:
// the rest hold what the draw before left (often another character's bones). Past limit it stops counting: the caller
// has a nearer one already. a and b are of one program, being alike
float Rel(float u, float v) {
    if (u == v || !std::isfinite(u) || !std::isfinite(v)) return 0;
    return std::fabs(u - v) / (1.0f + std::fabs(u) + std::fabs(v));
}
// Rel of four values at once, each as Rel has it, times w; added into d in turn, as four calls would. Distance was
// most of the matching's time
void AddRel4(const float* u, const float* v, float w, float& d) {
    const __m128 sign = _mm_set1_ps(-0.0f), one = _mm_set1_ps(1.0f), big = _mm_set1_ps(FLT_MAX);
    __m128 a = _mm_loadu_ps(u), b = _mm_loadu_ps(v);
    __m128 au = _mm_andnot_ps(sign, a), av = _mm_andnot_ps(sign, b);
    __m128 q = _mm_div_ps(_mm_andnot_ps(sign, _mm_sub_ps(a, b)), _mm_add_ps(_mm_add_ps(one, au), av));
    __m128 counted = _mm_and_ps(_mm_cmpneq_ps(a, b), _mm_and_ps(_mm_cmple_ps(au, big), _mm_cmple_ps(av, big)));
    alignas(16) float r[4];
    _mm_store_ps(r, _mm_mul_ps(_mm_and_ps(counted, q), _mm_set1_ps(w)));
    d += r[0];
    d += r[1];
    d += r[2];
    d += r[3];
}
// The palette registers another draw left (Stale) count a quarter: whole, another character's bones in them took a
// character for its look-alike; left out, an eye (its own upload a register, its face's bones left before it) was
// as near to the other squirrel's eye as to its own. all: they count whole
constexpr float kStaleWeight = 0.25f;
float Distance(const Frame& fa, const Snap& a, const Frame& fb, const Snap& b, float limit, bool all = false) {
    float d = 0;
    if (b.reads) {
        const std::array<float, 4>* x = fa.Values(a);
        const std::array<float, 4>* y = fb.Values(b);
        for (size_t j = 0; j < b.reads->regs.size(); j++) {
            uint8_t r = b.reads->regs[j];
            float w = !all && (a.stale[r] || b.stale[r]) ? kStaleWeight : 1.0f;
            AddRel4(x[j].data(), y[j].data(), w, d);
            if (d > limit) return d;
        }
        return d;
    }
    for (size_t m = 0; m < kXf; m++)
        for (size_t i = 0; i < 16; i += 4) AddRel4(&a.xf[m][i], &b.xf[m][i], 1.0f, d);
    return d;
}

// the nearest of the draws listed under key in the frame before that no draw found yet, into sn, one with the same
// addresses ahead of an equally near one; false for none. turn is the draw's among its kind this frame: the search
// goes out from the same turn and weighs kTries draws not taken, as a crowd of alike draws (a battle's soldiers, a
// ground drawn a quad at a time) weighed each against all would cost the square of their number every frame. One the
// same in every value and address ends it
constexpr uint32_t kTries = 16;
bool Nearest(const Frame& prev, const Frame& cur, const std::unordered_map<uint64_t, std::vector<uint32_t>>& by,
             uint64_t key, size_t turn, Snap& sn) {
    auto it = by.find(key);
    if (it == by.end()) return false;
    const std::vector<uint32_t>& list = it->second;
    int32_t best = -1;
    float best_d = 0, next_d = INFINITY;
    bool best_same = false;
    uint32_t n = 0;
    size_t from = std::min(turn, list.size() - 1);
    for (size_t step = 0; n < kTries && step < 2 * list.size(); step++) {
        // from, from + 1, from - 1, from + 2, ...
        size_t off = (step + 1) / 2;
        if (step % 2 ? from + off >= list.size() : off > from) continue;
        uint32_t i = list[step % 2 ? from + off : from - off];
        if (prev.taken[i]) continue;
        n++;
        float d = Distance(prev, prev.snaps[i], cur, sn, next_d);
        bool same = prev.snaps[i].parts[kAddresses] == sn.parts[kAddresses];
        if (best < 0 || d < best_d || (d == best_d && same && !best_same)) {
            if (best >= 0) next_d = best_d;
            best = int32_t(i), best_d = d, best_same = same;
        } else if (d < next_d) {
            next_d = d;
        }
        if (best_d == 0 && best_same) break;
    }
    if (best < 0) return false;
    size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
    sn.found = best;
    sn.distance = values ? best_d / float(values) : 0;
    sn.next = values ? next_d / float(values) : INFINITY;
    sn.candidates = n;
    return true;
}

// what keeps a draw from the draws of its program in the frame before: the parts the nearest by parts differs in
std::string Why(const Frame& prev, const Snap& sn) {
    auto it = prev.programs.find(sn.program);
    if (it == prev.programs.end() || it->second.empty()) return "its program was not drawn";
    int best = kParts + 1;
    uint32_t mask = 0;
    bool all_taken = true;
    for (size_t j = 0; j < std::min<size_t>(it->second.size(), 64); j++) {
        uint32_t i = it->second[j];
        const Snap& o = prev.snaps[i];
        uint32_t m = 0;
        for (int p = 0; p < kParts; p++)
            if (o.parts[size_t(p)] != sn.parts[size_t(p)]) m |= 1u << p;
        int n = __builtin_popcount(m);
        if (n < best) best = n, mask = m;
        all_taken &= prev.taken[i];
    }
    if (!mask) return all_taken ? "its program's draws were all found by others" : "?";
    std::string o = "differs in";
    for (int p = 0; p < kParts; p++)
        if (mask >> p & 1) o += std::string(" ") + kPartNames[p] + ",";
    o.pop_back();
    return o;
}

struct Totals {
    uint64_t frames = 0, draws = 0, found = 0, moved = 0, fresh = 0, gone = 0;
    uint64_t still = 0, rewritten = 0;  // of the found: no constant or transform moved; vertices written again
    std::array<uint64_t, 192> reg{};    // found draws whose constant moved, per register (hardware order)
    std::array<uint64_t, kXf> xf{};
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> unfound;  // program: moved, new
};

Frame g_frames[2];
uint32_t g_cur = 0;
Totals g_t;

// a picture drawn again, for its note: its draws, those not found (by program), those with registers that jumped
struct Pass {
    bool on = false;
    float t = 0;
    uint32_t draws = 0, unfound = 0, jumped = 0, borrowed = 0, across = 0, kept = 0, second = 0;
    uint32_t tagged = 0, by_tag = 0;  // draws the title tagged, those found by their tag (Match)
    uint32_t moved = 0;               // draws whose vertices were moved part way (MoveVertices)
    uint32_t offscreen = 0;           // draws into a target not presented, drawn as the frame has them (OffscreenKept)
    std::unordered_map<uint64_t, std::pair<uint32_t, std::string>> unfound_programs;  // count, the first's reason
    std::string jumps;  // the first draws with a jump: draw, program, registers
    std::string watched;  // XBE_INTERP_WATCH's program's draws (Watch)
    std::unordered_map<uint64_t, std::pair<float, std::string>> farthest;  // per program, its draw farthest from what it was found as
    std::unordered_map<uint64_t, uint32_t> jump_lines;                     // per program, draws listed in jumps
    uint32_t rewritten = 0;  // draws whose vertices changed since the frame drew them, by program
    std::unordered_map<uint64_t, uint32_t> rewritten_programs;
};
Pass g_pass;
std::string g_notes[8];
// frames ended; the frame scored (Suspects::first), logged; pictures drawn again this frame. XBE_INTERP_AUTO's saves
// done, pictures to wait before the next, the frame it may come from
uint32_t g_frame = 0, g_scored = ~0u, g_logged = 0, g_passes = 0;
uint32_t g_pass_id = 0;  // pictures drawn again, ever: a leader's blend kept for its followers is of one (BlendDraw)
uint32_t g_saves = 0, g_save_in = 0, g_save_after = 0;

uint64_t ProgramOf(const State& s) {
    return (s.vs_handle & 1) && s.vs && !s.vs->program.empty() ? s.vs->hash : s.vs_handle;
}

// registers with counts as ranges: "c96-98 1200, c204-251 340"
std::string Ranges(const std::array<uint64_t, 192>& reg) {
    std::string o;
    char b[48];
    for (uint32_t i = 0; i < 192;) {
        if (!reg[i]) {
            i++;
            continue;
        }
        uint32_t j = i;
        uint64_t most = 0;
        while (j < 192 && reg[j]) most = std::max(most, reg[j++]);
        if (j - 1 > i) snprintf(b, sizeof(b), "%sc%u-%u %llu", o.empty() ? "" : ", ", i, j - 1, (unsigned long long)most);
        else snprintf(b, sizeof(b), "%sc%u %llu", o.empty() ? "" : ", ", i, (unsigned long long)most);
        o += b;
        i = j;
    }
    return o.empty() ? "none" : o;
}

void Log() {
    const Totals& t = g_t;
    double n = double(std::max<uint64_t>(t.frames, 1));
    LOGR("interp: %llu frames, %.0f draws a frame: %.1f%% found, %.1f%% addresses moved, %.1f%% new, %.0f gone a frame",
         (unsigned long long)t.frames, t.draws / n, 100.0 * t.found / std::max<uint64_t>(t.draws, 1),
         100.0 * t.moved / std::max<uint64_t>(t.draws, 1), 100.0 * t.fresh / std::max<uint64_t>(t.draws, 1), t.gone / n);
    LOGR("interp: of the found, %.1f%% still, %.1f%% with vertices written again", 100.0 * t.still / std::max<uint64_t>(t.found, 1),
         100.0 * t.rewritten / std::max<uint64_t>(t.found, 1));
    LOGR("interp: constants moved (most draws per register): %s", Ranges(t.reg).c_str());
    LOGR("interp: transforms moved: view %llu, projection %llu, world %llu", (unsigned long long)t.xf[0],
         (unsigned long long)t.xf[1], (unsigned long long)t.xf[2]);
    std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> top(t.unfound.begin(), t.unfound.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        return a.second.first + a.second.second > b.second.first + b.second.second;
    });
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); i++)
        LOGR("interp:   not found: program %016llX, %.1f a frame with moved addresses, %.1f new",
             (unsigned long long)top[i].first, top[i].second.first / n, top[i].second.second / n);
}

void Trace(const Frame& cur, const Snap& sn, const Frame& prev, const Snap* was, bool exact, uint64_t program) {
    if (!was) {
        g_t.fresh++;
        g_t.unfound[program].second++;
        return;
    }
    if (!exact) {
        g_t.moved++;
        g_t.unfound[program].first++;
        return;
    }
    g_t.found++;
    bool moved = false;
    if (sn.reads) {
        const std::array<float, 4>* x = cur.Values(sn);
        const std::array<float, 4>* y = prev.Values(*was);
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            if (std::memcmp(&x[j], &y[j], 16) == 0) continue;
            g_t.reg[sn.reads->regs[j]]++;
            moved = true;
        }
    } else {
        for (size_t i = 0; i < kXf; i++) {
            if (std::memcmp(&sn.xf[i], &was->xf[i], sizeof(sn.xf[i])) == 0) continue;
            g_t.xf[i]++;
            moved = true;
        }
    }
    if (!moved) g_t.still++;
    if (sn.data != was->data) g_t.rewritten++;
}

// a value moved so far in a frame that it went somewhere else rather than on its way: a camera cut, a teleport, a
// flag or index; the scale grows with the value, which keeps a large coordinate's walk, and with reach for a
// translation (Reach)
bool Jumped(float a, float b, float reach) {
    return std::fabs(b - a) > 1.0f + 0.5f * std::max({std::fabs(a), std::fabs(b), reach});
}

// the largest translation of a draw's in either frame, for judging its translations by: one near the camera's axis
// crosses zero as the camera turns, a walk its own size would call a jump (a character's bones mid pan, which then
// stood still while the room turned). Past kPacked a value is packed data or a flag, no coordinate
constexpr float kPacked = 1e5f;
// a value that is packed data: past kPacked, or an integer's bits read as a float (a denormal: 8 is 1.1e-44)
bool Packed(float v) { return !(std::fabs(v) < kPacked) || std::fpclassify(v) == FP_SUBNORMAL; }
float Reach(float r, float v) { return std::isfinite(v) && std::fabs(v) < kPacked ? std::max(r, std::fabs(v)) : r; }
// a fixed function transform's: its fourth row, the translation, by the largest of it in either
std::array<float, 16> Reach16(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    float m = 0;
    for (size_t k = 12; k < 15; k++) m = Reach(Reach(m, a[k]), b[k]);
    return {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, m, m, m, 0};
}

// n floats at t from a to b, into out (which may be b); false when one jumped, out untouched. A value that stayed
// the same, bit for bit, never jumped: some registers hold packed data that isn't a number. reach: a value's
// translation reach, none for nullptr
bool Lerp(const float* a, const float* b, size_t n, float t, float* out, const float* reach = nullptr) {
    for (size_t i = 0; i < n; i++) {
        if (std::memcmp(&a[i], &b[i], 4) == 0) continue;
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || Jumped(a[i], b[i], reach ? reach[i] : 0.0f)) return false;
    }
    for (size_t i = 0; i < n; i++)
        if (std::memcmp(&a[i], &b[i], 4) != 0) out[i] = a[i] + (b[i] - a[i]) * t;
    return true;
}

// a constant register that went somewhere else rather than on its way, as a row of a matrix the program takes with
// dp4: its xyz (a rotation's row, its scale) by length, which a turn keeps, against the longer; its w (a
// translation) against the draw's reach (Reach) too. A row turned past about 120 degrees, or a translation that more
// than doubled or crossed zero further than the draw reaches, jumped. The w alone misjudged a bone turning about a
// pivot far from its mesh (an eye's: its translation halves while the eye barely moves)
bool RowJumped(const float* a, const float* b, float reach) {
    float la = std::hypot(a[0], a[1], a[2]), lb = std::hypot(b[0], b[1], b[2]);
    float d = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
    if (d > 1.0f + std::max(la, lb)) return true;
    // no row: a value grown many times over, further than the draw reaches (a scale or count, 63 one frame and
    // 4028 the next)
    for (size_t i = 0; i < 3; i++)
        if (std::fabs(b[i] - a[i]) > 1.0f + reach + 0.5f * std::max(std::fabs(a[i]), std::fabs(b[i]))) return true;
    return std::fabs(b[3] - a[3]) > 1.0f + std::max({std::fabs(a[3]), std::fabs(b[3]), reach});
}

// a constant register holding a counter the game wraps: one value of it moved by a whole number, nearly, the rest
// stayed. Its program can't tell the two apart (or the frame would pop), but taken part way it goes the long way
// round: the tunnel walls' phase (A7E3951C c114.w, -0.25 up to 0.75 and back by 1) lit them flat for a picture
bool Wrapped(const float* a, const float* b) {
    int moved = -1;
    for (int i = 0; i < 4; i++) {
        if (std::memcmp(&a[i], &b[i], 4) == 0) continue;
        if (moved >= 0) return false;
        moved = i;
    }
    if (moved < 0 || std::fabs(a[moved]) > 1.0f || std::fabs(b[moved]) > 1.0f) return false;
    float d = b[moved] - a[moved], whole = std::round(d);
    return whole != 0 && std::fabs(d - whole) < 0.05f;
}

// a constant register the game zeroes for a slot it leaves off and fills when it uses it: the characters' third group
// of four blob shadows (c154-c157: x, y, the height they cast below, their weights) is all zeros some frames and
// casters thousands of units away the next. Taken part way, a blob at half its weight lay half way to the origin, a
// shadow cast by nothing (capture 170648 pics 98-104). Only the whole register, bit for bit zero (either sign)
bool Cleared(const float* a, const float* b) {
    auto zero = [](const float* v) {
        for (int i = 0; i < 4; i++)
            if (v[i] != 0.0f) return false;
        return true;
    };
    return zero(a) != zero(b);
}

constexpr float kFlipNear = 0.2f;
constexpr uint8_t kFlip = 2;
// a row of a matrix the game gave negated: a shadow's projection (5342E94B, a dinosaur's flattened onto the floor),
// homogeneous, is the same picture either sign, and the game turns it over now and then. Taken part way through zero
// the shadow went flat and was gone for a picture (captures 160718 pic 52, 092739 pic 14). Its xyz (a turn of 180
// degrees in a frame, not a move) and its whole row near opposite; taken from the negated row instead
bool Flipped(const float* a, const float* b) {
    float sum = std::hypot(a[0] + b[0], a[1] + b[1], a[2] + b[2]);
    float diff = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
    if (diff < 1e-3f || sum > kFlipNear * diff) return false;
    float sum4 = std::hypot(sum, a[3] + b[3]), diff4 = std::hypot(diff, b[3] - a[3]);
    return sum4 <= kFlipNear * diff4;
}

}  // namespace

bool Tracing() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_INTERP_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}

// the palette registers a draw's program reads that another draw left there. A character uploads the palette entries
// its draws pick before them, all at once: the registers that changed since the draw before, along with the last of
// the palette to change. Those that changed before then hold another character's bones, which move unlike this one's
// (or by a different camera) and say nothing of whether the draw was found right or moved too far. Every draw of the
// frame, followed or not, sees the changes: in the blocks written since the draw before (s.vsc.dirty, the render
// thread's), not all 192 each draw, which was most of the matching's time with a battle's 16000 draws
std::bitset<192> Stale(const State& s, const Reads* reads) {
    static std::array<std::array<float, 4>, 192> last{};
    static std::array<uint32_t, 192> changed{};
    static uint32_t draw = 0;
    draw++;
    using Vsc = decltype(s.vsc);
    for (uint64_t dirty = std::exchange(s.vsc.dirty, 0) & Vsc::kAll; dirty; dirty &= dirty - 1) {
        size_t lo = size_t(__builtin_ctzll(dirty)) * Vsc::kBlock;
        for (size_t r = lo; r < std::min(lo + Vsc::kBlock, Vsc::size()); r++) {
            if (std::memcmp(&last[r], &s.vsc.v[r], 16) == 0) continue;
            last[r] = s.vsc.v[r];
            changed[r] = draw;
        }
    }
    std::bitset<192> stale;
    if (!reads || reads->relative >= 192) return stale;
    uint32_t upload = 0;
    for (uint8_t r : reads->regs)
        if (r >= reads->relative) upload = std::max(upload, changed[r]);
    for (uint8_t r : reads->regs)
        if (r >= reads->relative && changed[r] < upload) stale.set(r);
    return stale;
}

// a draw in the state of the draw before it (but for vertex sources and constants) whose constants are that draw's
// too: it moves as that one does (a ground drawn a quad at a time is thousands of these), and is found as it is. Not
// a skinned draw (a palette, Reads::relative): a character's piece follows the piece before it only in the frames no
// bone of its own was uploaded between them, and a follower is in no list of its frame, so the frame after had nothing
// to find it as. Conker's ear pieces (D293CFF5, counts 94/126/128) drew at the frame's own pose, or with a
// neighbour's bones, for a picture: a crack behind the ear (captures 114704, 120029, 120259)
bool Follows(const Frame& cur, const State& s, const Reads* reads, int32_t& leader) {
    if (cur.snaps.empty() || (reads && reads->relative < 192)) return false;
    leader = cur.snaps.back().leader >= 0 ? cur.snaps.back().leader : int32_t(cur.snaps.size() - 1);
    const Snap& l = cur.snaps[size_t(leader)];
    if (l.reads) {
        const std::array<float, 4>* v = cur.Values(l);
        for (size_t j = 0; j < l.reads->regs.size(); j++)
            if (std::memcmp(&v[j], &s.vsc[l.reads->regs[j]], 16) != 0) return false;
        return true;
    }
    for (size_t i = 0; i < kXf; i++)
        if (std::memcmp(&l.xf[i], &s.transforms[size_t(kTransforms[i])], sizeof(l.xf[i])) != 0) return false;
    return true;
}

// A 2D draw the game places on the CPU (a speech bubble over a character, a marker) writes its vertices where they are
// on the screen each frame, with constants that never change: blended as a model is, it stayed where the frame drew
// it, at 60 in a picture at 120. Its vertices are kept as it read them, to be moved part way (MoveVertices): an
// untagged draw (a tagged one is a model's, whose mesh stays put) of at most kMoveVertices vertices from one stream,
// its position floats. XBE_INTERP_VERTICES=0 keeps none
constexpr uint32_t kMoveVertices = 64;
bool Moving() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_INTERP_VERTICES");
        return !e || *e != '0';
    }();
    return on;
}
void KeepVertices(Frame& cur, Snap& sn, const State& s, uint32_t start, uint32_t count, const uint8_t* indices) {
    if ((sn.tag.key && !sn.tag.group) || !count || !Moving() || count > (indices ? 6 * kMoveVertices : kMoveVertices)) return;
    InputAt at;
    if (!InputPlace(s, 0, at) || !at.alone || (at.type & 15) != 2 || !(at.type >> 4) || (at.type >> 4) > 4 ||
        !at.stride || at.stride > 0xFFFF || at.offset + (at.type >> 4) * 4 > at.stride)
        return;
    uint32_t first = start, last = start + count - 1;
    if (indices) {
        uint16_t lo = 0xFFFF, hi = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t v;
            std::memcpy(&v, indices + i * 2, 2);
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        first = s.base_vertex + lo, last = s.base_vertex + hi;
    }
    if (last - first >= kMoveVertices) return;
    uint32_t va = mem::VirtualFromPhysical(kernel::R32(s.streams[at.stream].vb + kResData));
    uint32_t bytes = (last - first + 1) * at.stride;
    sn.raw_at = uint32_t(cur.raw.size());
    sn.verts = last - first + 1;
    sn.first = first;
    sn.stride = uint16_t(at.stride);
    sn.offset = uint16_t(at.offset);
    sn.stream = uint8_t(at.stream);
    sn.type = uint8_t(at.type);
    sn.indexed = indices != nullptr;
    const uint8_t* src = static_cast<const uint8_t*>(kernel::HostPtr(va + first * at.stride));
    AppendBytes(cur.raw, src, bytes);
}

// the draw of the frame before a draw with kept vertices is moved from: the one it was found as; a follower's, the
// draw as far on from its leader's. Null when there is none, or it read other vertices
const Snap* MovedFrom(const Frame& prev, const Frame& cur, uint32_t n) {
    const Snap& sn = cur.snaps[n];
    const Snap* was = nullptr;
    if (sn.leader < 0) {
        if (sn.found >= 0) was = &prev.snaps[size_t(sn.found)];
    } else if (int32_t lf = cur.snaps[size_t(sn.leader)].found; lf >= 0) {
        size_t q = size_t(lf) + (n - uint32_t(sn.leader));
        if (q < prev.snaps.size() && prev.snaps[q].leader == lf) was = &prev.snaps[q];
    }
    if (!was || was->verts != sn.verts || was->stride != sn.stride || was->offset != sn.offset || was->type != sn.type)
        return nullptr;
    return was;
}

// Whether a draw's kept vertices may move part way from was's, and the box they cover in either frame. Only the same
// sprite carried along whole. A 2D draw's constants never change, so it was found by its turn alone, and that pairs it
// with another sprite whenever the frame's turns shift: a loading bar's blocks, a block taken part way toward its
// neighbour, were drawn scrambled (capture 214722 pics 114-116). So all but its positions the same, byte for byte
// (texture coordinates, colours), and every vertex moved by the same step: a sprite that changes its shape (the
// peephole's circle, closing) went part way while what fits around it didn't, showing the scene through the seams
// (capture 214710 pic 70). A step further than the draw's reach is a jump (a bubble shown somewhere else)
struct SpriteBox {
    uint32_t n = 0, target = 0;
    std::array<float, 2> lo{}, hi{};
    float slack = 0;
    bool shaped = false;  // held for its shape: the sprites it touches are held with it (Settle)
};
const char* Judge(const Frame& prev, const Frame& cur, const Snap& sn, const Snap& was, SpriteBox& box) {
    uint32_t comps = sn.type >> 4, pos_end = sn.offset + comps * 4;
    const uint8_t* ra = prev.raw.data() + was.raw_at;
    const uint8_t* rb = cur.raw.data() + sn.raw_at;
    const uint8_t* a = ra + sn.offset;
    const uint8_t* b = rb + sn.offset;
    for (uint32_t v = 0; v < sn.verts; v++) {
        const uint8_t* va = ra + v * sn.stride;
        const uint8_t* vb = rb + v * sn.stride;
        if (std::memcmp(va, vb, sn.offset) != 0 || std::memcmp(va + pos_end, vb + pos_end, sn.stride - pos_end) != 0)
            return "other values than its position changed (another sprite)";
    }
    float reach = 0;
    bool moved = false;
    std::array<float, 4> step{};
    box.lo.fill(INFINITY), box.hi.fill(-INFINITY);
    for (uint32_t v = 0; v < sn.verts; v++)
        for (uint32_t c = 0; c < comps; c++) {
            float x, y;
            std::memcpy(&x, a + v * sn.stride + c * 4, 4);
            std::memcpy(&y, b + v * sn.stride + c * 4, 4);
            if (!std::isfinite(x) || !std::isfinite(y)) return "not a number";
            reach = Reach(Reach(reach, x), y);
            moved |= x != y;
            if (!v) step[c] = y - x;
            if (c < 2) box.lo[c] = std::min({box.lo[c], x, y}), box.hi[c] = std::max({box.hi[c], x, y});
        }
    if (comps < 2) box.lo.fill(INFINITY), box.hi.fill(-INFINITY);  // no place on the screen: touches nothing
    box.slack = 1e-4f * (1.0f + reach);
    for (uint32_t v = 0; v < sn.verts; v++)
        for (uint32_t c = 0; c < comps; c++) {
            float x, y;
            std::memcpy(&x, a + v * sn.stride + c * 4, 4);
            std::memcpy(&y, b + v * sn.stride + c * 4, 4);
            if (std::fabs((y - x) - step[c]) > box.slack) {
                box.shaped = true;
                return "not moved whole (its shape changed)";
            }
            if (Jumped(x, y, reach)) return "jumped";
        }
    return moved ? nullptr : "";
}

// The frame's draws with kept vertices judged once, before its pictures are drawn again (Snap::still). A sprite moved
// whole that touches one held for its shape is held too, and so on along what touches it: the peephole is its circle
// and four pieces fitting around it, and the circle carried along whole while the pieces shrank went part way from
// them, a sliver of the scene through the seam (capture 132017 pics 66, 68, 74)
void Settle(const Frame& prev, Frame& cur) {
    static std::vector<SpriteBox> boxes;
    boxes.clear();
    for (uint32_t n = 0; n < cur.snaps.size(); n++) {
        Snap& sn = cur.snaps[n];
        if (!sn.verts) continue;
        const Snap* was = MovedFrom(prev, cur, n);
        SpriteBox box;
        sn.still = was ? Judge(prev, cur, sn, *was, box) : "";
        if (sn.still && !box.shaped) continue;
        box.n = n;
        box.target = sn.leader < 0 ? sn.target : cur.snaps[size_t(sn.leader)].target;
        boxes.push_back(box);
    }
    auto touch = [](const SpriteBox& p, const SpriteBox& q) {
        float e = std::max(p.slack, q.slack);
        return p.target == q.target && p.lo[0] <= q.hi[0] + e && q.lo[0] <= p.hi[0] + e && p.lo[1] <= q.hi[1] + e &&
               q.lo[1] <= p.hi[1] + e;
    };
    for (bool more = true; more;) {
        more = false;
        for (SpriteBox& p : boxes) {
            if (p.shaped) continue;
            for (const SpriteBox& q : boxes)
                if (q.shaped && touch(p, q)) {
                    p.shaped = more = true;
                    cur.snaps[p.n].still = "beside a sprite held for its shape";
                    break;
                }
        }
    }
}

void Draw(const State& s, bool same, const Tag& tag, uint32_t prim, uint32_t start, uint32_t count,
          const uint8_t* indices) {
    Frame& cur = g_frames[g_cur];
    if (tag.key && !tag.group) cur.tags.Prefetch(tag.key);  // looked up after the work below
    int32_t leader = -1;
    const Reads* reads = ReadsOf(s);
    std::bitset<192> stale = Stale(s, reads);
    if (same && !Tracing() && Follows(cur, s, reads, leader)) {
        Snap& f = cur.snaps.emplace_back();
        f.leader = leader;
        f.tag = tag;
        if (tag.key && !tag.group) cur.tags[tag.key] = uint32_t(leader);
        KeepVertices(cur, f, s, start, count, indices);  // a string's letters follow its first
        return;
    }
    Keys k = MakeKeys(s, prim, start, count, indices);
    uint32_t n = uint32_t(cur.snaps.size());
    Snap& sn = cur.snaps.emplace_back();
    sn.reads = reads;
    sn.stale = stale;
    sn.tag = tag;
    sn.target = s.rt.data;
    sn.textures = k.textures;
    if (tag.key && !tag.group) cur.tags[tag.key] = n;
    else if (tag.key) cur.groups[tag.key].push_back(n);
    KeepVertices(cur, sn, s, start, count, indices);
    if (sn.reads) {
        sn.at = uint32_t(cur.values.size());
        for (uint8_t r : sn.reads->regs) cur.values.push_back(s.vsc[r]);
    } else {
        for (size_t i = 0; i < kXf; i++) sn.xf[i] = s.transforms[size_t(kTransforms[i])];
    }
    sn.data = k.data;
    sn.program = k.program;
    sn.parts = k.parts;
    sn.lod = k.lod;
    sn.mesh = k.mesh;
    cur.loose[k.loose].push_back(n);
    cur.programs[k.program].push_back(n);
    cur.lod[k.lod].push_back(n);
    cur.mesh[k.mesh].push_back(n);
    cur.exact[ExactKey(k.program, k.parts)].push_back(n);
}

// the frame's draws found in the frame before, once its draws are all in: among the draws alike but for their
// addresses (a dynamic buffer's draw is still the same draw), the nearest pairs first, of all of a kind at once. Each
// draw taking its nearest in turn left the last of a kind what the others didn't take: a text's letters took their
// drop shadow's, and the shadow the letters'. Same addresses ahead of other as near, then the same turn. A crowd too
// large to weigh each against all goes a draw at a time (Nearest)
constexpr size_t kPairs = 4096;
constexpr float kLooseCost = 3.0f;
bool Suspecting();
// whether the frame before has a draw of the same mesh at its addresses, at any detail, no draw took yet: the draw is
// that one, not another's at other addresses (a clump drawing fewer blades took a clump of that many). Only one that
// kept its addresses from the frame before it (Snap::by_exact): a dynamic buffer's addresses say nothing, a fly cloud
// written where the other cloud was a frame before missed its own and the two clouds were drawn averaged
bool OwnElsewhere(const Frame& prev, const Snap& sn) {
    auto it = prev.lod.find(sn.lod);
    if (it == prev.lod.end()) return false;
    for (uint32_t p : it->second)
        if (!prev.taken[p] && prev.snaps[p].by_exact) return true;
    return false;
}
constexpr size_t kSecondTries = 256;
constexpr float kSecondDistance = 0.01f;
constexpr float kSecondMargin = 0.5f;  // the nearest at most this of the next nearest  // per value; a draw found right is under 0.01, another object's past 0.03
// whether the frame before has a draw of the program that no draw found yet, of another kind than those weighed with
// it (another count: Keys::loose), much nearer than d: the draw is that one at another detail, found by the passes
// after (another detail's). Two fly clouds in dynamic buffers each change their count; one took the other cloud's of
// its own count, and the two were drawn averaged
bool NearerElsewhere(const Frame& prev, const Frame& cur, const Snap& sn, float d) {
    auto it = prev.programs.find(sn.program);
    if (it == prev.programs.end()) return false;
    for (size_t k = 0; k < std::min<size_t>(it->second.size(), kSecondTries); k++) {
        uint32_t p = it->second[k];
        const Snap& o = prev.snaps[p];
        if (prev.taken[p] || o.leader >= 0) continue;
        if (std::equal(o.parts.begin(), o.parts.begin() + kAddresses, sn.parts.begin())) continue;
        if (Distance(prev, o, cur, sn, kSecondMargin * d) < kSecondMargin * d) return true;
    }
    return false;
}
// whether a pair at other addresses is a toss-up: another draw of the frame before, not the taken one's twin, is about
// as near (kClear). Dust puffs (2F4A85DC, a dynamic buffer, two quads each, c176 the puff's place) are drawn in a new
// order each frame, move about as far as they lie apart, and the puffs a dinosaur kicked up took the dying puffs
// of the other dinosaur's cloud, 1200 units off, and were drawn between the two (capture 093233 pics 16, 34, 42).
// A twin is a draw all but the same as the taken one, per value (kTwin): a puff's other quad, 10 units over (0.00003;
// two puffs 50 units apart are 0.002). Not as against how far the pair are apart: the puffs of a cloud 600 units off
// were all twins then, and a cloud took the other's again (093658 pic 112). Left not found, the draw is drawn as the
// frame has it
constexpr float kClear = 0.6f;
constexpr float kTwin = 0.0005f;
bool TossUp(const Frame& prev, const std::vector<uint32_t>& before, const float* d, uint32_t p, size_t values) {
    float twin = kTwin * float(values);
    for (uint32_t q = 0; q < before.size(); q++) {
        if (q == p || d[q] * kClear >= d[p]) continue;
        if (Distance(prev, prev.snaps[before[q]], prev, prev.snaps[before[p]], twin) >= twin) return true;
    }
    return false;
}
// as TossUp, for a crowd matched a draw at a time (Nearest): its rivals weighed as they come, out from the one taken
// in the list, kSecondTries of them. The dust puffs of two dinosaurs are such a crowd
bool TossUpIn(const Frame& prev, const Frame& cur, const Snap& sn, const std::vector<uint32_t>& before, uint32_t at,
              float d, size_t values) {
    float twin = kTwin * float(values);
    size_t from = size_t(std::find(before.begin(), before.end(), at) - before.begin());
    uint32_t tries = 0;
    for (size_t step = 1; tries < kSecondTries && step < 2 * before.size(); step++) {
        // from + 1, from - 1, from + 2, ...
        size_t off = (step + 1) / 2;
        if (step % 2 ? from + off >= before.size() : off > from) continue;
        uint32_t q = before[step % 2 ? from + off : from - off];
        tries++;
        if (Distance(prev, prev.snaps[q], cur, sn, d / kClear) * kClear >= d) continue;
        if (Distance(prev, prev.snaps[q], prev, prev.snaps[at], twin) >= twin) return true;
    }
    return false;
}
// per value, the farthest a draw at other addresses (a dynamic buffer's) is taken as: such draws found right are
// under 0.02; a string of text fading out took the one fading in (0.037) and was drawn over it, in a still menu. As
// far as kMotionSlack times how far its program's draws found at their own addresses moved (Motion), or the frame's
// for a program with none: in a fast pan, draws found at their own addresses were 0.03 apart, and a ground whose
// buffers all moved that frame (0.027) was drawn unblended, sky showing through its seams. Not the frame's for all:
// in the menu its draws that moved most (a view drawn every other frame, characters) took it to 0.11, and text took
// another string again. The frame's is its 90th percentile, not its median: most of the training level's draws hardly
// move in a fast turn, the median held the limit at 0.025 while the camera's draws moved 0.04, and the ground's
// pieces, every one in new buffers, went unblended again (capture 081202 pics 62-84). Nor only by draw: a character's
// skinned pieces, many and each with many registers that hardly move, held it under 0.02 while the camera's draws
// moved 0.05, and the ground's pieces (0.027) went unblended (capture 194651 pics 48-94); so the 90th percentile of
// its programs' medians too, if higher
constexpr float kLooseDistance = 0.025f;
constexpr float kMotionSlack = 1.25f;
float g_loose = kLooseDistance;  // the frame's (Match), for the notes
// how far the frame's draws moved, per value: the 90th percentile of those the frame before has at the same addresses,
// in every part, its draws that moved (a HUD that didn't says nothing of the camera), or of its programs that moved,
// the higher; and each program's, the median of all of its such draws, those that stayed too
struct Motion {
    float frame = 0;
    std::unordered_map<uint64_t, float> program;
    float Of(uint64_t p) const {
        auto it = program.find(p);
        return it != program.end() ? it->second : frame;
    }
};
float Percentile(std::vector<float>& d, size_t tenths) {
    if (d.empty()) return 0;
    auto at = d.begin() + ptrdiff_t(d.size() * tenths / 10);
    std::nth_element(d.begin(), at, d.end());
    return *at;
}
void Measure(const Frame& prev, const Frame& cur, Motion& m) {
    static std::vector<float> moved;
    static std::unordered_map<uint64_t, std::vector<float>> by;
    moved.clear();
    for (auto& [p, d] : by) d.clear();
    m.program.clear();
    for (const Snap& sn : cur.snaps) {
        if (sn.leader >= 0) continue;
        auto it = prev.exact.find(ExactKey(sn.program, sn.parts));
        if (it == prev.exact.end()) continue;
        for (uint32_t p : it->second) {
            if (prev.snaps[p].parts != sn.parts || prev.snaps[p].program != sn.program) continue;
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            float v = values ? Distance(prev, prev.snaps[p], cur, sn, INFINITY) / float(values) : 0;
            if (v > 0) moved.push_back(v);
            by[sn.program].push_back(v);
            break;
        }
    }
    m.frame = Percentile(moved, 9);
    static std::vector<float> programs;
    programs.clear();
    for (auto& [p, d] : by) {
        if (d.empty()) continue;
        float v = m.program[p] = Percentile(d, 5);
        if (v > 0) programs.push_back(v);
    }
    m.frame = std::max(m.frame, Percentile(programs, 9));
}

// copies of a mesh at the same addresses (a room's lamps: 9E03A2D4, 4EDA4F42 in the hall of lasers) are told apart by
// where they are, a translation each. A fast turn moves each further than they lie apart, and nearest pairs first took
// a lamp for the one beside it: c96.w 66, 182, 299, 415 a frame before were 30, 92, 153, 215, and 92 took 415. The
// near lamp's beam was drawn halfway to another lamp, gone for a picture (capture 205559 pics 32-38, 68-74, 100-106).
// The camera moves all the copies alike, by one affine map of each value: of the pairs as found and the same draws
// paired in their turns (the game draws a room's copies in one order), the one the map fits clearly better. Two pairs
// fit any map
constexpr size_t kInTurnMin = 3;
constexpr double kInTurnBetter = 0.5;  // the turns' misfit at most this of the pairs' as found
// how badly the best affine map of each value from the frame before to this one fits pairs (draws of the frame, of
// the frame before): the share of the value's spread it leaves unexplained, summed. Packed values and palette
// registers another draw left say nothing of where a copy is
double Misfit(const Frame& prev, const Frame& cur, const std::vector<std::pair<uint32_t, uint32_t>>& pairs) {
    const Snap& first = cur.snaps[pairs[0].first];
    double n = double(pairs.size()), misfit = 0;
    for (size_t j = 0; j < first.reads->regs.size(); j++) {
        if (first.stale[first.reads->regs[j]]) continue;
        for (size_t k = 0; k < 4; k++) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
            bool numbers = true;
            for (auto [c, p] : pairs) {
                float x = prev.Values(prev.snaps[p])[j][k], y = cur.Values(cur.snaps[c])[j][k];
                if (Packed(x) || Packed(y)) {
                    numbers = false;
                    break;
                }
                sx += x, sy += y, sxx += double(x) * x, sxy += double(x) * y, syy += double(y) * y;
            }
            double cxx = sxx - sx * sx / n, cxy = sxy - sx * sy / n, cyy = syy - sy * sy / n;
            if (!numbers || cyy <= 1e-9 * (1.0 + syy)) continue;
            double left = cxx > 1e-9 * (1.0 + sxx) ? cyy - cxy * cxy / cxx : cyy;
            misfit += std::max(0.0, left) / cyy;
        }
    }
    return misfit;
}
// a kind's pairs at the same addresses (found[c], -1 for none or at other addresses), paired again in their turns
// where that fits the camera's move clearly better (Misfit); dist and two as Match has them
void InTurns(const Frame& prev, Frame& cur, const std::vector<uint32_t>& list, const std::vector<uint32_t>& before,
             const std::vector<int32_t>& found, const std::vector<float>& dist,
             const std::vector<std::array<float, 4>>& two) {
    static std::unordered_map<uint64_t, std::vector<uint32_t>> by;  // turns in list, by addresses
    for (auto& [a, v] : by) v.clear();
    for (uint32_t c = 0; c < list.size(); c++)
        if (found[c] >= 0 && cur.snaps[list[c]].reads) by[cur.snaps[list[c]].parts[kAddresses]].push_back(c);
    static std::vector<std::pair<uint32_t, uint32_t>> as_found, in_turns;
    static std::vector<uint32_t> ps;
    for (const auto& [a, cs] : by) {
        if (cs.size() < kInTurnMin) continue;
        ps.clear();
        as_found.clear();
        for (uint32_t c : cs) {
            ps.push_back(uint32_t(found[c]));
            as_found.emplace_back(list[c], before[size_t(found[c])]);
        }
        std::sort(ps.begin(), ps.end());
        bool moved = false;
        in_turns.clear();
        for (size_t i = 0; i < cs.size(); i++) {
            moved |= ps[i] != uint32_t(found[cs[i]]);
            in_turns.emplace_back(list[cs[i]], before[ps[i]]);
        }
        if (!moved || Misfit(prev, cur, in_turns) >= kInTurnBetter * Misfit(prev, cur, as_found)) continue;
        for (size_t i = 0; i < cs.size(); i++) {
            Snap& sn = cur.snaps[list[cs[i]]];
            size_t values = sn.reads->regs.size() * 4;
            float d = dist[cs[i] * before.size() + ps[i]];
            const float* t = two[cs[i]].data();
            sn.found = int32_t(before[ps[i]]);
            sn.distance = values ? d / float(values) : 0;
            sn.next = values ? (d == t[0] ? t[1] : t[0]) / float(values) : INFINITY;
        }
    }
}

void Match() {
    Frame& cur = g_frames[g_cur];
    Frame& prev = g_frames[g_cur ^ 1];
    if (cur.matched) return;
    cur.matched = true;
    static Motion motion;
    Measure(prev, cur, motion);
    g_loose = std::max(kLooseDistance, kMotionSlack * motion.frame);
    auto loose = [&](const Snap& sn) { return std::max(kLooseDistance, kMotionSlack * motion.Of(sn.program)); };
    struct Pair {
        float d;
        bool same;
        uint32_t turns, c, p;  // how far apart their turns are; the draws' turns
    };
    static std::vector<Pair> pairs;
    static std::vector<bool> used;
    static std::vector<float> dist;  // each pair's, by draw then draw of the frame before (TossUp)
    // each draw's two nearest, at the same addresses ([0], [1]) and at others ([2], [3]): its runner-up is the
    // nearer of those alike to the one it took (Snap::next)
    static std::vector<std::array<float, 4>> two;
    static std::vector<int32_t> same_found;  // each draw's turn in before, found at the same addresses (InTurns)
    const bool diagnosing = Recording() || Tracing() || Suspecting();
    // a tagged draw is the draw of the frame before with its tag (Tag::key), as the title says, however near another
    // looks: look-alikes (two squirrels' eyes, a room's lamps) are no longer told apart by distance. Only of the same
    // program, whose registers it blends. The passes after find the rest: draws the title tags nothing of, a model at
    // another detail (another piece), an object new to the frame
    constexpr size_t kAhead = 8;  // draws whose tag's slot is fetched before it is looked up
    for (size_t k = 0; k < cur.snaps.size(); k++) {
        if (k + kAhead < cur.snaps.size()) prev.tags.Prefetch(cur.snaps[k + kAhead].tag.key);
        Snap& sn = cur.snaps[k];
        if (sn.leader >= 0 || !sn.tag.key || sn.tag.group) continue;
        const uint32_t* at = prev.tags.Find(sn.tag.key);
        if (!at) continue;
        const Snap& o = prev.snaps[*at];
        // and into the same target (its address too, but for a presented back buffer: MakeKeys). An object drawn into
        // a texture another object's went to the frame before is read through by draws that know nothing of the blend:
        // the characters' shadows from the light (5342E94B, the frame's first) taken part way lost part of Conker's
        // shadow, and dimmed others (capture 222752 pics 24, 26, 104). Drawn as the frame has it, as before tags
        if (o.program != sn.program || o.reads != sn.reads || o.parts[kTarget] != sn.parts[kTarget]) continue;
        prev.taken[*at] = true;
        sn.found = int32_t(*at);
        // how far it moved, for the notes and the suspects only: weighed for each of a battle's 16000 draws, it was
        // most of the matching's time
        if (diagnosing) {
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            sn.distance = values ? Distance(prev, o, cur, sn, INFINITY) / float(values) : 0;
        }
        sn.candidates = 1;
        sn.by_tag = true;
    }
    // a group's draws (an effect's particles, in a new order each frame) are found among its draws in the frame before,
    // nearest pairs first, as near as a draw at other addresses may be (loose); those left are new to it (a puff born)
    // and those of the frame before left were gone (one died). Neither is taken for another group's or an untagged
    // draw: the dust a dinosaur kicked up took the other dinosaur's dying puffs, drawn between the two clouds
    struct GroupPair {
        float d;
        uint32_t c, p;
    };
    static std::vector<GroupPair> group_pairs;
    static std::vector<bool> group_used;
    for (const auto& [key, list] : cur.groups) {
        auto it = prev.groups.find(key);
        if (it == prev.groups.end()) continue;
        const std::vector<uint32_t>& before = it->second;
        if (list.size() * before.size() > kPairs) continue;  // a crowd too large: the passes by distance alone
        group_pairs.clear();
        for (uint32_t c = 0; c < list.size(); c++) {
            const Snap& sn = cur.snaps[list[c]];
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            float limit = values ? 1.01f * loose(sn) * float(values) : INFINITY;
            for (uint32_t p = 0; p < before.size(); p++) {
                const Snap& o = prev.snaps[before[p]];
                if (o.program != sn.program || o.reads != sn.reads || o.parts[kTarget] != sn.parts[kTarget] ||
                    prev.taken[before[p]])
                    continue;
                float d = Distance(prev, o, cur, sn, limit);
                if (!values || d / float(values) <= loose(sn)) group_pairs.push_back({d, c, p});
            }
        }
        std::sort(group_pairs.begin(), group_pairs.end(), [](const GroupPair& a, const GroupPair& b) {
            return a.d != b.d ? a.d < b.d : a.c != b.c ? a.c < b.c : a.p < b.p;
        });
        group_used.assign(list.size(), false);
        for (const GroupPair& g : group_pairs) {
            uint32_t p = before[g.p];
            if (group_used[g.c] || prev.taken[p]) continue;
            Snap& sn = cur.snaps[list[g.c]];
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            group_used[g.c] = true;
            prev.taken[p] = true;
            sn.found = int32_t(p);
            sn.distance = values ? g.d / float(values) : 0;
            sn.candidates = uint32_t(before.size());
            sn.by_tag = true;
        }
        for (uint32_t c : list) cur.snaps[c].grouped = true;
        for (uint32_t p : before) prev.taken[p] = true;
    }
    for (const auto& [key, list] : cur.loose) {
        auto it = prev.loose.find(key);
        if (it == prev.loose.end()) continue;
        const std::vector<uint32_t>& before = it->second;
        if (list.size() * before.size() > kPairs) {
            for (size_t k = 0; k < list.size(); k++) {
                Snap& sn = cur.snaps[list[k]];
                if (sn.found >= 0 || sn.grouped || !Nearest(prev, cur, prev.loose, key, k, sn)) continue;
                if (prev.snaps[size_t(sn.found)].parts[kAddresses] != sn.parts[kAddresses]) {
                    size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
                    if (sn.distance > loose(sn) ||
                        TossUpIn(prev, cur, sn, before, uint32_t(sn.found), sn.distance * float(values), values)) {
                        sn.found = -1;
                        continue;
                    }
                }
                prev.taken[size_t(sn.found)] = true;
            }
            continue;
        }
        pairs.clear();
        two.assign(list.size(), {INFINITY, INFINITY, INFINITY, INFINITY});
        dist.resize(list.size() * before.size());
        used.assign(list.size(), false);
        for (uint32_t c = 0; c < list.size(); c++) {
            const Snap& sn = cur.snaps[list[c]];
            if (sn.found >= 0 || sn.grouped) {  // by its tag, or among its group's
                used[c] = true;
                continue;
            }
            // a pair at other addresses past loose is never taken, and TossUp weighs only those within loose / kClear:
            // past that its distance stops counting (the runner-up, Snap::next, may be such a part sum). Weighed in
            // full, a cutscene's crowds took most of the matching's time
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            float far = values ? 1.01f * loose(sn) * float(values) / kClear : INFINITY;  // clear of rounding
            for (uint32_t p = 0; p < before.size(); p++) {
                const Snap& o = prev.snaps[before[p]];
                bool same = o.parts[kAddresses] == sn.parts[kAddresses];
                const Pair& pr = pairs.emplace_back(Pair{Distance(prev, o, cur, sn, same ? INFINITY : far), same,
                                                         c > p ? c - p : p - c, c, p});
                dist[c * before.size() + p] = pr.d;
                float* t = two[c].data() + (pr.same ? 0 : 2);
                if (pr.d < t[0]) t[1] = t[0], t[0] = pr.d;
                else if (pr.d < t[1]) t[1] = pr.d;
            }
        }
        // at other addresses a pair counts kLooseCost times as far: a static mesh's copies (grass clumps) are told
        // apart by their buffers, at distances among them like their own's a frame on; one at other addresses still
        // wins when much nearer, a dynamic buffer's draw moving round the ring
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
            float ca = a.same ? a.d : a.d * kLooseCost, cb = b.same ? b.d : b.d * kLooseCost;
            if (ca != cb) return ca < cb;
            if (a.same != b.same) return a.same;
            return a.turns < b.turns;
        });
        same_found.assign(list.size(), -1);
        for (const Pair& pr : pairs) {
            uint32_t p = before[pr.p];
            if (used[pr.c] || prev.taken[p]) continue;
            Snap& sn = cur.snaps[list[pr.c]];
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            if (!pr.same && values && pr.d / float(values) > loose(sn)) continue;
            if (!pr.same && (OwnElsewhere(prev, sn) || NearerElsewhere(prev, cur, sn, pr.d) ||
                             TossUp(prev, before, &dist[pr.c * before.size()], pr.p, values)))
                continue;
            used[pr.c] = true;
            prev.taken[p] = true;
            sn.found = int32_t(p);
            sn.distance = values ? pr.d / float(values) : 0;
            const float* t = two[pr.c].data() + (pr.same ? 0 : 2);
            float next = pr.d == t[0] ? t[1] : t[0];
            sn.next = values ? next / float(values) : INFINITY;
            sn.candidates = uint32_t(before.size());
            if (pr.same) same_found[pr.c] = int32_t(pr.p);
        }
        InTurns(prev, cur, list, before, same_found, dist, two);
    }
    // a draw still not found that is a draw of the frame before but for its count (the same buffers, start, first
    // indices and kind of primitive) is that draw at another detail: a grass clump drawing fewer blades as the camera
    // moves off. The nearest such that no draw found. Then, as well, one that is but for its count, start and indices:
    // the same buffers drawn with an index list built again, a ground's pieces as the camera turns (cracked from the
    // pieces found beside them). Pieces sharing buffers are told apart by distance; alike, any is drawn the same
    auto by_detail = [&](const std::unordered_map<uint64_t, std::vector<uint32_t>>& by, uint64_t Snap::*key) {
        for (Snap& sn : cur.snaps) {
            if (sn.leader >= 0 || sn.found >= 0 || sn.grouped) continue;
            auto it = by.find(sn.*key);
            if (it == by.end()) continue;
            float best = INFINITY, next = INFINITY;
            int32_t at = -1;
            for (uint32_t p : it->second) {
                if (prev.taken[p]) continue;
                float d = Distance(prev, prev.snaps[p], cur, sn, next);
                if (d < best) next = best, best = d, at = int32_t(p);
                else if (d < next) next = d;
            }
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            // as near as loose: a dynamic buffer's ring can put another draw at the same addresses
            if (at < 0 || (values && best / float(values) > loose(sn))) continue;
            prev.taken[size_t(at)] = true;
            sn.found = at;
            sn.distance = values ? best / float(values) : 0;
            sn.next = values ? next / float(values) : INFINITY;
            sn.candidates = uint32_t(it->second.size());
            sn.second = true;
        }
    };
    by_detail(prev.lod, &Snap::lod);
    by_detail(prev.mesh, &Snap::mesh);
    // a draw still not found takes the nearest draw of its program that no draw found, if it is near: a skinned
    // mesh drawn at another detail (a grass clump's blades, a character's other vertices, the same bones) leaves the
    // draws of the detail before unfound. Only a skinned draw (a palette, Reads::relative), and near by its own bones
    // (Distance: those another draw left count a quarter): copies of a mesh share the camera and the light, and near by
    // all it reads a clump new to the frame took another, drawn halfway between the two. Only the clearly nearest: an
    // eye's own bones are a register or two, alike in both squirrels
    for (Snap& sn : cur.snaps) {
        if (sn.leader >= 0 || sn.found >= 0 || sn.grouped || !sn.reads || sn.reads->relative >= 192) continue;
        auto it = prev.programs.find(sn.program);
        if (it == prev.programs.end()) continue;
        float best = INFINITY, next = INFINITY;
        int32_t at = -1;
        for (size_t k = 0; k < std::min<size_t>(it->second.size(), kSecondTries); k++) {
            uint32_t p = it->second[k];
            if (prev.taken[p]) continue;
            float d = Distance(prev, prev.snaps[p], cur, sn, next);
            if (d < best) next = best, best = d, at = int32_t(p);
            else if (d < next) next = d;
        }
        size_t values = sn.reads->regs.size() * 4;
        if (at < 0 || best / float(values) > kSecondDistance || best > kSecondMargin * next) continue;
        sn.found = at;
        sn.distance = best / float(values);
        sn.next = next / float(values);
        sn.candidates = uint32_t(it->second.size());
        sn.second = true;
    }
    for (Snap& sn : cur.snaps) {
        if (sn.leader >= 0) continue;
        sn.by_exact = sn.found >= 0 && prev.snaps[size_t(sn.found)].parts[kAddresses] == sn.parts[kAddresses];
        if (sn.found < 0 && (Tracing() || Recording())) cur.why[uint32_t(&sn - cur.snaps.data())] = Why(prev, sn);
        if (Tracing()) {
            g_t.draws++;
            Trace(cur, sn, prev, sn.found >= 0 ? &prev.snaps[size_t(sn.found)] : nullptr, sn.by_exact, sn.program);
        }
    }
    for (const auto& [program, list] : cur.programs) {
        std::vector<uint32_t>& turns = cur.found_turns[program];
        for (uint32_t k = 0; k < list.size(); k++)
            if (cur.snaps[list[k]].found >= 0) turns.push_back(k);
    }
    for (uint32_t i = 0; i < cur.snaps.size(); i++) {
        const Snap& sn = cur.snaps[i];
        if (sn.leader < 0 && sn.found >= 0 && sn.reads) cur.found_programmed.push_back(i);
    }
    if (Moving()) Settle(prev, cur);
}

void Presented(uint32_t data) {
    if (std::find(g_presented.begin(), g_presented.end(), data) != g_presented.end()) return;
    if (g_presented.size() >= 8) g_presented.erase(g_presented.begin());  // a new swap chain's
    g_presented.push_back(data);
}

void EndFrame() {
    Match();
    if (Tracing()) {
        Frame& prev = g_frames[g_cur ^ 1];
        g_t.gone += uint64_t(std::count(prev.taken.begin(), prev.taken.end(), false));
        if (++g_t.frames >= kLogEvery) {
            Log();
            g_t = {};
        }
    }
    // a save waits for the pictures after the suspect one: the frame's own and those drawn again
    if (g_save_in) {
        uint32_t pictures = g_passes + 1;
        g_save_in = g_save_in > pictures ? g_save_in - pictures : 0;
        if (!g_save_in) gpu::g_record_dump = true;
    }
    g_passes = 0;
    g_frame++;
    g_cur ^= 1;
    g_frames[g_cur].Clear();
    g_frames[g_cur ^ 1].taken.assign(g_frames[g_cur ^ 1].snaps.size(), false);
}

// XBE_INTERP_WATCH=<programs, hex, comma separated>: each of their draws in the notes, how it came out and why;
// XBE_INTERP_WATCH=all watches every draw, to find which program a glitch is by comparing pictures
bool WatchingAll() {
    static const bool all = [] {
        const char* e = std::getenv("XBE_INTERP_WATCH");
        return e && std::strcmp(e, "all") == 0;
    }();
    return all;
}
bool Watched(uint64_t program) {
    if (WatchingAll()) return true;
    static const std::vector<uint64_t> w = [] {
        std::vector<uint64_t> v;
        const char* e = std::getenv("XBE_INTERP_WATCH");
        for (char* end; e && *e; e = *end ? end + 1 : end) {
            uint64_t p = std::strtoull(e, &end, 16);
            if (end == e) break;
            if (p) v.push_back(p);
        }
        return v;
    }();
    return std::find(w.begin(), w.end(), program) != w.end();
}
void Watch(uint32_t n, const Snap& sn, Blended how, const std::string& jumped, const std::string& kept,
           const std::string& moved = "") {
    if (g_pass.watched.size() > (WatchingAll() ? 1000000u : 48000u)) return;
    static const char* const kHow[] = {"not found", "jumped", "borrowed", "blended"};
    char b[320];
    snprintf(b, sizeof(b),
             "\n    draw %u program %016llX: %s, as %d of %u (%s), distance %.4f, runner-up %.4f, %zu palette registers "
             "left, palette from c%u",
             n, (unsigned long long)sn.program, kHow[size_t(how)], sn.found, sn.candidates,
             sn.found < 0         ? "-"
             : sn.by_tag          ? "by tag"
             : sn.second          ? "another detail's"
             : sn.by_exact        ? "exact"
                                  : "other addresses",
             sn.distance, sn.next, sn.stale.count(), sn.reads ? sn.reads->relative : 192u);
    g_pass.watched += b;
    snprintf(b, sizeof(b), "\n      target %08X, textures %08X %08X %08X %08X", sn.target, sn.textures[0],
             sn.textures[1], sn.textures[2], sn.textures[3]);
    g_pass.watched += b;
    // its tag: the object and the piece of its model, and the key; untagged draws were matched by distance alone
    if (sn.tag.key) {
        const Frame& prev = g_frames[g_cur ^ 1];
        bool before = sn.tag.group ? prev.groups.count(sn.tag.key) != 0 : prev.tags.Find(sn.tag.key) != nullptr;
        snprintf(b, sizeof(b), "\n      %s object %08X piece %08X key %016llX%s",
                 sn.tag.group ? "group tag" : "tag", sn.tag.object, sn.tag.piece, (unsigned long long)sn.tag.key,
                 sn.by_tag     ? ""
                 : !before     ? " (not in the frame before)"
                 : sn.tag.group ? " (none of its group's draws in the frame before near enough: new to it)"
                                : " (the frame before's draw of it is another program's, or into another target)");
        g_pass.watched += b;
    } else {
        // where it came from, to tell what the game draws it for: an HLE'd draw's caller, the static push buffer it was
        // read from, the call that had it read (what the game pushed inline came before that call)
        snprintf(b, sizeof(b), "\n      untagged: caller %08X push buffer %08X walked at %08X", sn.tag.caller,
                 sn.tag.source, sn.tag.walked);
        g_pass.watched += b;
    }
    if (!moved.empty()) g_pass.watched += "\n      moved most:" + moved;
    if (!jumped.empty()) g_pass.watched += "\n      jumped:" + jumped;
    if (!kept.empty()) g_pass.watched += "\n      left, jumped, kept:" + kept;
    const std::string& why = g_frames[g_cur].WhyNot(n);
    if (how == Blended::kNotFound && !why.empty()) g_pass.watched += "\n      " + why;
    if (sn.found >= 0) return;
    // what it could have been found as: its program's draws in the frame before no draw took, nearest first, by
    // Distance per value as the second chance weighs them (Match)
    const Frame& cur = g_frames[g_cur];
    const Frame& prev = g_frames[g_cur ^ 1];
    auto it = prev.programs.find(sn.program);
    if (it == prev.programs.end()) return;
    size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
    std::vector<std::pair<float, uint32_t>> near;
    for (uint32_t p : it->second)
        if (!prev.taken[p]) near.emplace_back(Distance(prev, prev.snaps[p], cur, sn, INFINITY) / float(values), p);
    std::sort(near.begin(), near.end());
    snprintf(b, sizeof(b), "\n      count %u; untaken of its program, of %zu:", uint32_t(sn.parts[kShape]),
             it->second.size());
    g_pass.watched += b;
    for (size_t i = 0; i < std::min<size_t>(near.size(), 4); i++) {
        const Snap& o = prev.snaps[near[i].second];
        snprintf(b, sizeof(b), " %u at %.4f (count %u%s)", near[i].second, near[i].first, uint32_t(o.parts[kShape]),
                 o.parts[kAddresses] == sn.parts[kAddresses] ? ", its addresses" : "");
        g_pass.watched += b;
    }
}

// XBE_INTERP_HIDE: draws left out of the pictures drawn again, to see which are which: 1 not found, 2 jumped,
// 4 borrowed (Borrow), 8 blended, 16 suspects (Suspicion, those of XBE_INTERP_SUSPECT_MASK); summed
constexpr uint32_t kHideSuspects = 16;
uint32_t HideMask() {
    static const uint32_t mask = [] {
        const char* e = std::getenv("XBE_INTERP_HIDE");
        return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
    }();
    return mask;
}

// Draws likely found wrong, or not found though they were there, told during play (XBE_INTERP_SUSPECT=1, or
// recording, or hiding them). Why, as bits:
//   1 a toss-up: its runner-up nearly as near (Snap::next). Look-alikes (fur shells, clumps) often are, found right
//   2 a spike: far from what it was found as, though that draw was near its own a frame before: an object on its way
//     moves alike frame to frame, another object's draw doesn't. Many at once are the camera starting to turn; the
//     only draw it could be found as is no spike, but something starting to move
//   4 not found, near a draw of its program in the frame before that no draw found: there, but its keys changed;
//     drawn as the frame has it, it pops
//   8 jumped while nearly all the picture's draws blended: a cut jumps them all
// A program suspect of a kind most frames is the game's way, no one-picture glitch, and left out (Habitual): a view
// the game draws every other frame, a value look-alikes trade each frame
// XBE_INTERP_SUSPECT_MASK picks the bits that make a picture suspect (2 | 4 | 8 by default). A suspect picture is
// logged (one in kLogGap frames at most) and its note lists the suspects; XBE_INTERP_AUTO=<n> saves the pictures
// kept (as F4 does, with XBE_INTERP_RECORD) up to n times a run, once the suspect picture is mid-way through them
enum SuspectKind : uint32_t { kTossUp = 1, kSpike = 2, kBesideGone = 4, kLoneJump = 8 };
constexpr const char* kSuspicionNames[] = {"toss-ups", "spikes", "not found beside a gone draw", "lone jumps"};
constexpr float kSeen = 0.002f;       // per value: draws nearer than this look the same
constexpr float kTossUpShare = 0.5f;  // the nearest at least this of the runner-up
constexpr float kSpikeTimes = 4.0f;   // farther than its draw a frame before, at least kSeen
constexpr float kGoneNear = 0.03f;    // another object's draw is past 0.03
constexpr uint32_t kCrowd = 20;       // spikes or jumps in more than one in kCrowd found draws: the camera's
constexpr uint32_t kLines = 3;        // draws listed per kind
constexpr uint32_t kLogGap = 30;      // frames
constexpr uint32_t kSaveGap = 600;    // frames
constexpr float kHabitFade = 0.9f;    // a frame
constexpr float kHabit = 2.0f;        // suspect frames, faded: about one in five frames of late

bool SuspectOn() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_INTERP_SUSPECT");
        return e && *e && *e != '0';
    }();
    return on;
}
bool Suspecting() { return SuspectOn() || Recording() || (HideMask() & kHideSuspects); }
uint32_t SuspectMask() {
    static const uint32_t mask = [] {
        const char* e = std::getenv("XBE_INTERP_SUSPECT_MASK");
        return e ? uint32_t(std::strtoul(e, nullptr, 0)) : uint32_t(kSpike | kBesideGone | kLoneJump);
    }();
    return mask;
}
uint32_t AutoSaves() {
    static const uint32_t n = [] {
        const char* e = std::getenv("XBE_INTERP_AUTO");
        return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    return n;
}

// a picture's suspects, as it is drawn again
struct Suspects {
    bool on = false;
    bool first = false;  // the frame's first picture drawn again: it logs and saves, the frame's others only note
    bool last = false;   // the draw Blend did last is a suspect, of the mask's kinds (Hidden)
    uint32_t blended = 0, jumped = 0, habitual = 0;
    std::array<uint32_t, 4> by{};
    std::array<std::string, 4> lines;
};
Suspects g_sus;

// whether a program has been suspect of a kind in many frames of late, those before this one; counts this frame's
std::unordered_map<uint64_t, std::pair<uint32_t, float>> g_habits;  // the last frame suspect, the frames faded
bool Habitual(uint64_t program, uint32_t kind) {
    auto& [last, level] = g_habits[Mix(program, kind)];
    float before = last == g_frame ? level - 1.0f : level * std::pow(kHabitFade, float(g_frame - last));
    if (last != g_frame) last = g_frame, level = before + 1.0f;
    return before > kHabit;
}

// a draw not found: per value, the nearest draw of its program in the frame before that no draw found
float Gone(const Frame& prev, const Frame& cur, const Snap& sn) {
    auto it = prev.programs.find(sn.program);
    if (it == prev.programs.end()) return INFINITY;
    float best = INFINITY;
    uint32_t tries = 0;
    for (uint32_t p : it->second) {
        if (prev.taken[p]) continue;
        if (tries++ >= kSecondTries) break;
        best = std::min(best, Distance(prev, prev.snaps[p], cur, sn, best));
    }
    size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
    return values ? best / float(values) : INFINITY;
}

uint32_t Suspicion(const Frame& cur, const Frame& prev, Snap& sn, Blended how) {
    uint32_t why = 0;
    if (sn.found >= 0) {
        if (sn.distance > kSeen && sn.distance >= kTossUpShare * sn.next) why |= kTossUp;
        const Snap& was = prev.snaps[size_t(sn.found)];
        // with another draw to take it for: found among one, it is that one (a cup lifted after standing still)
        if (was.found >= 0 && std::isfinite(sn.next) && sn.distance > kSpikeTimes * std::max(was.distance, kSeen))
            why |= kSpike;
    } else if (how == Blended::kNotFound) {
        if (sn.gone < 0) sn.gone = Gone(prev, cur, sn);
        if (sn.gone > kSeen && sn.gone <= kGoneNear) why |= kBesideGone;
    }
    return why;
}

std::string SuspectLine(uint32_t n, const Frame& prev, const Snap& sn) {
    char b[200];
    float was = sn.found >= 0 ? prev.snaps[size_t(sn.found)].distance : 0.0f;
    if (sn.found >= 0)
        snprintf(b, sizeof(b), "\n    draw %u program %016llX: distance %.4f, runner-up %.4f, as %d, which was %.4f from its own",
                 n, (unsigned long long)sn.program, sn.distance, sn.next, sn.found, was);
    else
        snprintf(b, sizeof(b), "\n    draw %u program %016llX: not found, a gone draw of its program at %.4f", n,
                 (unsigned long long)sn.program, sn.gone);
    return b;
}

// the picture's suspects, summed up: what the note gets. Logs and saves on the frame's first picture
std::string Score() {
    if (!g_sus.on) return {};
    uint32_t found = g_sus.blended + g_sus.jumped;
    if (g_sus.by[1] * kCrowd > found) g_sus.by[1] = 0, g_sus.lines[1].clear();
    if (g_sus.jumped * kCrowd > found) g_sus.by[3] = 0, g_sus.lines[3].clear();
    uint32_t mask = SuspectMask(), counted = 0;
    std::string o;
    for (size_t i = 0; i < 4; i++) {
        if (!g_sus.by[i]) continue;
        if (mask >> i & 1) counted += g_sus.by[i];
        o += (o.empty() ? "" : ", ") + std::to_string(g_sus.by[i]) + " " + kSuspicionNames[i];
    }
    if (g_sus.habitual) o += (o.empty() ? "" : ", ") + std::to_string(g_sus.habitual) + " habitual, left out";
    if (o.empty()) return {};
    std::string lines;
    for (size_t i = 0; i < 4; i++)
        if (g_sus.by[i] && (mask >> i & 1)) lines += g_sus.lines[i];
    if (counted && g_sus.first) {
        if (g_frame - g_logged >= kLogGap || !g_logged) {
            g_logged = g_frame;
            LOGR("interp: frame %u suspect: %s%s", g_frame, o.c_str(), lines.substr(0, lines.find('\n', 1)).c_str());
        }
        if (g_saves < AutoSaves() && !g_save_in && g_frame >= g_save_after) {
            if (!Recording()) {
                LOGR("interp: XBE_INTERP_AUTO saves the pictures XBE_INTERP_RECORD keeps: set it too");
                g_saves = AutoSaves();
            } else {
                g_saves++;
                g_save_in = std::max(Recording() / 2, 1u);
                g_save_after = g_frame + kSaveGap;
                LOGR("interp: frame %u suspect, the pictures kept saved %u pictures on (%u of %u)", g_frame, g_save_in,
                     g_saves, AutoSaves());
            }
        }
    }
    return "\n  suspects: " + o + (counted ? "" : " (none of XBE_INTERP_SUSPECT_MASK)") + lines;
}

// the turns of a sorted list out from turn from, nearest first and the later of two as near: from + 1, from - 1,
// from + 2, ... as they are in the list; until visit says done
template <class F>
void Outward(const std::vector<uint32_t>& turns, size_t from, F visit) {
    size_t lo = size_t(std::lower_bound(turns.begin(), turns.end(), from) - turns.begin());
    size_t hi = size_t(std::upper_bound(turns.begin(), turns.end(), from) - turns.begin());
    while (lo > 0 || hi < turns.size()) {
        bool up = lo == 0 || (hi < turns.size() && turns[hi] - from <= from - turns[lo - 1]);
        if (visit(up ? turns[hi++] : turns[--lo])) return;
    }
}

// a draw not found (a ground's piece drawn only this frame, beside pieces drawn in both) moves as a found draw of its
// program next to it in the frame does, when every constant that one moved (or transform) is the same in this one:
// it takes their values in the frame before from that draw, and keeps its others. Drawn as the frame has it instead,
// it would part from its neighbours by the camera's move and show the sky through the seam. Into old (xf for the
// fixed function); false for no such draw
bool Borrow(const Frame& cur, const Frame& prev, uint32_t n, std::vector<std::array<float, 4>>& old,
            std::array<std::array<float, 16>, kXf>& xf) {
    const Snap& sn = cur.snaps[n];
    auto it = cur.programs.find(sn.program);
    if (it == cur.programs.end()) return false;
    const std::vector<uint32_t>& list = it->second;
    auto found = cur.found_turns.find(sn.program);
    if (found == cur.found_turns.end()) return false;
    size_t from = size_t(std::lower_bound(list.begin(), list.end(), n) - list.begin());
    uint32_t tries = 0;
    bool borrowed = false;
    // the found draws of its program, out from its turn
    Outward(found->second, from, [&](uint32_t turn) {
        const Snap& o = cur.snaps[list[turn]];
        if (o.reads != sn.reads) return false;
        tries++;
        const Snap& was = prev.snaps[size_t(o.found)];
        bool moved = false, same = true;
        if (sn.reads) {
            // by value, not register: a fur's shells share their matrices and a direction whose w is each shell's own
            // (the direction moves, the w doesn't). One moved register the same whole, to know it is the same object
            const std::array<float, 4>* v = cur.Values(sn);
            const std::array<float, 4>* x = cur.Values(o);
            const std::array<float, 4>* y = prev.Values(was);
            for (size_t j = 0; same && j < sn.reads->regs.size(); j++) {
                if (sn.stale[sn.reads->regs[j]] || std::memcmp(&x[j], &y[j], 16) == 0) continue;
                for (size_t c = 0; same && c < 4; c++)
                    same = std::memcmp(&x[j][c], &y[j][c], 4) == 0 || std::memcmp(&v[j][c], &x[j][c], 4) == 0;
                moved |= std::memcmp(&v[j], &x[j], 16) == 0;
            }
            if (!moved || !same) return tries >= kTries;
            old.assign(v, v + sn.reads->regs.size());
            for (size_t j = 0; j < old.size(); j++)
                for (size_t c = 0; c < 4 && !sn.stale[sn.reads->regs[j]]; c++)
                    if (std::memcmp(&x[j][c], &y[j][c], 4) != 0) old[j][c] = y[j][c];
            return borrowed = true;
        }
        for (size_t i = 0; same && i < kXf; i++) {
            if (o.xf[i] == was.xf[i]) continue;
            moved = true;
            same = sn.xf[i] == o.xf[i];
        }
        if (!moved || !same) return tries >= kTries;
        for (size_t i = 0; i < kXf; i++) xf[i] = o.xf[i] == was.xf[i] ? sn.xf[i] : was.xf[i];
        return borrowed = true;
    });
    return borrowed;
}

// a draw not found with no found draw of its program near it (a ground whose pieces all changed their index lists
// that frame, beside a wall that didn't, cracked apart at their seam): as Borrow, from a found draw of another program
// near it in the frame, register by register number: a scene's programs keep the camera in the same registers. A draw
// that moved a register this one reads to another value (its own object moving) is passed over
bool BorrowAcross(const Frame& cur, const Frame& prev, uint32_t n, std::vector<std::array<float, 4>>& old) {
    const Snap& sn = cur.snaps[n];
    if (!sn.reads) return false;
    const std::array<float, 4>* v = cur.Values(sn);
    static std::array<int16_t, 192> at;  // the other draw's value for each register, or -1
    uint32_t tries = 0;
    bool borrowed = false;
    // the frame's found draws with a program, out from its turn
    Outward(cur.found_programmed, n, [&](uint32_t i) {
        const Snap& o = cur.snaps[i];
        if (o.reads == sn.reads) return false;
        tries++;
        const Snap& was = prev.snaps[size_t(o.found)];
        const std::array<float, 4>* x = cur.Values(o);
        const std::array<float, 4>* y = prev.Values(was);
        at.fill(-1);
        for (size_t j = 0; j < o.reads->regs.size(); j++) at[o.reads->regs[j]] = int16_t(j);
        bool moved = false, same = true;
        for (size_t j = 0; same && j < sn.reads->regs.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            int k = at[r];
            if (k < 0 || sn.stale[r] || o.stale[r] || std::memcmp(&x[k], &y[k], 16) == 0) continue;
            for (size_t c = 0; same && c < 4; c++)
                same = std::memcmp(&x[k][c], &y[k][c], 4) == 0 || std::memcmp(&v[j][c], &x[k][c], 4) == 0;
            moved |= std::memcmp(&v[j], &x[k], 16) == 0;
        }
        if (!moved || !same) return tries >= kTries;
        old.assign(v, v + sn.reads->regs.size());
        for (size_t j = 0; j < old.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            int k = at[r];
            if (k < 0 || sn.stale[r] || o.stale[r]) continue;
            for (size_t c = 0; c < 4; c++)
                if (std::memcmp(&x[k][c], &y[k][c], 4) != 0) old[j][c] = y[k][c];
        }
        return borrowed = true;
    });
    return borrowed;
}

Blended BlendLeader(uint32_t n, float t, State& s, Undo& undo) {
    const Frame& cur = g_frames[g_cur];
    const Frame& prev = g_frames[g_cur ^ 1];
    if (g_pass.on) g_pass.draws++;
    if (n < cur.snaps.size() && cur.snaps[n].leader >= 0) n = uint32_t(cur.snaps[n].leader);
    if (g_pass.on && n < cur.snaps.size() && cur.snaps[n].tag.key) {
        g_pass.tagged++;
        if (cur.snaps[n].by_tag) g_pass.by_tag++;
    }
    // its constants, or the fixed function's transforms, in the frame before: the draw's it was found as, or ones
    // borrowed (Borrow)
    static std::vector<std::array<float, 4>> borrowed;
    std::array<std::array<float, 16>, kXf> borrowed_xf;
    const std::array<float, 4>* old = nullptr;
    const std::array<std::array<float, 16>, kXf>* old_xf = nullptr;
    if (n < cur.snaps.size() && cur.snaps[n].found >= 0) {
        const Snap& was = prev.snaps[size_t(cur.snaps[n].found)];
        old = prev.Values(was);
        old_xf = &was.xf;
    } else if (n < cur.snaps.size() && Borrow(cur, prev, n, borrowed, borrowed_xf)) {
        old = borrowed.data();
        old_xf = &borrowed_xf;
        if (g_pass.on) g_pass.borrowed++;
    } else if (n < cur.snaps.size() && BorrowAcross(cur, prev, n, borrowed)) {
        old = borrowed.data();
        if (g_pass.on) g_pass.across++;
    } else {
        if (g_pass.on) {
            g_pass.unfound++;
            auto& u = g_pass.unfound_programs[ProgramOf(s)];
            if (!u.first++ && n < cur.snaps.size()) u.second = cur.WhyNot(n);
            if (n < cur.snaps.size() && Watched(ProgramOf(s))) Watch(n, cur.snaps[n], Blended::kNotFound, "", "");
        }
        return Blended::kNotFound;
    }
    const Snap& sn = cur.snaps[n];
    uint64_t program = ProgramOf(s);
    bool watch = g_pass.on && Watched(program);
    if (g_pass.on && sn.second) g_pass.second++;
    if (g_pass.on && sn.found >= 0) {
        char b[128];
        snprintf(b, sizeof(b), "\n    draw %u program %016llX: distance %.4f, nearest of %u, %s", n,
                 (unsigned long long)program, sn.distance, sn.candidates,
                 sn.by_tag ? "by tag" : sn.second ? "another detail's" : sn.by_exact ? "exact" : "without addresses");
        auto& f = g_pass.farthest[program];
        if (f.second.empty() || sn.distance > f.first) f = {sn.distance, b};
    }
    // the translations' reach: a register's w (a row of a matrix the program takes with dp4), a transform's fourth row.
    // Of all it reads, those another draw left too: the bones an eye shares with its face are counted as left
    float reach = 0;
    if (sn.reads) {
        for (size_t j = 0; j < sn.reads->regs.size(); j++)
            reach = Reach(Reach(reach, old[j][3]), s.vsc.v[sn.reads->regs[j]][3]);
    }
    // what its program reads, or the fixed function's transforms, that jumped: a register moves whole, as a matrix
    // does. Kept as the frame has it rather than jumped: a register holding packed data (a flag, not a number), and a
    // palette register another draw left (Stale) that jumped. One left that didn't is taken part way all the same: a
    // character's later upload (its eyes, its head) leaves the bones it shares with the body before it looking left
    std::string regs, kept;
    bool jumped = false;
    static std::vector<uint8_t> keep;  // 1 kept as the frame has it, kFlip taken from the row negated (Flipped)
    if (sn.reads) {
        keep.assign(sn.reads->regs.size(), 0);
        // the rows turned over: two at least, as a matrix's are, or a row crossing zero is taken for one
        uint32_t flips = 0;
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            if (sn.stale[r] || !Flipped(old[j].data(), s.vsc.v[r].data())) continue;
            keep[j] = kFlip;
            flips++;
        }
        if (flips < 2) std::fill(keep.begin(), keep.end(), 0);
        else if (watch) kept += " " + std::to_string(flips) + " rows (negated)";
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            if (keep[j] == kFlip) continue;
            uint8_t r = sn.reads->regs[j];
            const float* a = old[j].data();
            const float* c = s.vsc.v[r].data();
            if (std::memcmp(a, c, 16) == 0) continue;
            bool packed = false;
            for (size_t i = 0; i < 4; i++) packed |= std::memcmp(&a[i], &c[i], 4) != 0 && (Packed(a[i]) || Packed(c[i]));
            if (packed) {
                keep[j] = 1;
                continue;
            }
            if (Wrapped(a, c)) {
                keep[j] = 1;
                if (watch) kept += " c" + std::to_string(r) + " (wrapped)";
                continue;
            }
            if (Cleared(a, c)) {
                keep[j] = 1;
                if (watch) kept += " c" + std::to_string(r) + " (cleared)";
                continue;
            }
            if (!RowJumped(a, c, reach)) continue;
            if (sn.stale[r]) {
                // with the rows either side, however its matrix lines up: a bone the draw does use, uploaded before
                // the draw's own (a head's, for the eyes), must not part, a row as the frame has it and two part way
                for (size_t k = j >= 2 ? j - 2 : 0; k < std::min(j + 3, keep.size()); k++) {
                    uint8_t q = sn.reads->regs[k];
                    if (q >= sn.reads->relative && q + 2 >= r && q <= r + 2) keep[k] = 1;
                }
                if (g_pass.on) {
                    g_pass.kept++;
                    if (watch) kept += " c" + std::to_string(r);
                }
                continue;
            }
            jumped = true;
            if (g_pass.on && (regs.size() < 200 || (watch && regs.size() < 600))) {
                char b[128];
                snprintf(b, sizeof(b), " c%u (%g %g %g %g -> %g %g %g %g)", r, a[0], a[1], a[2], a[3], c[0], c[1], c[2],
                         c[3]);
                regs += b;
            }
        }
    } else {
        for (size_t i = 0; i < kXf; i++) {
            const auto& a = (*old_xf)[i];
            const auto& b = s.transforms[size_t(kTransforms[i])];
            float tmp[16];
            if (Lerp(a.data(), b.data(), 16, t, tmp, Reach16(a, b).data())) continue;
            jumped = true;
            if (g_pass.on) regs += " xf" + std::to_string(kTransforms[i]);
        }
    }
    if (jumped && g_pass.on) {
        g_pass.jumped++;
        if (g_pass.jump_lines[program]++ < 2) {
            char b[64];
            snprintf(b, sizeof(b), "\n    draw %u program %016llX:", n, (unsigned long long)program);
            g_pass.jumps += b + regs;
        }
    }
    // a draw with anything that jumped is drawn as the frame has it: the rows of one matrix must not part, half
    // taken part way and half not. XBE_INTERP_WHOLE=0 takes each register on its own
    static const bool whole = [] {
        const char* e = std::getenv("XBE_INTERP_WHOLE");
        return !e || *e != '0';
    }();
    Blended how = jumped && whole ? Blended::kJumped : sn.found >= 0 ? Blended::kYes : Blended::kBorrowed;
    // the registers that moved most, for telling an object found as its look-alike from one on its way
    std::string moved;
    if (watch && sn.reads) {
        std::array<std::pair<float, size_t>, 3> top{};
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            const float* a = old[j].data();
            const float* c = s.vsc.v[sn.reads->regs[j]].data();
            float d = 0;
            for (size_t i = 0; i < 4; i++) d += Rel(a[i], c[i]);
            if (d > top[2].first) {
                top[2] = {d, j};
                std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            }
        }
        for (const auto& [d, j] : top) {
            if (d <= 0) break;
            const float* a = old[j].data();
            const float* c = s.vsc.v[sn.reads->regs[j]].data();
            char b[160];
            snprintf(b, sizeof(b), " c%u (%g %g %g %g -> %g %g %g %g)", sn.reads->regs[j], a[0], a[1], a[2], a[3],
                     c[0], c[1], c[2], c[3]);
            moved += b;
        }
    }
    if (watch) Watch(n, sn, how, regs, kept, moved);
    if (how == Blended::kJumped) return how;
    if (sn.reads) {
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            if (keep[j] == 1 || std::memcmp(&old[j], &s.vsc.v[r], 16) == 0) continue;
            undo.vsc.emplace_back(uint32_t(r), s.vsc.v[r]);
            float sign = keep[j] == kFlip ? -1.0f : 1.0f;
            for (size_t i = 0; i < 4; i++) s.vsc.v[r][i] = sign * old[j][i] + (s.vsc.v[r][i] - sign * old[j][i]) * t;
        }
        return how;
    }
    for (size_t i = 0; i < kXf; i++) {
        auto& m = s.transforms[size_t(kTransforms[i])];
        if (std::memcmp(&(*old_xf)[i], &m, sizeof(m)) == 0) continue;
        undo.xf.emplace_back(uint32_t(kTransforms[i]), m);
        Lerp((*old_xf)[i].data(), m.data(), 16, t, m.data(), Reach16((*old_xf)[i], m).data());
    }
    return how;
}

// a draw following its leader (Follows) reads the leader's constants and comes out as it did: the leader's blend
// kept, set again, and counted again in the picture's note. Worked out again for each, a battle's thousands of
// followers took the picture 8 ms (its draws not found looked for a neighbour to borrow from, each time)
struct Kept {
    uint32_t pass = ~0u, leader = ~0u;
    Blended how = Blended::kNotFound;
    std::vector<std::pair<uint32_t, std::array<float, 4>>> vsc;
    std::vector<std::pair<uint32_t, std::array<float, 16>>> xf;
    uint32_t unfound = 0, jumped = 0, borrowed = 0, across = 0, kept = 0, second = 0;  // what it added to Pass
    uint32_t tagged = 0, by_tag = 0;
    bool suspect = false, judged = false;  // Suspects::last, once Blend judged the leader
};
Kept g_kept;
// XBE_INTERP_OFFSCREEN=0: a draw into a target that is not presented (a shadow from the light, a character's lighting
// map) drawn as the frame has it, its texture read by draws taken part way: to tell a glitch in what reads such a
// texture from one in what is drawn into it
bool OffscreenKept() {
    static const bool kept = [] {
        const char* e = std::getenv("XBE_INTERP_OFFSCREEN");
        return e && *e == '0';
    }();
    return kept;
}

Blended BlendDraw(uint32_t n, float t, State& s, Undo& undo) {
    const Frame& cur = g_frames[g_cur];
    if (n >= cur.snaps.size()) return BlendLeader(n, t, s, undo);
    uint32_t leader = cur.snaps[n].leader >= 0 ? uint32_t(cur.snaps[n].leader) : n;
    if (OffscreenKept() &&
        std::find(g_presented.begin(), g_presented.end(), cur.snaps[leader].target) == g_presented.end()) {
        if (g_pass.on) g_pass.draws++, g_pass.offscreen++;
        return Blended::kNotFound;
    }
    Kept& k = g_kept;
    if (leader != n && k.pass == g_pass_id && k.leader == leader) {
        for (const auto& [r, v] : k.vsc) {
            undo.vsc.emplace_back(r, s.vsc.v[r]);
            s.vsc.v[r] = v;
        }
        for (const auto& [i, m] : k.xf) {
            undo.xf.emplace_back(i, s.transforms[i]);
            s.transforms[i] = m;
        }
        if (g_pass.on) {
            // as the leader counted; its watch lines and farthest distance said already
            g_pass.draws++;
            g_pass.unfound += k.unfound;
            g_pass.jumped += k.jumped;
            g_pass.borrowed += k.borrowed;
            g_pass.across += k.across;
            g_pass.kept += k.kept;
            g_pass.second += k.second;
            g_pass.tagged += k.tagged;
            g_pass.by_tag += k.by_tag;
            if (k.unfound) g_pass.unfound_programs[ProgramOf(s)].first++;
            if (k.jumped) g_pass.jump_lines[ProgramOf(s)]++;
        }
        return k.how;
    }
    size_t vsc = undo.vsc.size(), xf = undo.xf.size();
    uint32_t unfound = g_pass.unfound, jumped = g_pass.jumped, borrowed = g_pass.borrowed, across = g_pass.across,
             kept = g_pass.kept, second = g_pass.second, tagged = g_pass.tagged, by_tag = g_pass.by_tag;
    Blended how = BlendLeader(n, t, s, undo);
    k.pass = g_pass_id;
    k.leader = leader;
    k.how = how;
    k.judged = false;
    k.unfound = g_pass.unfound - unfound;
    k.jumped = g_pass.jumped - jumped;
    k.borrowed = g_pass.borrowed - borrowed;
    k.across = g_pass.across - across;
    k.kept = g_pass.kept - kept;
    k.second = g_pass.second - second;
    k.tagged = g_pass.tagged - tagged;
    k.by_tag = g_pass.by_tag - by_tag;
    k.vsc.clear();
    k.xf.clear();
    for (size_t i = vsc; i < undo.vsc.size(); i++) k.vsc.emplace_back(undo.vsc[i].first, s.vsc.v[undo.vsc[i].first]);
    for (size_t i = xf; i < undo.xf.size(); i++) k.xf.emplace_back(undo.xf[i].first, s.transforms[undo.xf[i].first]);
    return how;
}

Blended Blend(uint32_t n, float t, State& s, Undo& undo) {
    Blended how = BlendDraw(n, t, s, undo);
    g_sus.last = false;
    Frame& cur = g_frames[g_cur];
    if (!g_sus.on || n >= cur.snaps.size()) return how;
    // a draw in the state of the one before it is judged, and counted, as that one: suspect as it was
    bool follows = cur.snaps[n].leader >= 0;
    uint32_t at = follows ? uint32_t(cur.snaps[n].leader) : n;
    if (follows && g_kept.judged && g_kept.pass == g_pass_id && g_kept.leader == at) {
        g_sus.last = g_kept.suspect;
        return how;
    }
    const Frame& prev = g_frames[g_cur ^ 1];
    Snap& sn = cur.snaps[at];
    uint32_t why = Suspicion(cur, prev, sn, how);
    if (how == Blended::kJumped) why |= kLoneJump;  // whether lone, Score tells
    uint32_t habits = 0;
    for (uint32_t i = 0; i < 4; i++)
        if ((why >> i & 1) && Habitual(sn.program, i)) habits |= 1u << i;
    why &= ~habits;
    g_sus.last = (why & SuspectMask() & ~uint32_t(kLoneJump)) != 0;
    if (g_kept.pass == g_pass_id && g_kept.leader == at) g_kept.suspect = g_sus.last, g_kept.judged = true;
    if (follows) return how;
    if (habits) g_sus.habitual++;
    if (how == Blended::kJumped) g_sus.jumped++;
    else if (how != Blended::kNotFound) g_sus.blended++;
    for (size_t i = 0; i < 4; i++)
        if ((why >> i & 1) && g_sus.by[i]++ < kLines)
            g_sus.lines[i] += SuspectLine(at, prev, sn) + (i == 3 ? ", jumped" : "");
    return how;
}

// the guest memory MoveVertices writes moved vertices to, and the vertex buffer headers that point at them; the
// backend copies a draw's vertices as it takes the draw, so a ring is used again once round
uint32_t g_move_data = 0, g_move_data_size = 0, g_move_data_pos = 0;
uint32_t g_move_headers = 0, g_move_headers_size = 0, g_move_headers_pos = 0;
constexpr uint32_t kHeaderBytes = 12;  // a vertex buffer's: common, data, lock

void MoveRing(uint32_t data, uint32_t data_size, uint32_t headers, uint32_t headers_size) {
    g_move_data = data, g_move_data_size = data_size;
    g_move_headers = headers, g_move_headers_size = headers_size;
}

bool MoveVertices(uint32_t n, float t, Blended how, State& s, uint32_t& start, Undo& undo) {
    const Frame& cur = g_frames[g_cur];
    const Frame& prev = g_frames[g_cur ^ 1];
    if (!g_move_data || n >= cur.snaps.size() || how == Blended::kNotFound || how == Blended::kJumped) return false;
    const Snap& sn = cur.snaps[n];
    if (!sn.verts) return false;
    // judged before the frame's pictures (Settle); a draw held, or with nothing to move, is drawn as the frame has it
    const Snap* was = MovedFrom(prev, cur, n);
    bool watch = g_pass.on && Watched(ProgramOf(s));
    if (!was || sn.still) {
        if (watch && was && sn.still && *sn.still) {
            char line[160];
            snprintf(line, sizeof(line), "\n    draw %u program %016llX: %u vertices kept as the frame has them: %s", n,
                     (unsigned long long)ProgramOf(s), sn.verts, sn.still);
            g_pass.watched += line;
        }
        return false;
    }
    uint32_t comps = sn.type >> 4;
    const uint8_t* a = prev.raw.data() + was->raw_at + sn.offset;
    const uint8_t* b = cur.raw.data() + sn.raw_at + sn.offset;
    std::array<float, 4> step{}, lo{}, hi{};
    // this frame's vertices with their positions part way, and a header of the stream's pointing at them
    uint32_t bytes = sn.verts * sn.stride, room = (bytes + 63) & ~63u;
    if (room > g_move_data_size) return false;
    if (g_move_data_pos + room > g_move_data_size) g_move_data_pos = 0;
    if (g_move_headers_pos + 16 > g_move_headers_size) g_move_headers_pos = 0;
    uint32_t data = g_move_data + g_move_data_pos, header = g_move_headers + g_move_headers_pos;
    g_move_data_pos += room;
    g_move_headers_pos += 16;
    uint8_t* out = static_cast<uint8_t*>(kernel::HostPtr(data));
    std::memcpy(out, cur.raw.data() + sn.raw_at, bytes);
    for (uint32_t v = 0; v < sn.verts; v++)
        for (uint32_t c = 0; c < comps; c++) {
            float x, y;
            std::memcpy(&x, a + v * sn.stride + c * 4, 4);
            std::memcpy(&y, b + v * sn.stride + c * 4, 4);
            float z = x + (y - x) * t;
            std::memcpy(out + v * sn.stride + sn.offset + c * 4, &z, 4);
            if (!v) step[c] = y - x, lo[c] = hi[c] = y;
            lo[c] = std::min(lo[c], y), hi[c] = std::max(hi[c], y);
        }
    State::Stream& st = s.streams[sn.stream];
    std::memcpy(kernel::HostPtr(header), kernel::HostPtr(st.vb), kHeaderBytes);
    kernel::W32(header + kResData, mem::PhysicalFromVirtual(data));
    undo.stream = sn.stream;
    undo.vb = st.vb;
    undo.base_vertex = s.base_vertex;
    st.vb = header;
    // vertex first is the copy's first: an indexed draw's indices go from the base vertex, the others from start
    if (sn.indexed) s.base_vertex -= sn.first;
    else start -= sn.first;
    if (g_pass.on) {
        g_pass.moved++;
        if (watch) {
            char line[256];
            snprintf(line, sizeof(line),
                     "\n    draw %u program %016llX: %u vertices moved part way, from the frame before's draw %u, by "
                     "(%g %g), at (%g %g)-(%g %g) in the frame",
                     n, (unsigned long long)ProgramOf(s), sn.verts, uint32_t(was - prev.snaps.data()), step[0],
                     step[1], lo[0], lo[1], hi[0], hi[1]);
            g_pass.watched += line;
        }
    }
    return true;
}

bool Hidden(Blended how) {
    uint32_t mask = HideMask();
    return (mask >> uint32_t(how) & 1) || ((mask & kHideSuspects) && g_sus.last);
}

void CheckVertices(uint32_t n, const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    const Frame& cur = g_frames[g_cur];
    if (!g_pass.on || n >= cur.snaps.size() || cur.snaps[n].leader >= 0) return;
    if (MakeKeys(s, prim, start, count, indices).data == cur.snaps[n].data) return;
    g_pass.rewritten++;
    g_pass.rewritten_programs[ProgramOf(s)]++;
}

uint32_t Recording() {
    static const uint32_t n = [] {
        const char* e = std::getenv("XBE_INTERP_RECORD");
        long v = e ? std::strtol(e, nullptr, 10) : 0;
        return uint32_t(std::clamp(v, 0l, 1000l));
    }();
    return n;
}

void BeginPass(float t) {
    g_pass = {};
    g_pass.on = Recording() != 0;
    g_pass.t = t;
    g_sus = {};
    g_sus.on = Suspecting();
    g_sus.first = g_scored != g_frame;
    g_scored = g_frame;
    g_passes++;
    g_pass_id++;
}

void EndPass(uint32_t slot) {
    std::string suspects = Score();
    if (!g_pass.on || slot >= std::size(g_notes)) return;
    char b[512];
    snprintf(b, sizeof(b),
             "drawn again at t %.2f: %u draws (%u tagged, %u found by tag, %u with their vertices moved, %u off screen "
             "kept), %u not found, "
             "%u found as another detail's, %u moved as a neighbour found, %u as another program's, %u with registers "
             "that jumped, %u palette registers another draw left that jumped; at other addresses found within %.4f "
             "(by the frame)",
             g_pass.t, g_pass.draws, g_pass.tagged, g_pass.by_tag, g_pass.moved, g_pass.offscreen, g_pass.unfound,
             g_pass.second,
             g_pass.borrowed, g_pass.across, g_pass.jumped, g_pass.kept, g_loose);
    std::string note = b + suspects;
    std::vector<std::pair<uint64_t, std::pair<uint32_t, std::string>>> top(g_pass.unfound_programs.begin(),
                                                                           g_pass.unfound_programs.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); i++) {
        snprintf(b, sizeof(b), "\n    not found: program %016llX x%u, ", (unsigned long long)top[i].first,
                 top[i].second.first);
        note += b + top[i].second.second;
    }
    if (g_pass.rewritten) {
        snprintf(b, sizeof(b), "\n  vertices changed since drawn: %u draws", g_pass.rewritten);
        note += b;
        for (const auto& [program, k] : g_pass.rewritten_programs) {
            snprintf(b, sizeof(b), "\n    program %016llX x%u", (unsigned long long)program, k);
            note += b;
        }
    }
    note += "\n  farthest found, a draw per program:";
    std::vector<std::pair<float, std::string>> far;
    for (auto& [program, f] : g_pass.farthest) far.push_back(f);
    std::sort(far.begin(), far.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < std::min<size_t>(far.size(), 10); i++) note += far[i].second;
    if (!g_pass.jumps.empty()) note += "\n  jumped:" + g_pass.jumps;
    if (!g_pass.watched.empty()) note += "\n  watched:" + g_pass.watched;
    g_notes[slot] = std::move(note);
    g_pass.on = false;
}

const std::string& Note(uint32_t slot) {
    static const std::string none;
    return slot < std::size(g_notes) ? g_notes[slot] : none;
}

}  // namespace gpu::interp
