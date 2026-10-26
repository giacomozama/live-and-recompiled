// The GP's effects on the host; see dsfx.h. The state layouts are read off the image's own DSP code (XDK 5849, as
// Conker ships it: the library decrypts the code with 0x4CD361 before the GP loads it at P:0x171).
#include "xdk/dsfx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "xdk/layout.h"

namespace dsfx {
namespace {

constexpr uint32_t kXram = 0x1000, kStateBase = 0x80;  // the kernel loads the states at x:0x80 + header[0]
constexpr uint32_t kBins = 0x1400, kBinsAlias = 0xC00, kBinWords = 32 * kFrame;
constexpr float kRate = 48000;

// state words every effect starts with: scratch offset and size, Y offset and size, then flags
constexpr uint32_t kFlags = 4;
constexpr uint32_t kInit = 1, kMix = 2;  // flags: has run once; adds into its outputs instead of replacing them

enum class Kind { Unknown, FanOut, Iir2, Reverb, Xtalk, Mix2, Capture };

struct Reverb {
    std::vector<float> in;  // the room-filtered input, read back at the reflection and late delays
    uint32_t in_pos = 0;
    float shelf = 0;  // room HF one-pole
    std::array<std::vector<float>, 2> ap;
    std::array<uint32_t, 2> ap_pos{};
    std::array<std::vector<float>, 4> line;
    std::array<uint32_t, 4> pos{};
    std::array<float, 4> damp{};
};

struct Effect {
    Kind kind = Kind::Unknown;
    uint32_t base = 0;  // x: address of its state
    float h[4] = {};    // IIR2: x1, x2, y1, y2, which the DSP keeps in Y memory
    std::array<std::array<float, 11>, 4> delay{};  // crosstalk: the two passes' delay lines
    float f[4] = {};
    uint32_t n[2] = {};
    Reverb rev;
};

uint32_t* g_x = nullptr;  // X memory words: the effects' states, where the title reads and writes them too
float g_buf[kXram];    // audio in X memory below the bins: the temp buffers effects pass between them
float* g_bins = nullptr;
std::vector<Effect> g_fx;

// reverb parameters, from the I3DL2 listener
struct Late {
    float room = 0, shelf_a = 0, room_hf = 1, early = 0, late = 0;
    uint32_t taps[4] = {1, 1, 1, 1}, late_delay = 1;
    float ap_g = 0;
    uint32_t len[4] = {1, 1, 1, 1};
    float fb[4] = {}, p[4] = {}, scale[4] = {};
} g_rev;

float F(uint32_t w) { return float(int32_t(w << 8)) / 2147483648.0f; }  // a 24 bit fraction
float Db(int32_t mb) { return mb <= -10000 ? 0.0f : std::pow(10.0f, mb / 2000.0f); }

// the 32 words at x:a as audio: the bins (and their alias at 0xC00), or a temp buffer
float* Buf(uint32_t a) {
    if (a >= kBins && a + kFrame <= kBins + kBinWords) return g_bins + (a - kBins);
    if (a >= kBinsAlias && a + kFrame <= kBinsAlias + kBinWords) return g_bins + (a - kBinsAlias);
    if (a + kFrame <= kXram) return g_buf + a;
    static float sink[kFrame];
    return sink;
}
uint32_t X(const Effect& e, uint32_t i) { return g_x[(e.base + i) % kXram] & 0xFFFFFF; }

// (in, outs x8, count, gains x8): each output gets the input at its gain
void FanOut(Effect& e, uint32_t n) {
    const float* in = Buf(X(e, 5));
    bool mix = X(e, kFlags) & kMix;
    for (uint32_t k = 0; k < std::min<uint32_t>(X(e, 14), 8); k++) {
        float* out = Buf(X(e, 6 + k));
        float g = F(X(e, 15 + k));
        for (uint32_t i = 0; i < n; i++) out[i] = (mix ? out[i] : 0) + g * in[i];
    }
}

// (in, out, five coefficients): a biquad run with the scaling mode on, so its feedback and its history are twice
// the accumulator, and its output is the accumulator itself
void Iir2(Effect& e, uint32_t n) {
    const float* in = Buf(X(e, 5));
    float* out = Buf(X(e, 6));
    float c[5];
    for (int k = 0; k < 5; k++) c[k] = F(X(e, 7 + k));
    bool mix = X(e, kFlags) & kMix;
    for (uint32_t i = 0; i < n; i++) {
        float x = in[i];
        float acc = c[0] * x + c[1] * e.h[0] + c[2] * e.h[1] + c[3] * e.h[2] + c[4] * e.h[3];
        float y = std::clamp(2 * acc, -1.0f, 1.0f);
        e.h[1] = e.h[0];
        e.h[0] = x;
        e.h[3] = e.h[2];
        e.h[2] = y;
        out[i] = (mix ? out[i] : 0) + acc;
    }
}

// (in L, in R x2, out L, out R x2, ...): two crosstalk cancellers, each feeding the other side's output back 11
// samples later through a one-pole at +63 and +64. Without the mix flag the inputs pass straight through.
void Xtalk(Effect& e, uint32_t n) {
    if (!(X(e, kFlags) & kInit)) {
        g_x[e.base + 63] = 0x7645A1;
        g_x[e.base + 64] = 0xF88313;
    }
    float c0 = F(X(e, 63)), c1 = F(X(e, 64));
    bool on = X(e, kFlags) & kMix;
    for (int p = 0; p < 2; p++) {
        const float* l = Buf(X(e, 5 + p * 2));
        const float* r = Buf(X(e, 6 + p * 2));
        float* ol = Buf(X(e, 9 + p * 2));
        float* oR = Buf(X(e, 10 + p * 2));
        auto& da = e.delay[p * 2];
        auto& db = e.delay[p * 2 + 1];
        float& f0 = e.f[p * 2];
        float& f1 = e.f[p * 2 + 1];
        uint32_t& k = e.n[p];
        for (uint32_t i = 0; i < n; i++) {
            f0 = c0 * f0 + c1 * da[k];
            f1 = c0 * f1 + c1 * db[k];
            float a = l[i] + f1, b = r[i] + f0;
            da[k] = a;
            db[k] = b;
            ol[i] += on ? a : l[i];
            oR[i] += on ? b : r[i];
            k = (k + 1) % 11;
        }
    }
}

// (in 0, in 1, out, gain 0, gain 1)
void Mix2(Effect& e, uint32_t n) {
    const float* a = Buf(X(e, 5));
    const float* b = Buf(X(e, 6));
    float* out = Buf(X(e, 7));
    float g0 = F(X(e, 8)), g1 = F(X(e, 9));
    bool mix = X(e, kFlags) & kMix;
    for (uint32_t i = 0; i < n; i++) out[i] = (mix ? out[i] : 0) + g0 * a[i] + g1 * b[i];
}

// the I3DL2 reverb, (in L, in R, out FL, FR, BL, BR). The DSP's own is a 1200 word program; this one keeps its
// routing and the listener's levels, delays and decay: the room filter, four early taps and a four line
// feedback delay network behind two diffusing allpasses, each half the total energy of its level.
void Rev(Effect& e, uint32_t n) {
    Reverb& r = e.rev;
    if (r.line[0].empty()) {
        r.in.assign(size_t(0.5f * kRate), 0.0f);
        for (int k = 0; k < 2; k++) r.ap[k].assign(k ? 379 : 142, 0.0f);
        for (int k = 0; k < 4; k++) r.line[k].assign(5000, 0.0f);
    }
    const float* l = Buf(X(e, 5));
    const float* rr = Buf(X(e, 6));
    float* out[4];
    for (int k = 0; k < 4; k++) out[k] = Buf(X(e, 7 + k));
    bool mix = X(e, kFlags) & kMix;
    const Late& p = g_rev;
    uint32_t size = uint32_t(r.in.size());
    for (uint32_t i = 0; i < n; i++) {
        float x = (l[i] + rr[i]) * 0.5f;
        r.shelf = x + p.shelf_a * (r.shelf - x);
        r.in[r.in_pos] = p.room * (r.shelf + p.room_hf * (x - r.shelf));
        auto back = [&](uint32_t d) { return r.in[(r.in_pos + size - std::min(d, size - 1)) % size]; };
        float early[4], y = back(p.late_delay) * p.late;
        for (int k = 0; k < 2; k++) {
            float& z = r.ap[k][r.ap_pos[k]];
            float v = y + p.ap_g * z;
            y = z - p.ap_g * v;
            z = v;
            r.ap_pos[k] = (r.ap_pos[k] + 1) % uint32_t(r.ap[k].size());
        }
        float s[4], sum = 0;
        for (int k = 0; k < 4; k++) {
            early[k] = back(p.taps[k]) * p.early;
            s[k] = r.line[k][(r.pos[k] + 5000 - p.len[k]) % 5000];
            r.damp[k] = p.fb[k] * ((1 - p.p[k]) * s[k] + p.p[k] * r.damp[k]);
            sum += r.damp[k];
        }
        for (int k = 0; k < 4; k++) {
            // Householder: an orthogonal mix keeps the decay the lines set
            float v = r.damp[k] - 0.5f * sum + y * p.scale[k];
            r.line[k][r.pos[k]] = std::abs(v) < 1e-20f ? 0.0f : v;
            r.pos[k] = (r.pos[k] + 1) % 5000;
            out[k][i] = (mix ? out[k][i] : 0) + early[k] + s[k];
        }
        r.in_pos = (r.in_pos + 1) % size;
    }
}

Kind KindOf(uint32_t code, uint32_t state) {
    struct {
        uint32_t code, state;
        Kind kind;
    } static const kKnown[] = {{0x90, 0xB8, Kind::FanOut},  {0xC0, 0x9C, Kind::Iir2},    {0x12EC, 0x8A8, Kind::Reverb},
                               {0x1D0, 0x174, Kind::Xtalk}, {0xA8, 0x70, Kind::Mix2},    {0x1F0, 0x9C, Kind::Capture}};
    for (auto& k : kKnown)
        if (k.code == code && k.state == state) return k.kind;
    return Kind::Unknown;
}
const char* Name(Kind k) {
    static const char* const kNames[] = {"unknown", "fan-out", "iir2", "i3dl2 reverb", "crosstalk", "2:1 mixer",
                                         "capture"};
    return kNames[int(k)];
}

}  // namespace

bool Load(const std::vector<uint8_t>& img, uint32_t* xmem) {
    auto u32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4);
        return v;
    };
    g_fx.clear();
    g_x = xmem;
    std::memset(g_x, 0, kXram * 4);
    std::memset(g_buf, 0, sizeof g_buf);
    uint32_t code_words = u32(XDK_EffectsImage_Skip0), state_words = u32(XDK_EffectsImage_Skip1);
    size_t states = XDK_EffectsImage_Desc + size_t(code_words) * 4;  // the state words follow the code
    size_t t = states + size_t(state_words) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64), at = kStateBase + u32(XDK_EffectsImage_Desc - 0x18);
    for (uint32_t i = 0; i < state_words && at + i < kXram; i++) g_x[at + i] = u32(states + i * 4) & 0xFFFFFF;
    bool known = false;
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;  // DSEFFECTMAP: code, state, Y and scratch segments, each offset and size
        Effect e;
        e.kind = KindOf(u32(m + 4), u32(m + 12));
        e.base = at + uint32_t((u32(m + 8) - states) / 4);
        known |= e.kind != Kind::Unknown;
        LOGI("dsound: effect %u %s, state at x:%04X", i, Name(e.kind), e.base);
        g_fx.push_back(std::move(e));
    }
    return known;
}

