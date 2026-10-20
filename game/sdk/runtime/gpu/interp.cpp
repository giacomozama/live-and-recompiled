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
    k.data = d;
    return k;
}

// the constant registers a program reads (ProgramReads), as a list
struct Reads {
    std::bitset<192> bits;
    std::vector<uint8_t> regs;
};

const Reads* ReadsOf(const State& s) {
    if (!((s.vs_handle & 1) && s.vs && !s.vs->program.empty())) return nullptr;
    static std::unordered_map<uint64_t, Reads> reads;
    auto [it, fresh] = reads.try_emplace(s.vs->hash);
    if (fresh) {
        it->second.bits = ProgramReads(s.vs->program);
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
    int32_t found = -1;  // the draw of the frame before it was found as
    float distance = 0;  // from it, per value read (Distance)
    uint32_t candidates = 0;  // draws it was found among
    bool by_exact = false;
    std::string why;     // not found, when recording or tracing: what keeps it from the nearest draw of its program
};

struct Frame {
    std::unordered_map<uint64_t, std::vector<uint32_t>> loose, programs;  // the draws of each key
    std::vector<Snap> snaps;
    std::vector<std::array<float, 4>> values;  // the snaps' constants
    std::vector<bool> taken;  // found by a draw of the frame after
    const std::array<float, 4>* Values(const Snap& sn) const { return values.data() + sn.at; }
    void Clear() {
        loose.clear();
        programs.clear();
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
float Distance(const Frame& fa, const Snap& a, const Frame& fb, const Snap& b, float limit) {
    float d = 0;
    if (b.reads) {
        const std::array<float, 4>* x = fa.Values(a);
        const std::array<float, 4>* y = fb.Values(b);
        for (size_t j = 0; j < b.reads->regs.size(); j++) {
            for (size_t c = 0; c < 4; c++) d += Rel(x[j][c], y[j][c]);
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
    float best_d = 0;
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
        float d = Distance(prev, prev.snaps[i], cur, sn, best < 0 ? INFINITY : best_d);
        bool same = prev.snaps[i].parts[kAddresses] == sn.parts[kAddresses];
        if (best < 0 || d < best_d || (d == best_d && same && !best_same)) best = int32_t(i), best_d = d, best_same = same;
        if (best_d == 0 && best_same) break;
    }
    if (best < 0) return false;
    size_t values = sn.reads ? sn.reads->regs.size() * 4 : kXf * 16;
    sn.found = best;
    sn.distance = values ? best_d / float(values) : 0;
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
    uint32_t draws = 0, unfound = 0, jumped = 0;
    std::unordered_map<uint64_t, std::pair<uint32_t, std::string>> unfound_programs;  // count, the first's reason
    std::string jumps;  // the first draws with a jump: draw, program, registers
    std::unordered_map<uint64_t, std::pair<float, std::string>> farthest;  // per program, its draw farthest from what it was found as
    std::unordered_map<uint64_t, uint32_t> jump_lines;                     // per program, draws listed in jumps
    uint32_t rewritten = 0;  // draws whose vertices changed since the frame drew them, by program
    std::unordered_map<uint64_t, uint32_t> rewritten_programs;
};
Pass g_pass;
std::string g_notes[8];

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
// flag or index; the scale grows with the value, which keeps a large coordinate's walk
bool Jumped(float a, float b) { return std::fabs(b - a) > 1.0f + 0.5f * std::max(std::fabs(a), std::fabs(b)); }

// n floats at t from a to b, into out (which may be b); false when one jumped, out untouched. A value that stayed
// the same, bit for bit, never jumped: some registers hold packed data that isn't a number
bool Lerp(const float* a, const float* b, size_t n, float t, float* out) {
    for (size_t i = 0; i < n; i++) {
        if (std::memcmp(&a[i], &b[i], 4) == 0) continue;
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || Jumped(a[i], b[i])) return false;
    }
    for (size_t i = 0; i < n; i++)
        if (std::memcmp(&a[i], &b[i], 4) != 0) out[i] = a[i] + (b[i] - a[i]) * t;
    return true;
}

}  // namespace

bool Tracing() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_INTERP_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
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
    Frame& prev = g_frames[g_cur ^ 1];
    int32_t leader = -1;
    if (same && !Tracing() && Follows(cur, s, leader)) {
        Snap& f = cur.snaps.emplace_back();
        f.leader = leader;
        return;
    }
    Keys k = MakeKeys(s, prim, start, count, indices);
    uint32_t n = uint32_t(cur.snaps.size());
    Snap& sn = cur.snaps.emplace_back();
    sn.reads = ReadsOf(s);
    if (sn.reads) {
        sn.at = uint32_t(cur.values.size());
        for (uint8_t r : sn.reads->regs) cur.values.push_back(s.vsc[r]);
    } else {
        for (size_t i = 0; i < kXf; i++) sn.xf[i] = s.transforms[size_t(kTransforms[i])];
    }
    sn.data = k.data;
    sn.program = k.program;
    sn.parts = k.parts;
    cur.loose[k.loose].push_back(n);
    cur.programs[k.program].push_back(n);

    // among the draws alike but for their addresses (a dynamic buffer's draw is still the same draw), the one whose
    // constants are nearest: a draw whose own moved must not take another left at its addresses
    Nearest(prev, cur, prev.loose, k.loose, cur.loose[k.loose].size() - 1, sn);
    sn.by_exact = sn.found >= 0 && prev.snaps[size_t(sn.found)].parts[kAddresses] == sn.parts[kAddresses];
    bool by_exact = sn.by_exact;
    if (sn.found >= 0) prev.taken[size_t(sn.found)] = true;
    else if (Tracing() || Recording()) sn.why = Why(prev, sn);
    if (Tracing()) {
        g_t.draws++;
        Trace(cur, sn, prev, sn.found >= 0 ? &prev.snaps[size_t(sn.found)] : nullptr, by_exact, k.program);
    }
}

void EndFrame() {
    if (Tracing()) {
        Frame& prev = g_frames[g_cur ^ 1];
        g_t.gone += uint64_t(std::count(prev.taken.begin(), prev.taken.end(), false));
        if (++g_t.frames >= kLogEvery) {
            Log();
            g_t = {};
        }
    }
    g_cur ^= 1;
    g_frames[g_cur].Clear();
    g_frames[g_cur ^ 1].taken.assign(g_frames[g_cur ^ 1].snaps.size(), false);
}

bool Blend(uint32_t n, float t, State& s, Undo& undo) {
    const Frame& cur = g_frames[g_cur];
    const Frame& prev = g_frames[g_cur ^ 1];
    if (g_pass.on) g_pass.draws++;
    if (n < cur.snaps.size() && cur.snaps[n].leader >= 0) n = uint32_t(cur.snaps[n].leader);
    if (n >= cur.snaps.size() || cur.snaps[n].found < 0) {
        if (g_pass.on) {
            g_pass.unfound++;
            auto& u = g_pass.unfound_programs[ProgramOf(s)];
            if (!u.first++ && n < cur.snaps.size()) u.second = cur.snaps[n].why;
        }
        return false;
    }
    const Snap& sn = cur.snaps[n];
    const Snap& was = prev.snaps[size_t(sn.found)];
    uint64_t program = ProgramOf(s);
    if (g_pass.on) {
        char b[128];
        snprintf(b, sizeof(b), "\n    draw %u program %016llX: distance %.4f, nearest of %u, %s", n,
                 (unsigned long long)program, sn.distance, sn.candidates, sn.by_exact ? "exact" : "without addresses");
        auto& f = g_pass.farthest[program];
        if (f.second.empty() || sn.distance > f.first) f = {sn.distance, b};
    }
    // what its program reads, or the fixed function's transforms, that jumped: a register moves whole, as a matrix
    // does, and one that jumped is left as it is
    const std::array<float, 4>* old = prev.Values(was);
    std::string regs;
    bool jumped = false;
    if (sn.reads) {
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            float tmp[4];
            if (Lerp(old[j].data(), s.vsc.v[r].data(), 4, t, tmp)) continue;
            jumped = true;
            if (g_pass.on && regs.size() < 200) {
                const float* a = old[j].data();
                const float* c = s.vsc.v[r].data();
                char b[128];
                snprintf(b, sizeof(b), " c%u (%g %g %g %g -> %g %g %g %g)", r, a[0], a[1], a[2], a[3], c[0], c[1], c[2],
                         c[3]);
                regs += b;
            }
        }
    } else {
        for (size_t i = 0; i < kXf; i++) {
            float tmp[16];
            if (Lerp(was.xf[i].data(), s.transforms[size_t(kTransforms[i])].data(), 16, t, tmp)) continue;
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
    if (jumped && whole) return false;
    if (sn.reads) {
        for (size_t j = 0; j < sn.reads->regs.size(); j++) {
            uint8_t r = sn.reads->regs[j];
            if (std::memcmp(&old[j], &s.vsc.v[r], 16) == 0) continue;
            undo.vsc.emplace_back(uint32_t(r), s.vsc.v[r]);
            Lerp(old[j].data(), s.vsc.v[r].data(), 4, t, s.vsc.v[r].data());
        }
        return true;
    }
    for (size_t i = 0; i < kXf; i++) {
        auto& m = s.transforms[size_t(kTransforms[i])];
        if (std::memcmp(&was.xf[i], &m, sizeof(m)) == 0) continue;
        undo.xf.emplace_back(uint32_t(kTransforms[i]), m);
        Lerp(was.xf[i].data(), m.data(), 16, t, m.data());
    }
    return true;
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
}

void EndPass(uint32_t slot) {
    if (!g_pass.on || slot >= std::size(g_notes)) return;
    char b[160];
    snprintf(b, sizeof(b), "drawn again at t %.2f: %u draws, %u not found, %u with registers that jumped", g_pass.t,
             g_pass.draws, g_pass.unfound, g_pass.jumped);
    std::string note = b;
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
    g_notes[slot] = std::move(note);
    g_pass.on = false;
}

const std::string& Note(uint32_t slot) {
    static const std::string none;
    return slot < std::size(g_notes) ? g_notes[slot] : none;
}

}  // namespace gpu::interp
