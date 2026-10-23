#include "gpu/interp.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
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
};

Keys MakeKeys(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    Keys k;
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    if (prog) EnsureProgramHash(s);
    k.program = prog ? s.vs->hash : s.vs_handle;
    k.parts[kCombiner] = s.ps ? s.ps->handle : 0;
    // targets by shape, not address: the back buffers' memory rotates every Swap (d3d8.cpp)
    k.parts[kTarget] = uint64_t(s.rt.format) << 32 | s.rt.size;
    k.parts[kDepth] = uint64_t(s.zs.format) << 32 | s.zs.size;
    k.parts[kShape] = uint64_t(prim) << 32 | count;
    k.parts[kStart] = start;
    k.parts[kIndices] = indices ? HashBytes(0, indices, size_t(std::min(count, kIndicesHashed)) * 2) : 0;
    uint64_t h = Mix(0, k.program);
    for (int i = 0; i < kAddresses; i++) h = Mix(h, k.parts[size_t(i)]);
    k.loose = h;
    uint64_t addresses = 0;

    // the streams the draw reads: a raw draw's inline formats, a declaration's streams, an FVF's stream 0
    uint32_t used = 0;
    for (uint32_t i = 0; i < 16; i++)
        if (s.streams[i].fmt & 0xF0) used |= 1u << i;
    if ((s.vs_handle & 1) && s.vs) {
        for (const auto& in : s.vs->inputs)
            if (in.stream >= 0) used |= 1u << in.stream;
    } else {
        used |= 1;
    }
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
        if ((sampled >> i & 1) && s.textures[i] && Describe(s.textures[i], td))
            addresses = Mix(addresses, uint64_t(td.data) << 2 | i);
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
    uint64_t lod = 0;     // Keys::lod
    uint64_t mesh = 0;    // Keys::mesh
    std::string why;     // not found, when recording or tracing: what keeps it from the nearest draw of its program
};