bool Loaded() { return !g_fx.empty(); }
uint32_t Count() { return uint32_t(g_fx.size()); }
uint32_t StateOf(uint32_t effect) { return effect < g_fx.size() ? g_fx[effect].base : 0; }

void Process(float* bins, uint32_t n) {
    g_bins = bins;
    n = std::min(n, kFrame);
    for (Effect& e : g_fx) {
        switch (e.kind) {
            case Kind::FanOut: FanOut(e, n); break;
            case Kind::Iir2: Iir2(e, n); break;
            case Kind::Reverb: Rev(e, n); break;
            case Kind::Xtalk: Xtalk(e, n); break;
            case Kind::Mix2: Mix2(e, n); break;
            // decimates its bin into a ring in system memory for the title to read: nothing to hear
            case Kind::Capture:
            case Kind::Unknown: break;
        }
        g_x[e.base + kFlags] |= kInit;
    }
    g_bins = nullptr;
}

void SetData(uint32_t effect, uint32_t offset, const uint32_t* data, uint32_t count) {
    if (effect >= g_fx.size()) return;
    for (uint32_t i = 0; i < count; i++) g_x[(g_fx[effect].base + offset / 4 + i) % kXram] = data[i] & 0xFFFFFF;
}
void GetData(uint32_t effect, uint32_t offset, uint32_t* data, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        data[i] = effect < g_fx.size() ? g_x[(g_fx[effect].base + offset / 4 + i) % kXram] : 0;
}