struct Frame {
    std::unordered_map<uint64_t, std::vector<uint32_t>> loose, programs, lod, mesh;  // the draws of each key
    std::vector<Snap> snaps;
    std::vector<std::array<float, 4>> values;  // the snaps' constants
    std::vector<bool> taken;  // found by a draw of the frame after
    bool matched = false;     // its draws looked for in the frame before (Match)
    const std::array<float, 4>* Values(const Snap& sn) const { return values.data() + sn.at; }
    void Clear() {
        matched = false;
        loose.clear();
        programs.clear();
        lod.clear();
        mesh.clear();
        snaps.clear();
        values.clear();
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
            for (size_t c = 0; c < 4; c++) d += w * Rel(x[j][c], y[j][c]);
            if (d > limit) return d;
        }
        return d;
    }
    for (size_t m = 0; m < kXf; m++)
        for (size_t i = 0; i < 16; i++) d += Rel(a.xf[m][i], b.xf[m][i]);
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
    if (it == prev.programs.end()) return "its program was not drawn";
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
// frame, followed or not, sees the changes
std::bitset<192> Stale(const State& s, const Reads* reads) {
    static std::array<std::array<float, 4>, 192> last{};
    static std::array<uint32_t, 192> changed{};
    static uint32_t draw = 0;
    draw++;
    for (size_t r = 0; r < 192; r++) {
        if (std::memcmp(&last[r], &s.vsc.v[r], 16) == 0) continue;
        last[r] = s.vsc.v[r];
        changed[r] = draw;
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
// too: it moves as that one does (a ground drawn a quad at a time is thousands of these), and is found as it is
bool Follows(const Frame& cur, const State& s, int32_t& leader) {
    if (cur.snaps.empty()) return false;
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

void Draw(const State& s, bool same, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    Frame& cur = g_frames[g_cur];
    int32_t leader = -1;
    const Reads* reads = ReadsOf(s);
    std::bitset<192> stale = Stale(s, reads);
    if (same && !Tracing() && Follows(cur, s, leader)) {
        Snap& f = cur.snaps.emplace_back();
        f.leader = leader;
        return;
    }
    Keys k = MakeKeys(s, prim, start, count, indices);
    uint32_t n = uint32_t(cur.snaps.size());
    Snap& sn = cur.snaps.emplace_back();
    sn.reads = reads;
    sn.stale = stale;
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
}

// the frame's draws found in the frame before, once its draws are all in: among the draws alike but for their
// addresses (a dynamic buffer's draw is still the same draw), the nearest pairs first, of all of a kind at once. Each
// draw taking its nearest in turn left the last of a kind what the others didn't take: a text's letters took their
// drop shadow's, and the shadow the letters'. Same addresses ahead of other as near, then the same turn. A crowd too
// large to weigh each against all goes a draw at a time (Nearest)
constexpr size_t kPairs = 4096;
constexpr float kLooseCost = 3.0f;
// whether the frame before has a draw of the same mesh at its addresses, at any detail, no draw took yet: the draw is
// that one, not another's at other addresses (a clump drawing fewer blades took a clump of that many)
bool OwnElsewhere(const Frame& prev, const Snap& sn) {
    auto it = prev.lod.find(sn.lod);
    if (it == prev.lod.end()) return false;
    for (uint32_t p : it->second)
        if (!prev.taken[p]) return true;
    return false;
}
// per value, the farthest a draw at other addresses (a dynamic buffer's) is taken as: such draws found right are
// under 0.02; a string of text fading out took the one fading in (0.037) and was drawn over it, in a still menu. As
// far as kMotionSlack times how far its program's draws found at their own addresses moved (Motion), or the frame's
// for a program with none: in a fast pan, draws found at their own addresses were 0.03 apart, and a ground whose
// buffers all moved that frame (0.027) was drawn unblended, sky showing through its seams. Not the frame's for all:
// in the menu its draws that moved most (a view drawn every other frame, characters) took it to 0.11, and text took
// another string again
constexpr float kLooseDistance = 0.025f;
constexpr float kMotionSlack = 1.25f;
float g_loose = kLooseDistance;  // the frame's (Match), for the notes
// how far the frame's draws moved, per value: the median of those the frame before has at the same addresses, in
// every part, its draws that moved (a HUD that didn't says nothing of the camera); and each program's, the median of
// all of its such draws, those that stayed too
struct Motion {
    float frame = 0;
    std::unordered_map<uint64_t, float> program;
    float Of(uint64_t p) const {
        auto it = program.find(p);
        return it != program.end() ? it->second : frame;
    }
};
float Median(std::vector<float>& d) {
    if (d.empty()) return 0;
    auto at = d.begin() + ptrdiff_t(d.size() / 2);
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
        auto it = prev.lod.find(sn.lod);
        if (it == prev.lod.end()) continue;
        for (uint32_t p : it->second) {
            if (prev.snaps[p].parts != sn.parts) continue;
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            float v = values ? Distance(prev, prev.snaps[p], cur, sn, INFINITY) / float(values) : 0;
            if (v > 0) moved.push_back(v);
            by[sn.program].push_back(v);
            break;
        }
    }
    m.frame = Median(moved);
    for (auto& [p, d] : by)
        if (!d.empty()) m.program[p] = Median(d);
}
constexpr size_t kSecondTries = 256;
constexpr float kSecondDistance = 0.01f;
constexpr float kSecondMargin = 0.5f;  // the nearest at most this of the next nearest  // per value; a draw found right is under 0.01, another object's past 0.03
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
    // each draw's two nearest, at the same addresses ([0], [1]) and at others ([2], [3]): its runner-up is the
    // nearer of those alike to the one it took (Snap::next)
    static std::vector<std::array<float, 4>> two;
    for (const auto& [key, list] : cur.loose) {
        auto it = prev.loose.find(key);
        if (it == prev.loose.end()) continue;
        const std::vector<uint32_t>& before = it->second;
        if (list.size() * before.size() > kPairs) {
            for (size_t k = 0; k < list.size(); k++) {
                Snap& sn = cur.snaps[list[k]];
                if (!Nearest(prev, cur, prev.loose, key, k, sn)) continue;
                if (prev.snaps[size_t(sn.found)].parts[kAddresses] != sn.parts[kAddresses] &&
                    sn.distance > loose(sn)) {
                    sn.found = -1;
                    continue;
                }
                prev.taken[size_t(sn.found)] = true;
            }
            continue;
        }
        pairs.clear();
        two.assign(list.size(), {INFINITY, INFINITY, INFINITY, INFINITY});
        for (uint32_t c = 0; c < list.size(); c++)
            for (uint32_t p = 0; p < before.size(); p++) {
                const Snap& sn = cur.snaps[list[c]];
                const Snap& o = prev.snaps[before[p]];
                const Pair& pr = pairs.emplace_back(Pair{Distance(prev, o, cur, sn, INFINITY),
                                                         o.parts[kAddresses] == sn.parts[kAddresses], c > p ? c - p : p - c, c, p});
                float* t = two[c].data() + (pr.same ? 0 : 2);
                if (pr.d < t[0]) t[1] = t[0], t[0] = pr.d;
                else if (pr.d < t[1]) t[1] = pr.d;
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
        used.assign(list.size(), false);
        for (const Pair& pr : pairs) {
            uint32_t p = before[pr.p];
            if (used[pr.c] || prev.taken[p]) continue;
            Snap& sn = cur.snaps[list[pr.c]];
            size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
            if (!pr.same && values && pr.d / float(values) > loose(sn)) continue;
            if (!pr.same && OwnElsewhere(prev, sn)) continue;
            used[pr.c] = true;
            prev.taken[p] = true;
            sn.found = int32_t(p);
            sn.distance = values ? pr.d / float(values) : 0;
            const float* t = two[pr.c].data() + (pr.same ? 0 : 2);
            float next = pr.d == t[0] ? t[1] : t[0];
            sn.next = values ? next / float(values) : INFINITY;
            sn.candidates = uint32_t(before.size());
        }
    }
    // a draw still not found that is a draw of the frame before but for its count (the same buffers, start, first
    // indices and kind of primitive) is that draw at another detail: a grass clump drawing fewer blades as the camera
    // moves off. The nearest such that no draw found. Then, as well, one that is but for its count, start and indices:
    // the same buffers drawn with an index list built again, a ground's pieces as the camera turns (cracked from the
    // pieces found beside them). Pieces sharing buffers are told apart by distance; alike, any is drawn the same
    auto by_detail = [&](const std::unordered_map<uint64_t, std::vector<uint32_t>>& by, uint64_t Snap::*key) {
        for (Snap& sn : cur.snaps) {
            if (sn.leader >= 0 || sn.found >= 0) continue;
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
        if (sn.leader >= 0 || sn.found >= 0 || !sn.reads || sn.reads->relative >= 192) continue;
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
        if (sn.found < 0 && (Tracing() || Recording())) sn.why = Why(prev, sn);
        if (Tracing()) {
            g_t.draws++;
            Trace(cur, sn, prev, sn.found >= 0 ? &prev.snaps[size_t(sn.found)] : nullptr, sn.by_exact, sn.program);
        }
    }
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

// XBE_INTERP_WATCH=<program, hex>: each of the program's draws in the notes, how it came out and why
bool Watched(uint64_t program) {
    static const uint64_t w = [] {
        const char* e = std::getenv("XBE_INTERP_WATCH");
        return e ? uint64_t(std::strtoull(e, nullptr, 16)) : 0ull;
    }();
    return w && program == w;
}
void Watch(uint32_t n, const Snap& sn, Blended how, const std::string& jumped, const std::string& kept) {
    if (g_pass.watched.size() > 12000) return;
    static const char* const kHow[] = {"not found", "jumped", "borrowed", "blended"};
    char b[200];
    snprintf(b, sizeof(b), "\n    draw %u: %s, as %d of %u, distance %.4f, %zu palette registers left, palette from c%u",
             n, kHow[size_t(how)], sn.found, sn.candidates, sn.distance, sn.stale.count(),
             sn.reads ? sn.reads->relative : 192u);
    g_pass.watched += b;
    if (!jumped.empty()) g_pass.watched += "\n      jumped:" + jumped;
    if (!kept.empty()) g_pass.watched += "\n      left, jumped, kept:" + kept;
    if (how == Blended::kNotFound && !sn.why.empty()) g_pass.watched += "\n      " + sn.why;
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
    size_t from = size_t(std::lower_bound(list.begin(), list.end(), n) - list.begin());
    uint32_t tries = 0;
    for (size_t step = 1; tries < kTries && step < 2 * list.size(); step++) {
        // from + 1, from - 1, from + 2, ...
        size_t off = (step + 1) / 2;
        if (step % 2 ? from + off >= list.size() : off > from) continue;
        const Snap& o = cur.snaps[list[step % 2 ? from + off : from - off]];
        if (o.found < 0 || o.reads != sn.reads) continue;
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
            if (!moved || !same) continue;
            old.assign(v, v + sn.reads->regs.size());
            for (size_t j = 0; j < old.size(); j++)
                for (size_t c = 0; c < 4 && !sn.stale[sn.reads->regs[j]]; c++)
                    if (std::memcmp(&x[j][c], &y[j][c], 4) != 0) old[j][c] = y[j][c];
            return true;
        }
        for (size_t i = 0; same && i < kXf; i++) {
            if (o.xf[i] == was.xf[i]) continue;
            moved = true;
            same = sn.xf[i] == o.xf[i];
        }
        if (!moved || !same) continue;
        for (size_t i = 0; i < kXf; i++) xf[i] = o.xf[i] == was.xf[i] ? sn.xf[i] : was.xf[i];
        return true;
    }
    return false;
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
    for (size_t step = 1; tries < kTries && step < 2 * cur.snaps.size(); step++) {
        // n + 1, n - 1, n + 2, ...
        size_t off = (step + 1) / 2;
        if (step % 2 ? n + off >= cur.snaps.size() : off > n) continue;
        const Snap& o = cur.snaps[step % 2 ? n + off : n - off];
        if (o.leader >= 0 || o.found < 0 || !o.reads || o.reads == sn.reads) continue;
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
        if (!moved || !same) continue;
        old.assign(v, v + sn.reads->regs.size());
        for (size_t j = 0; j < old.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            int k = at[r];
            if (k < 0 || sn.stale[r] || o.stale[r]) continue;
            for (size_t c = 0; c < 4; c++)
                if (std::memcmp(&x[k][c], &y[k][c], 4) != 0) old[j][c] = y[k][c];
        }
        return true;
    }
    return false;
}

Blended BlendDraw(uint32_t n, float t, State& s, Undo& undo) {
    const Frame& cur = g_frames[g_cur];
    const Frame& prev = g_frames[g_cur ^ 1];
    if (g_pass.on) g_pass.draws++;
    if (n < cur.snaps.size() && cur.snaps[n].leader >= 0) n = uint32_t(cur.snaps[n].leader);
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
            if (!u.first++ && n < cur.snaps.size()) u.second = cur.snaps[n].why;
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
                 (unsigned long long)program, sn.distance, sn.candidates, sn.second ? "another detail's" : sn.by_exact ? "exact" : "without addresses");
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
    static std::vector<uint8_t> keep;
    if (sn.reads) {
        keep.assign(sn.reads->regs.size(), 0);
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
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
    if (watch) Watch(n, sn, how, regs, kept);
    if (how == Blended::kJumped) return how;
    if (sn.reads) {
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            if (keep[j] || std::memcmp(&old[j], &s.vsc.v[r], 16) == 0) continue;
            undo.vsc.emplace_back(uint32_t(r), s.vsc.v[r]);
            for (size_t i = 0; i < 4; i++) s.vsc.v[r][i] = old[j][i] + (s.vsc.v[r][i] - old[j][i]) * t;
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

Blended Blend(uint32_t n, float t, State& s, Undo& undo) {
    Blended how = BlendDraw(n, t, s, undo);
    g_sus.last = false;
    Frame& cur = g_frames[g_cur];
    if (!g_sus.on || n >= cur.snaps.size()) return how;
    // a draw in the state of the one before it is judged, and counted, as that one
    bool follows = cur.snaps[n].leader >= 0;
    uint32_t at = follows ? uint32_t(cur.snaps[n].leader) : n;
    const Frame& prev = g_frames[g_cur ^ 1];
    Snap& sn = cur.snaps[at];
    uint32_t why = Suspicion(cur, prev, sn, how);
    if (how == Blended::kJumped) why |= kLoneJump;  // whether lone, Score tells
    uint32_t habits = 0;
    for (uint32_t i = 0; i < 4; i++)
        if ((why >> i & 1) && Habitual(sn.program, i)) habits |= 1u << i;
    why &= ~habits;
    g_sus.last = (why & SuspectMask() & ~uint32_t(kLoneJump)) != 0;
    if (follows) return how;
    if (habits) g_sus.habitual++;
    if (how == Blended::kJumped) g_sus.jumped++;
    else if (how != Blended::kNotFound) g_sus.blended++;
    for (size_t i = 0; i < 4; i++)
        if ((why >> i & 1) && g_sus.by[i]++ < kLines)
            g_sus.lines[i] += SuspectLine(at, prev, sn) + (i == 3 ? ", jumped" : "");
    return how;
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
}

void EndPass(uint32_t slot) {
    std::string suspects = Score();
    if (!g_pass.on || slot >= std::size(g_notes)) return;
    char b[384];
    snprintf(b, sizeof(b),
             "drawn again at t %.2f: %u draws, %u not found, %u found as another detail's, %u moved as a neighbour found, "
             "%u as another program's, %u with registers that jumped, "
             "%u palette registers another draw left that jumped; at other addresses found within %.4f (by the frame)",
             g_pass.t, g_pass.draws, g_pass.unfound, g_pass.second, g_pass.borrowed, g_pass.across, g_pass.jumped, g_pass.kept,
             g_loose);
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