void SetI3dl2(const I3dl2& l) {
    Late& p = g_rev;
    p.room = Db(std::clamp(l.room, -10000, 0));
    p.room_hf = Db(std::clamp(l.room_hf, -10000, 0));
    p.shelf_a = std::exp(-2 * 3.14159265f * std::clamp(l.hf_reference, 20.0f, 20000.0f) / kRate);
    // reflections and reverb are relative to the room; each set of four taps or lines carries half its energy to
    // each side once the backs fold onto the fronts
    p.early = Db(std::clamp(l.reflections, -10000, 1000)) * 0.5f;
    p.late = Db(std::clamp(l.reverb, -10000, 2000));
    float refl = std::clamp(l.reflections_delay, 0.0f, 0.3f), rev = std::clamp(l.reverb_delay, 0.0f, 0.1f);
    for (int k = 0; k < 4; k++) p.taps[k] = std::max(1u, uint32_t(refl * (1 + 0.13f * k) * kRate));
    p.late_delay = std::max(1u, uint32_t((refl + rev) * kRate));
    p.ap_g = 0.7f * std::clamp(l.diffusion, 0.0f, 100.0f) / 100;
    float density = 0.4f + 0.6f * std::clamp(l.density, 0.0f, 100.0f) / 100;
    float decay = std::clamp(l.decay, 0.1f, 20.0f), hf = decay * std::clamp(l.decay_hf_ratio, 0.1f, 1.0f);
    static const uint32_t kLen[4] = {1597, 1949, 2311, 2729};
    for (int k = 0; k < 4; k++) {
        p.len[k] = std::max(100u, uint32_t(kLen[k] * density));
        p.fb[k] = std::pow(10.0f, -3.0f * p.len[k] / (decay * kRate));
        float r = std::pow(10.0f, -3.0f * p.len[k] / (hf * kRate)) / p.fb[k];
        p.p[k] = (1 - r) / (1 + r);
        p.scale[k] = std::sqrt(1 - p.fb[k] * p.fb[k]) * 0.5f;
    }
}

}  // namespace dsfx
