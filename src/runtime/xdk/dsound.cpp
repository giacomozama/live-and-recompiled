// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "kernel/memory.h"
#include "xbe/xbe.h"
#include "xdk/d3d8.h"
#include "xdk/dsfx.h"
#include "xdk/dsound.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// configs written before the runtime knew the name
#ifndef SYM_IDirectSound_SynchPlayback
#define SYM_IDirectSound_SynchPlayback FFFFFF00
#define HAS_IDirectSound_SynchPlayback 0
#endif
#ifndef SYM_IDirectSound_DownloadEffectsImage
#define SYM_IDirectSound_DownloadEffectsImage FFFFFF01
#define HAS_IDirectSound_DownloadEffectsImage 0
#endif
#ifndef SYM_IDirectSound_GetEffectData
#define SYM_IDirectSound_GetEffectData FFFFFF02
#define HAS_IDirectSound_GetEffectData 0
#endif
#ifndef SYM_IDirectSound_CommitEffectData
#define SYM_IDirectSound_CommitEffectData FFFFFF03
#define HAS_IDirectSound_CommitEffectData 0
#endif
#ifndef SYM_IDirectSound_GetOutputLevels
#define SYM_IDirectSound_GetOutputLevels FFFFFF04
#define HAS_IDirectSound_GetOutputLevels 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPlayRegion
#define SYM_IDirectSoundBuffer_SetPlayRegion FFFFFF05
#define HAS_IDirectSoundBuffer_SetPlayRegion 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetAllParameters
#define SYM_IDirectSoundBuffer_SetAllParameters FFFFFF06
#define HAS_IDirectSoundBuffer_SetAllParameters 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetFormat
#define SYM_IDirectSoundBuffer_SetFormat FFFFFF07
#define HAS_IDirectSoundBuffer_SetFormat 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetOutputBuffer
#define SYM_IDirectSoundBuffer_SetOutputBuffer FFFFFF08
#define HAS_IDirectSoundBuffer_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetRolloffCurve
#define SYM_IDirectSoundBuffer_SetRolloffCurve FFFFFF09
#define HAS_IDirectSoundBuffer_SetRolloffCurve 0
#endif
#ifndef SYM_IDirectSoundBuffer_StopEx
#define SYM_IDirectSoundBuffer_StopEx FFFFFF0A
#define HAS_IDirectSoundBuffer_StopEx 0
#endif
#ifndef SYM_DirectSoundUseLightHRTF
#define SYM_DirectSoundUseLightHRTF FFFFFF0B
#define HAS_DirectSoundUseLightHRTF 0
#endif
#ifndef SYM_IDirectSoundStream_SetEG
#define SYM_IDirectSoundStream_SetEG FFFFFF0C
#define HAS_IDirectSoundStream_SetEG 0
#endif
#ifndef SYM_IDirectSoundStream_SetOutputBuffer
#define SYM_IDirectSoundStream_SetOutputBuffer FFFFFF0D
#define HAS_IDirectSoundStream_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundStream_SetPitch
#define SYM_IDirectSoundStream_SetPitch FFFFFF0E
#define HAS_IDirectSoundStream_SetPitch 0
#endif
#ifndef SYM_IDirectSoundStream_SetLFO
#define SYM_IDirectSoundStream_SetLFO FFFFFF0F
#define HAS_IDirectSoundStream_SetLFO 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPitch
#define SYM_IDirectSoundBuffer_SetPitch FFFFFF10
#define HAS_IDirectSoundBuffer_SetPitch 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetLFO
#define SYM_IDirectSoundBuffer_SetLFO FFFFFF11
#define HAS_IDirectSoundBuffer_SetLFO 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetEG
#define SYM_IDirectSoundBuffer_SetEG FFFFFF12
#define HAS_IDirectSoundBuffer_SetEG 0
#endif
#ifndef SYM_IDirectSoundBuffer_GetVoiceProperties
#define SYM_IDirectSoundBuffer_GetVoiceProperties FFFFFF13
#define HAS_IDirectSoundBuffer_GetVoiceProperties 0
#endif
#ifndef SYM_IDirectSoundStream_GetVoiceProperties
#define SYM_IDirectSoundStream_GetVoiceProperties FFFFFF14
#define HAS_IDirectSoundStream_GetVoiceProperties 0
#endif
#ifndef SYM_IDirectSound_CreateSoundStream
#define SYM_IDirectSound_CreateSoundStream FFFFFF15
#define HAS_IDirectSound_CreateSoundStream 0
#endif

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;  // E_PENDING, E_ABORT

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
};

// the cubic and sinc resamplers' window: kHalf frames up to the one playing and kHalf after it
constexpr int kHalf = 8, kTaps = 2 * kHalf;

// XACT's envelopes, LFOs and filters, as the library writes DSENVELOPEDESC, DSLFODESC and DSFILTERDESC into the VP's
// voice registers (0x4CA942, 0x4CA813, 0x4CAAD4) and the VP steps them, once every 32 samples (xemu's vp.c)
constexpr uint32_t kModFrame = 32;
enum EnvState : uint8_t { kEnvOff, kEnvDelay, kEnvAttack, kEnvHold, kEnvDecay, kEnvSustain, kEnvRelease, kEnvDone };
struct Envelope {
    uint8_t start = kEnvOff;  // DSEG_MODE: the state a voice starts in (off, delay, attack or hold)
    uint16_t delay = 0, attack = 0, hold = 0, decay = 0, release = 0;  // 12 bits each, in 512 sample units
    uint8_t sustain = 0;
    int8_t pitch = 0, cutoff = 0;  // the multi envelope's scales, 32 pitch units (1/128 octave) a step
    uint8_t cur = kEnvOff;
    int32_t count = 0;
    float level = 255;  // as the VP last set it; a release decays from it
};
struct Lfo {
    uint32_t delay = 0, delta = 0;  // in 512 sample units; the rate, delta / 65536 of a cycle per 32 samples
    int8_t pitch = 0, cutoff = 0, amp = 0;
    uint32_t count = 0;
    double phase = 0;
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0, play_start = 0, play_len = 0;
    uint32_t owned = 0;  // memory the library made for the buffer
    bool playing = false, looping = false, paused = false;
    bool sync = false;  // held until IDirectSound_SynchPlayback
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    bool hold = false;      // stream: paused with no activate, plays nothing until resumed
    bool starved = false;   // stream: played out the last packet it was given
    bool flushing = false;  // stream: stopped by an async FlushEx, its packets are flushed at the next DoWork
    // stream: that FlushEx asked for the release envelope, and the flush that ends it is done. XACT waits for the
    // ENVELOPECOMPLETE status before it stops reading the wave; without it, it streamed the rest after every stop.
    bool releasing = false, released = false;
    double pos = 0;  // frames
    // the pitch, as the library keeps it (4.12 octaves from 48 kHz): SetFormat sets it to the format's rate, SetPitch
    // and SetFrequency to theirs
    bool has_pitch = false;
    double pitch = 0;
    uint32_t flags = 0;  // DSBUFFERDESC / DSSTREAMDESC flags
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1, headroom = 1;
    uint8_t nbins = 0, bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    std::vector<float> curve;  // rolloff
    bool mixin = false;         // DSBCAPS_MIXIN: plays the submix of the voices routed to it
    uint32_t out = 0;           // SetOutputBuffer: the mixin buffer its multipass sends feed
    std::vector<float> submix;  // mixin: this render's submix
    bool is3d = false;
    uint32_t mode = 0;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    bool calc3d = false;  // the title's own 3D calculator drives the voice (dsound::SetVoice3d)
    float pan3d = 0;
    dsound::Voice3dTerms t3d{};  // the calculator's terms, as the voice last took them
    double energy = 0;  // XBE_TRACE_DSOUND: sum of squares mixed since the last report
    double recent = 0;  // F3: mean square mixed, smoothed over about half a second
    int64_t fed = 0, done = 0, born = 0;  // F3: steady ms of the last packet in, the last packet out, creation
    float lp[2][8] = {};  // the I3DL2 filters' last outputs, direct and room, by channel
    // XACT's shaping: amplitude and multi (pitch and cutoff) envelopes, the multi and pitch LFOs, the DLS2 filter
    Envelope ea, ef;
    Lfo lfo[2];
    uint32_t fmode = 0;   // DSFILTERDESC mode: 0 bypass, 1 DLS2, 2 parametric EQ, 3 DLS2 and EQ (mono)
    int32_t fc = 0;       // DLS2 cutoff, 4.12 octaves of the filter's coefficient
    uint32_t fq = 0;      // DLS2 damping, 0x8000 the least resonant
    float svf[8][2] = {};  // the filter's low and band pass state by channel
    uint32_t tick = 0;     // samples left in this envelope frame
    float amp_from = 1, amp_to = 1, mod_pitch = 0, mod_fc = 0;
    bool in_release = false;  // a release envelope is playing out before the voice stops or its flush completes
    uint32_t rel_speed = 0;   // VP frames the release steps per frame once the title waits on it (0 as written)
    int64_t rel_at = 0;       // XBE_TRACE_RELEASE: steady ms the release started
    uint32_t rel_polls = 0;   // XBE_TRACE_RELEASE: GetStatus calls while it played
    bool rel_late = false;    // XBE_TRACE_RELEASE: reported as overrunning its length
    uint32_t stop_polls = 0;  // XBE_TRACE_RELEASE: GetStatus calls since the last StopEx or FlushEx, 0 none since
    // the resampler's window (cubic, sinc), by channel: entry i is frame win_f - (kHalf - 1) + i of the voice's data
    float win[kTaps][8] = {};
    int win_fill = -1;     // entries read from the data, from the first; -1 starts the window afresh
    int64_t win_f = 0;
    double win_seen = -1;  // pos as the last Pull left it; a pos moved by anything else starts the window afresh
    // ADPCM block cache
    int64_t block = -1;
    int16_t decoded[65 * 8];
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
std::atomic<int64_t> g_mixed{0};           // the last Render, steady ms
std::atomic<int64_t> g_ran{0};             // the last callback run, steady ms
bool g_started = false;
constexpr uint32_t kGpXmem = 0xFE830000;  // the GP's X memory in the APU window, a dword per word
constexpr uint32_t kBins = 32, kMultipass = 31;  // XACT sends a voice to its submix through the last bin
// IDirectSound_SetMixBinHeadroom: the bits the VP takes off what it mixes into each bin. The output stage puts them
// back on the speaker bins, so only what the effects see changes; Conker's GP kernel outputs bins 0-5 as they are.
// The library starts every bin at one and the multipass bin at none (0x4C60E0).
std::array<uint8_t, kBins> DefaultBinHeadroom() {
    std::array<uint8_t, kBins> h;
    h.fill(1);
    h[kMultipass] = 0;
    return h;
}
std::array<uint8_t, kBins> g_bin_headroom = DefaultBinHeadroom();
uint32_t g_ds = 0;
dsfx::I3dl2 g_i3dl2{-10000, 0, 0, 1, 0.5f, -10000, 0, -10000, 0, 0, 0, 5000};  // F3: the last listener

const bool g_trace = std::getenv("XBE_TRACE_DSOUND") != nullptr;
// XBE_TRACE_RELEASE: every release envelope a stop or flush starts, how long it was meant to take and took, and how
// often the title polled the voice meanwhile (a title waiting on it before it goes on)
const bool g_trace_rel = std::getenv("XBE_TRACE_RELEASE") != nullptr;
int64_t NowMs();
uint32_t ReleaseMs(const Voice& v) { return uint32_t(uint64_t(v.ea.release) * 512 * 1000 / 48000); }
// a title still polling a voice long after it asked it to stop: it waits on it, so its whole state
void TraceStopPoll(Voice& v, uint32_t status) {
    if (!g_trace_rel || !v.stop_polls || ++v.stop_polls != 300) return;
    LOGR("release: %08X %s polled 300 times since its stop, status %X: playing %d looping %d paused %d active %d "
         "flushing %d releasing %d released %d in_release %d envelope %u/%d (release %u ms) packets %zu pos %.0f",
         v.obj, v.stream ? "stream" : "buffer", status, v.playing, v.looping, v.paused, v.active, v.flushing,
         v.releasing, v.released, v.in_release, unsigned(v.ea.cur), v.ea.count, ReleaseMs(v), v.packets.size(), v.pos);
}
// XBE_TRACE_RELEASE: a title waiting with the picture stopped (a load, a stop it waits on): every half second after the
// first one, the return addresses on the guest stack of the thread calling dsound, the first exact, later ones maybe
// stale. The esp tells the title's main thread from XACT's.
// the return addresses on the guest stack, the first exact, later ones maybe stale
std::string GuestChain(const X86Context& ctx, int most) {
    std::string chain;
    int found = 0;
    for (uint32_t i = 0; i < 1024 && found < most; i++) {
        const uint32_t at = ctx.esp + i * 4;
        if (!mem::IsCommitted(at)) break;
        const uint32_t r = R32(at);
        if (r < X86CodeBase || r >= X86CodeEnd) continue;
        char t[24];
        std::snprintf(t, sizeof t, " %08X", r);
        chain += t;
        found++;
    }
    return chain;
}
// XBE_TRACE_RELEASE: who makes, starts and flushes streams (the guest call chain)
void TraceStreamCall(const X86Context& ctx, uint32_t obj, const char* name) {
    if (g_trace_rel) LOGR("stream: %08X %s from%s", obj, name, GuestChain(ctx, 16).c_str());
}
void TraceStall(const X86Context& ctx, const char* from) {
    if (!g_trace_rel) return;
    const int64_t since = d3d::MsSincePresent();
    if (since < 1000) return;
    static std::atomic<int64_t> last{0};
    const int64_t now = NowMs();
    int64_t was = last.load();
    if (now - was < 500 || !last.compare_exchange_strong(was, now)) return;
    const std::string chain = GuestChain(ctx, 24);
    LOGR("stall: %lld ms without a frame, %s on esp %08X:%s", (long long)since, from, ctx.esp, chain.c_str());
}
void TraceReleaseEnd(Voice& v, const char* why) {
    if (!g_trace_rel || !v.in_release) return;
    LOGR("release: %08X %s ends (%s) after %lld ms of %u, %u polls", v.obj, v.stream ? "stream" : "buffer", why,
         (long long)(NowMs() - v.rel_at), ReleaseMs(v), v.rel_polls);
}
#define DSTRACE(...) \
    do {                \
        if (g_trace) LOGR(__VA_ARGS__); \
    } while (0)

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 64;  // WAVE_FORMAT_XBOX_ADPCM
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 64 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / 64) * 36 * v.channels;
        if (at != v.block) {
            DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded);
            v.block = at;
        }
        return v.decoded[(f % 64) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

int64_t NowMs();

void Complete(Voice& v, Packet& pk, uint32_t status) {
    v.done = NowMs();
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// the mixin buffer a voice's multipass sends feed, if it has one
Voice* Submix(const Voice& v) {
    auto it = v.out ? g_voices.find(v.out) : g_voices.end();
    return it != g_voices.end() && it->second.mixin && &it->second != &v ? &it->second : nullptr;
}
// a voice's own pitch: SetPitch's or SetFrequency's, else its format's rate (0x4C3995)
double BasePitch(const Voice& v) {
    return v.has_pitch ? v.pitch : 4096.0 * std::log2(double(std::max<uint32_t>(v.rate, 1)) / audio::kRate);
}
// the rate the voice plays at, from the pitch the library writes (0x4CA1AF): its own, plus its 3D descriptor's (the
// Doppler shift) when it was made with 0x1000000, plus its mixin buffer's own and descriptor's, held to -32767..8191
double Rate(const Voice& v) {
    if (v.mixin) return audio::kRate;
    double p = BasePitch(v);
    const bool doppler = app::Get().doppler;
    if (doppler && v.calc3d && (v.flags & 0x1000000)) p += v.t3d.pitch;
    if (const Voice* s = v.out ? Submix(v) : nullptr) p += BasePitch(*s) + (doppler && s->calc3d ? s->t3d.pitch : 0);
    return audio::kRate * std::exp2(std::clamp(p, -32767.0, 8191.0) / 4096.0);
}
// SetFrequency: a rate, or 0 for the format's own (DSBFREQUENCY_ORIGINAL)
void SetFrequency(Voice& v, uint32_t hz) {
    v.has_pitch = hz != 0;
    if (hz) v.pitch = 4096.0 * std::log2(double(hz) / audio::kRate);
}

// mixin buffers between the voice and the bins
int Depth(const Voice& v) {
    int d = 0;
    for (const Voice* s = Submix(v); s && d < 8; s = Submix(*s)) d++;
    return d;
}

bool Speaker(uint32_t b) { return b < 10 && b != 3; }  // fronts, centre, backs and the crosstalk inputs; 3 is the sub

constexpr uint32_t kChannels = 8;  // the most a voice mixes; a hardware voice holds two

struct Route {
    uint8_t bin;
    float g[kChannels];  // each voice channel into the bin
};

// a 3D voice's attenuation in bin b, as the library sets the slot's volume from the calculator's terms (0x4C9F5B):
// distance and cone in every bin, the I3DL2 direct path and the pairs in the 3D bins and the centre, the room only
// in the reverb send, each sum held to -100..0 dB. A sound ahead has its 3D backs off.
float Att3d(const dsound::Voice3dTerms& t, uint32_t b) {
    auto sum = [](int32_t x) { return std::clamp(x, -10000, 0); };
    int32_t d = sum(t.dist + t.cone);
    int32_t mb = b == 2           ? sum(d + t.direct + t.pair_a[0] + t.pair_b[1])
                 : b == 6 || b == 7 ? sum(d + t.direct + t.pair_a[0] + t.pair_b[0])
                 : b == 8 || b == 9 ? sum(d + t.direct + t.pair_a[1])
                 : b == 10          ? sum(d + t.room)
                                    : d;
    return Db(mb);
}

// what the voice puts in each bin, as the VP mixes it: every slot adds its channel at the slot's bin volume, the voice
// volume and headroom, less the bin's headroom bits. A hardware voice holds two channels and owns a run of slots,
// which alternate channels; a mono voice sends its one channel to all of them. With no bins given a voice plays its
// channels on the speakers in WAVE order: front left and right, then centre and sub, then the backs.
int Routes(const Voice& v, Route* r) {
    static const uint8_t kSpeakers[6] = {0, 1, 2, 3, 4, 5}, kQuad[4] = {0, 1, 4, 5};
    uint32_t channels = std::min<uint32_t>(std::max<uint32_t>(v.channels, 1), kChannels);
    const uint8_t* bin = v.nbins ? v.bin : channels == 4 ? kQuad : kSpeakers;
    uint32_t n = v.nbins ? v.nbins : std::clamp<uint32_t>(channels, 2, 6), voices = (channels - 1) / 2 + 1;
    uint32_t per = std::max<uint32_t>(n / voices, 2);
    float att = 1, pan = 0;
    if (v.calc3d) {
        pan = v.pan3d;
    } else if (v.is3d && v.mode != 2) {
        bool rel = v.mode == 1;  // the position is already in listener space
        float d[3] = {v.p[0], v.p[1], v.p[2]};
        for (int i = 0; i < 3 && !rel; i++) d[i] -= g_listener[i];
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist > v.min_dist && v.curve.empty()) {
            att = v.min_dist / std::min(dist, v.max_dist);
        } else if (dist > v.min_dist && v.max_dist > v.min_dist) {
            // points spread evenly from min to max distance, 1.0 at min
            float at = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
            size_t i = std::min(size_t(at), v.curve.size() - 1);
            float from = i ? v.curve[i - 1] : 1.0f;
            att = from + (v.curve[i] - from) * (at - i);
        }
        float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                          g_top[0] * g_front[1] - g_top[1] * g_front[0]};
        float side = rel ? d[0] : d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        pan = dist > 1e-3f ? side / dist : 0.0f;
    }
    // with no effects image, a voice heard only through effects (or a submix it has no target for) plays dry on the
    // front pair, at its loudest send
    bool heard = dsfx::Loaded();
    const float hr = app::Get().voice_headroom ? v.headroom : 1.0f;  // the Audio setting can play every voice without it
    for (uint32_t s = 0; s < n; s++) heard |= Speaker(bin[s]) || (bin[s] == kMultipass && Submix(v));
    int count = 0;
    auto add = [&](uint32_t b, int c, float x) {
        int k = 0;
        while (k < count && r[k].bin != b) k++;
        if (k == count) r[count++] = {uint8_t(b), {}};
        x /= float(1 << g_bin_headroom[b]);
        r[k].g[c] = heard ? r[k].g[c] + x : std::max(r[k].g[c], x);
    };
    for (uint32_t c = 0; c < channels; c++) {
        // channel c is the first or second of hardware voice c / 2, which owns slots [per * (c / 2), per * (c / 2 + 1))
        uint32_t pair = c / 2 * 2 + 1 < channels ? 2 : 1, end = std::min(per * (c / 2 + 1), n);
        for (uint32_t s = per * (c / 2) + c % 2; s < end; s += pair) {
            uint32_t b = bin[s];
            float x = Db(v.bin_mb[b]) * v.volume * hr * (v.calc3d ? Att3d(v.t3d, b) : att);
            float left = std::min(1.0f, 1.0f - pan), right = std::min(1.0f, 1.0f + pan);
            if (!heard) {
                add(0, c, x * left);
                add(1, c, x * right);
            } else {
                // even bins are left, odd right, 2 is the centre
                add(b, c, Speaker(b) && b != 2 ? x * (b & 1 ? right : left) : x);
            }
        }
    }
    return count;
}

// a stream goes quiet: the synch hold ends, a started one loses its pause as well
void Deactivate(Voice& v) {
    v.sync = v.starved = false;
    if (v.active) v.active = v.paused = v.ending = false;
}

// a mixin buffer has no data of its own and mixes what reaches it from the start: Conker never plays its own
bool Plays(const Voice& v) {
    if (v.mixin) return true;
    return !(v.paused || v.sync || v.hold || (v.flushing && !v.in_release) || (!v.stream && !v.playing));
}

// the voice's next frames, its channels interleaved (at most kChannels); silence once it runs out. A mixin buffer
// reads its submix, one channel, as the VP reads the multipass bin.
// one VP step of an envelope, its value 0..1. The curves are xemu's: attack a linear rise, decay an exponential fall
// to the sustain level, release an exponential fall of 60 dB from the level it starts at.
float StepEnv(Envelope& e) {
    switch (e.cur) {
        case kEnvOff:
            e.level = 255;
            return 1;
        case kEnvDelay:
            e.level = 0;
            if (e.count <= 0) e.cur = kEnvAttack, e.count = 0;
            else e.count--;
            return 0;
        case kEnvAttack: {
            int32_t span = e.attack * 16;
            float x = span ? std::min(255.0f, e.count * 255.0f / span) : 255.0f;
            e.level = x;
            if (e.count >= span) e.cur = kEnvHold, e.count = e.hold * 16;
            else e.count++;
            return x / 255;
        }
        case kEnvHold:
            e.level = 255;
            if (e.count <= 0) e.cur = kEnvDecay, e.count = e.decay * 16;
            else e.count--;
            return 1;
        case kEnvDecay: {
            float x = e.decay ? 255.0f * std::pow(0.99988799f, float(e.decay * 16 - e.count) * 4096.0f / e.decay) : 0.0f;
            if (x <= e.sustain + 0.2f || e.count <= 0) {
                e.cur = kEnvSustain;
                x = e.sustain;
            } else {
                e.count--;
            }
            e.level = x;
            return x / 255;
        }
        case kEnvSustain:
            e.level = e.sustain;
            return e.sustain / 255.0f;
        case kEnvRelease: {
            if (!e.release || e.count <= 0) {
                e.cur = kEnvDone;
                return 0;
            }
            float t = std::clamp(1.0f - e.count / (e.release * 16.0f), 0.0f, 1.0f);
            e.count--;
            return std::exp(-6.91f * t) * e.level / 255;
        }
        default: return 0;
    }
}
void StartEnv(Envelope& e) {
    e.cur = e.start <= kEnvHold ? e.start : kEnvOff;
    e.count = e.cur == kEnvDelay ? e.delay * 16 : e.cur == kEnvHold ? e.hold * 16 : 0;
    e.level = e.cur == kEnvOff || e.cur == kEnvHold ? 255.0f : 0.0f;
}
// the VP starts a voice: its envelopes from their start states, its LFOs from their delays, its filter empty
void VoiceOn(Voice& v) {
    StartEnv(v.ea);
    StartEnv(v.ef);
    for (Lfo& l : v.lfo) l.count = l.delay * 16, l.phase = 0;
    std::memset(v.svf, 0, sizeof v.svf);
    v.tick = 0;
    // from the level the amplitude envelope starts at, so its first frame doesn't ramp from full
    const bool silent = app::Get().envelopes && v.ea.cur != kEnvOff && v.ea.cur != kEnvHold;
    v.amp_from = v.amp_to = silent ? 0.0f : 1.0f;
    v.mod_pitch = v.mod_fc = 0;
    TraceReleaseEnd(v, "restarted");
    v.in_release = false;
}
// a stop or flush asking for the release envelope: the voice plays on through it when it has one to play
bool StartRelease(Voice& v) {
    if (!app::Get().envelopes || !v.ea.release) return false;
    for (Envelope* e : {&v.ea, &v.ef}) e->cur = kEnvRelease, e->count = e->release * 16;
    if (g_trace_rel)
        LOGR("release: %08X %s starts, %u ms%s%s%s", v.obj, v.stream ? "stream" : "buffer", ReleaseMs(v),
             v.paused ? " paused" : "", v.hold ? " held" : "", v.looping ? " looping" : "");
    v.in_release = true;
    v.rel_speed = 0;
    v.rel_at = NowMs();
    v.rel_polls = 0;
    v.rel_late = false;
    return true;
}
// the title waits with the picture stopped (no Swap for kStallMs): set by each Render. A release it waits on then
// finishes over kHurryFrames VP frames (250 ms).
constexpr int64_t kStallMs = 150;
constexpr int32_t kHurryFrames = 375;
bool g_stalled = false;
// one VP frame of the envelopes and LFOs: the amplitude to reach by its end, the pitch and cutoff offsets in 4.12
void StepMods(Voice& v) {
    const app::Settings& s = app::Get();
    float amp = 1, pitch = 0, fc = 0;
    if (s.envelopes) {
        // a release the title waits on with the picture stopped (the frontend music's 3 s fade before a level loads,
        // which was most of the load): what is left of it plays out over kHurryFrames
        if (v.in_release && !v.rel_speed && g_stalled) {
            const int32_t left = std::max(v.ea.count, v.ef.count);
            v.rel_speed = uint32_t(std::max<int32_t>(1, (left + kHurryFrames - 1) / kHurryFrames));
            if (g_trace_rel && v.rel_speed > 1)
                LOGR("release: %08X hurried, the picture stopped: %u ms left over %u ms", v.obj,
                     unsigned(int64_t(left) * kModFrame * 1000 / 48000), unsigned(kHurryFrames * kModFrame * 1000 / 48000));
        }
        for (uint32_t k = 1; k < v.rel_speed && v.ea.cur == kEnvRelease; k++) StepEnv(v.ea), StepEnv(v.ef);
        amp = StepEnv(v.ea);
        float f = StepEnv(v.ef);
        pitch += v.ef.pitch * 32.0f * f;
        fc += v.ef.cutoff * 32.0f * f;
    }
    if (s.lfo)
        for (int k = 0; k < 2; k++) {
            Lfo& l = v.lfo[k];
            if (!l.delta) continue;
            if (l.count) {
                l.count--;
                continue;
            }
            l.phase = std::fmod(l.phase + l.delta / 65536.0, 1.0);
            const float p = float(l.phase);
            const float tri = p < 0.25f ? 4 * p : p < 0.75f ? 2 - 4 * p : 4 * p - 4;  // from 0, up first
            pitch += l.pitch * 32.0f * tri;
            if (k == 0) {
                fc += l.cutoff * 32.0f * tri;
                amp *= std::max(0.0f, 1.0f + l.amp * tri / 256.0f);
            }
        }
    v.amp_from = v.amp_to;
    v.amp_to = amp;
    v.mod_pitch = pitch;
    v.mod_fc = fc;
}
// whether the VP runs the voice's DLS2 low-pass: on a 3D voice mode 1 (DLS2 and I3DL2), on a 2D mono one mode 1 or 3
// (0x4CAAD4's modes, xemu's vp.c). Not on a 2D stereo voice, though xemu's table runs mode 1 there too: Conker's
// BrontoThroat music (a 2ch stream) gets a sequence event that adds 100 Hz to a track cutoff that starts at 0 (XACT
// 0x554E61, relative by flag 0x10), so XACT hands the library a 100 Hz DLS2 filter (0x550B94: 4096 log2(2 sin(pi Hz /
// 48000)) = -25623), and the library writes it to the voice as it is (0x4CAAD4, FCA again when the voice is set up,
// 0x4CAF7D). The console plays that music clear, so its VP can't be low-passing stereo voices so.
bool Dls2(const Voice& v) {
    if (!app::Get().voice_filters) return false;
    if (v.flags & 0x200010) return v.fmode == 1;
    return v.channels == 1 && (v.fmode & 1) != 0;
}

// the sinc kernel's taps for a frame t (0..1) past the one playing: a Blackman windowed sinc over the window, low-pass
// at c (1 the output's Nyquist, less when the voice plays faster than 48 kHz), summing to one
void SincTaps(float t, float c, float* k) {
    constexpr double kPi = 3.14159265358979323846;
    double sum = 0, w[kTaps];
    for (int i = 0; i < kTaps; i++) {
        double x = (i - (kHalf - 1)) - double(t), u = x / kHalf;
        double s = std::abs(x) < 1e-9 ? 1.0 : std::sin(kPi * c * x) / (kPi * c * x);
        w[i] = std::abs(u) >= 1 ? 0.0 : s * (0.42 + 0.5 * std::cos(kPi * u) + 0.08 * std::cos(2 * kPi * u));
        sum += w[i];
    }
    for (int i = 0; i < kTaps; i++) k[i] = float(w[i] / sum);
}

// the Audio setting's resampler, how a voice's samples are read between its frames: 0 linear (between this frame and
// the next), 1 cubic (Catmull-Rom over four), 2 sinc (sixteen)
void Pull(Voice& v, float* ab, uint32_t frames) {
    uint32_t ch = std::min<uint32_t>(std::max<uint32_t>(v.channels, 1), kChannels);
    std::memset(ab, 0, frames * ch * sizeof(float));
    if (v.mixin) {
        for (uint32_t i = 0; i < frames && i < v.submix.size(); i++) ab[i * ch] = v.submix[i];
        return;
    }
    uint32_t base = v.data + v.play_start;
    const double rate = Rate(v) / audio::kRate;
    double step = rate * std::exp2(v.mod_pitch / 4096.0);
    const uint32_t mode = std::min<uint32_t>(app::Get().resampler, 2);
    const bool dls2 = Dls2(v);
    // a release that can't play out (the setting is off, or the data ran out) ends now
    auto end_release = [&](const char* why) {
        if (!v.in_release) return;
        TraceReleaseEnd(v, why);
        v.in_release = false;
        if (!v.stream) v.playing = false;
    };
    if (v.in_release && !app::Get().envelopes) end_release("setting off");
    if (v.pos != v.win_seen) v.win_fill = -1;
    static const std::vector<float> kSinc = [] {  // 1024 phases at the output's Nyquist
        std::vector<float> k(1024 * kTaps);
        for (int p = 0; p < 1024; p++) SincTaps(p / 1024.0f, 1.0f, &k[size_t(p) * kTaps]);
        return k;
    }();
    // frame f + j of the data into s; false when it is a stream's not queued yet, to read again later
    uint32_t start = 0, end = 0;
    auto fetch = [&](uint32_t f, uint32_t j, float* s) {
        uint64_t at = uint64_t(f) + j;
        if (v.stream) {
            for (const Packet& pk : v.packets) {
                uint32_t n = FramesOf(v, pk.size);
                if (at < n) {
                    for (uint32_t c = 0; c < ch; c++) s[c] = Sample(v, pk.data, uint32_t(at), int(c));
                    return true;
                }
                at -= n;
            }
            return false;
        }
        if (at >= end) {
            if (!v.looping || end <= start) {
                for (uint32_t c = 0; c < ch; c++) s[c] = 0;
                return true;
            }
            at = start + (at - start) % (end - start);
        }
        for (uint32_t c = 0; c < ch; c++) s[c] = Sample(v, base, uint32_t(at), int(c));
        return true;
    };
    // moves the window to frame f and reads what it lacks; a frame not queued yet holds the one before
    auto slide = [&](uint32_t f) {
        int64_t d = int64_t(f) - v.win_f;
        if (v.win_fill < 0 || d < 0 || d >= kTaps) {
            std::memset(v.win, 0, sizeof v.win);
            v.win_fill = kHalf - 1;
        } else if (d > 0) {
            std::memmove(v.win[0], v.win[d], size_t(kTaps - d) * sizeof v.win[0]);
            v.win_fill = int(std::max<int64_t>(v.win_fill - d, kHalf - 1));
        }
        v.win_f = f;
        for (int i = v.win_fill; i < kTaps; i++) {
            if (fetch(f, uint32_t(i - (kHalf - 1)), v.win[i])) {
                if (v.win_fill == i) v.win_fill++;
            } else if (i > 0) {
                std::memcpy(v.win[i], v.win[i - 1], sizeof v.win[i]);
            }
        }
    };
    float now[kChannels], next[kChannels];  // linear: this frame and the next, interpolated between
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t f = uint32_t(v.pos);
        if (v.stream) {
            while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
                Packet pk = v.packets.front();
                v.packets.pop_front();
                pk.used = pk.size;
                Complete(v, pk, 0);
                v.pos -= FramesOf(v, pk.size);
                v.win_f -= FramesOf(v, pk.size);
                f = uint32_t(v.pos);
                if (v.packets.empty() && !v.ending) v.starved = true;
            }
            if (v.packets.empty()) {
                v.pos = 0;
                v.win_seen = -1;
                end_release("packets ran out");
                if (v.ending) Deactivate(v);
                return;
            }
            if (mode == 0) {
                Packet& pk = v.packets.front();
                bool last = f + 1 >= FramesOf(v, pk.size);
                for (uint32_t c = 0; c < ch; c++) {
                    now[c] = Sample(v, pk.data, f, int(c));
                    next[c] = !last                  ? Sample(v, pk.data, f + 1, int(c))
                              : v.packets.size() > 1 ? Sample(v, v.packets[1].data, 0, int(c))
                                                     : now[c];
                }
            }
        } else {
            start = v.looping ? FramesOf(v, v.loop_start) : 0;
            end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
            if (f >= end) {
                if (!v.looping || end <= start) {
                    v.playing = false;
                    v.pos = 0;
                    v.win_seen = -1;
                    end_release("data ran out");
                    return;
                }
                uint32_t was = f;
                v.pos = start + std::fmod(v.pos - start, double(end - start));
                f = uint32_t(v.pos);
                v.win_f += int64_t(f) - int64_t(was);
            }
            if (!v.data) {
                v.win_seen = -1;
                return;
            }
            if (mode == 0) {
                uint32_t after = f + 1 < end ? f + 1 : v.looping ? start : f;
                for (uint32_t c = 0; c < ch; c++) {
                    now[c] = Sample(v, base, f, int(c));
                    next[c] = Sample(v, base, after, int(c));
                }
            }
        }
        // a new VP frame: the envelopes and LFOs step, and the pitch follows them; a finished release stops the voice
        if (v.tick == 0) {
            StepMods(v);
            step = rate * std::exp2(v.mod_pitch / 4096.0);
            v.tick = kModFrame;
            if (v.in_release && v.ea.cur == kEnvDone) {
                end_release("envelope done");
                v.win_seen = -1;
                if (!v.stream) v.pos = 0;
                return;
            }
        }
        const float amp = v.amp_to + (v.amp_from - v.amp_to) * float(v.tick - 1) / kModFrame;
        v.tick--;
        float t = float(v.pos - f);
        float* o = &ab[i * ch];
        if (mode == 0) {
            for (uint32_t c = 0; c < ch; c++) o[c] = now[c] + (next[c] - now[c]) * t;
        } else if (slide(f); mode == 1) {
            for (uint32_t c = 0; c < ch; c++) {
                float p0 = v.win[kHalf - 2][c], p1 = v.win[kHalf - 1][c], p2 = v.win[kHalf][c], p3 = v.win[kHalf + 1][c];
                o[c] = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
            }
        } else {
            float own[kTaps];
            const float* k = &kSinc[size_t(std::min(int(t * 1024), 1023)) * kTaps];
            if (step > 1.0001) {  // faster than 48 kHz: low-pass below the output's Nyquist
                SincTaps(t, float(1.0 / step), own);
                k = own;
            }
            for (uint32_t c = 0; c < ch; c++) {
                float y = 0;
                for (int n = 0; n < kTaps; n++) y += k[n] * v.win[n][c];
                o[c] = y;
            }
        }
        // the DLS2 low-pass, the VP's state variable filter as xemu runs it: the coefficient 2^(fc / 4096) of the
        // output rate, damping fq / 0x8000, the input scaled to keep the resonance in range
        if (dls2) {
            const float fcf = std::clamp(std::exp2((float(int16_t(v.fc)) + v.mod_fc) / 4096.0f), 0.003906f, 1.0f);
            const float q = std::clamp(v.fq / 32768.0f, 0.079407f, 1.0f), qn = std::sqrt(q / 2 + 0.01f);
            for (uint32_t c = 0; c < ch; c++) {
                float& l = v.svf[c][0];
                float& b = v.svf[c][1];
                b -= b * b * b * 0.001f;
                float h = o[c] * qn - l - q * b;
                b += fcf * h;
                l += fcf * b;
                if (std::abs(l) < 1e-20f) l = 0;
                if (std::abs(b) < 1e-20f) b = 0;
                o[c] = std::clamp(l, -1.0f, 1.0f);
            }
        }
        for (uint32_t c = 0; c < ch; c++) o[c] *= amp;
        v.pos += step;
    }
    v.win_seen = v.pos;
}

// how a voice is routed: volume and headroom in mB, then bin:channel=mB for each channel it takes, before bin
// headroom, and its 3D attenuation and pan
void Describe(uint32_t obj, const Voice& v) {
    char b[160];
    Route r[16];
    int routes = Routes(v, r);
    auto mb = [](float x) { return x > 0 ? int(std::lround(2000 * std::log10(x))) : -10000; };
    std::string bins;
    for (int k = 0; k < routes; k++) {
        float hr = float(1 << g_bin_headroom[r[k].bin]);
        // where it goes unheard: a multipass bin with no mixin buffer behind it, or a bin no speaker or heard effect
        // takes
        uint32_t bn = r[k].bin;
        const char* lost = bn == kMultipass ? (!Submix(v) ? "!nosub" : "")
                           : (dsfx::Loaded() ? bn > 5 || bn == 3 : !Speaker(bn)) && !dsfx::Reads(bn) ? "!unread"
                                                                                                   : "";
        std::snprintf(b, sizeof b, " %u%s:", bn, lost);
        bins += b;
        for (uint32_t c = 0; c < std::min<uint32_t>(v.channels, kChannels); c++) {
            if (r[k].g[c] <= 0) continue;
            std::snprintf(b, sizeof b, "%s%u=%d", bins.back() == ':' ? "" : ",", c, mb(r[k].g[c] * hr));
            bins += b;
        }
    }
    std::string d3;
    if (v.calc3d) {
        const dsound::Voice3dTerms& t = v.t3d;
        std::snprintf(b, sizeof b, " (dist %d cone %d a %d %d b %d %d direct %d room %d filter %d %d)", t.dist, t.cone,
                      t.pair_a[0], t.pair_a[1], t.pair_b[0], t.pair_b[1], t.direct, t.room, t.filter[0], t.filter[1]);
        d3 = b;
    } else if (v.is3d) {
        std::snprintf(b, sizeof b, " (at %.1f %.1f %.1f min %.1f max %.1f mode %u curve %zu)", v.p[0], v.p[1], v.p[2],
                      v.min_dist, v.max_dist, v.mode, v.curve.size());
        d3 = b;
    }
    // XACT's DLS2 filter: mode, cutoff (4.12 octaves) and damping as set, and the corner the SVF's coefficient gives at
    // 48 kHz (f = 2 sin(pi Fc / 48000)) with the envelopes' and LFOs' cutoff modulation; "off" when the VP skips it
    std::string flt;
    if (v.fmode) {
        const float f = std::clamp(std::exp2((float(int16_t(v.fc)) + v.mod_fc) / 4096.0f), 0.003906f, 1.0f);
        std::snprintf(b, sizeof b, " filter %u fc %d%+d q %u = %.0f Hz%s", v.fmode, int(int16_t(v.fc)), int(v.mod_fc),
                      v.fq, 48000.0 / 3.14159265 * std::asin(f / 2), Dls2(v) ? "" : " off");
        flt = b;
    }
    LOGR("dsound:  %08X %uch %uHz%s out %08X vol %d headroom %d%s bins%s 3d %s att %d%s pan %.2f level %d%s", obj,
         v.channels, uint32_t(std::lround(Rate(v))), v.mixin ? " mixin" : "", v.out, mb(v.volume), mb(v.headroom),
         v.nbins ? "" : " default", bins.c_str(), v.calc3d ? "calc" : v.is3d ? "ds" : "no",
         v.calc3d ? mb(Att3d(v.t3d, 0)) : 0, d3.c_str(),
         v.pan3d, mb(float(std::sqrt(v.recent))), flt.c_str());
}

void Render(float* out, uint32_t frames) {
    static std::vector<float> bus, ab, filtered[2];
    static std::vector<Voice*> order;
    const uint32_t speakers = audio::Channels();
    std::memset(out, 0, size_t(frames) * speakers * sizeof(float));
    if (Held()) return;  // a host menu is up: nothing plays on, nothing is used up
    bus.assign(size_t(kBins) * frames, 0.0f);
    ab.resize(frames * kChannels);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_mixed = NowMs();
    g_stalled = d3d::MsSincePresent() > kStallMs;
    // a submix plays once every voice routed to it has mixed in, so the deepest go first and the mixins last
    order.clear();
    for (auto& [obj, v] : g_voices) {
        order.push_back(&v);
        if (v.mixin) v.submix.assign(frames, 0.0f);
    }
    std::stable_sort(order.begin(), order.end(), [](const Voice* a, const Voice* b) {
        if (a->mixin != b->mixin) return b->mixin;
        return Depth(*a) > Depth(*b);
    });
    for (Voice* v : order) {
        if (g_trace_rel && v->in_release && !v->rel_late && g_mixed - v->rel_at > ReleaseMs(*v) + 500) {
            v->rel_late = true;
            LOGR("release: %08X still releasing after %lld ms of %u:%s%s%s%s%s, envelope state %u", v->obj,
                 (long long)(g_mixed - v->rel_at), ReleaseMs(*v), Plays(*v) ? " mixing" : " not mixing",
                 v->paused ? " paused" : "", v->sync ? " sync" : "", v->hold ? " held" : "",
                 v->flushing ? " flushing" : "", unsigned(v->ea.cur));
        }
        if (!Plays(*v)) continue;
        Route r[16];
        int routes = Routes(*v, r);
        Pull(*v, ab.data(), frames);
        Voice* sub = Submix(*v);
        uint32_t ch = std::min<uint32_t>(std::max<uint32_t>(v->channels, 1), kChannels);
        // a 3D voice's I3DL2 filters: the direct path's on every bin but the reverb send, the room's on that
        const float* in[2] = {ab.data(), ab.data()};
        for (int p = 0; p < 2 && v->calc3d && app::Get().distance_filters; p++) {
            if (!v->t3d.filter[p]) continue;
            float a = float(std::min<int32_t>(v->t3d.filter[p], 0xFFFF)) / 65536.0f;
            std::vector<float>& f = filtered[p];
            f.resize(size_t(frames) * ch);
            for (uint32_t i = 0; i < frames; i++)
                for (uint32_t c = 0; c < ch; c++) {
                    float& y = v->lp[p][c];
                    y = ab[i * ch + c] + a * (y - ab[i * ch + c]);
                    if (std::abs(y) < 1e-20f) y = 0;
                    f[i * ch + c] = y;
                }
            in[p] = f.data();
        }
        double block = 0;
        for (int k = 0; k < routes; k++) {
            float* dst = r[k].bin == kMultipass && sub ? sub->submix.data() : &bus[size_t(r[k].bin) * frames];
            const float* src = in[r[k].bin == 10];
            for (uint32_t i = 0; i < frames; i++) {
                float x = 0;
                for (uint32_t c = 0; c < ch; c++) x += src[i * ch + c] * r[k].g[c];
                dst[i] += x;
                block += x * x;
            }
        }
        v->energy += block;
        double w = std::min(1.0, frames / (0.5 * audio::kRate));
        v->recent += (block / frames - v->recent) * w;
    }
    // the GP's effects, a frame at a time
    if (dsfx::Loaded()) {
        static float frame[kBins * dsfx::kFrame];
        for (uint32_t at = 0; at < frames; at += dsfx::kFrame) {
            uint32_t n = std::min(dsfx::kFrame, frames - at);
            for (uint32_t b = 0; b < kBins; b++)
                std::memcpy(&frame[b * dsfx::kFrame], &bus[size_t(b) * frames + at], n * 4);
            dsfx::Process(frame, n);
            for (uint32_t b = 0; b < kBins; b++)
                std::memcpy(&bus[size_t(b) * frames + at], &frame[b * dsfx::kFrame], n * 4);
        }
    }
    // the GP's output: the speaker bins with their headroom back. With an effects image the GP outputs bins 0-5 and
    // the crosstalk inputs reach them through its canceller; without one they are heard as the speakers they stand for
    // (front left and right, back left and right). On 5.1 each bin is its speaker and the sub is heard; on two
    // speakers the console's encoder DSP (the DOLBY image, run with the stereo setting of 0x4CD798) folds the centre
    // at -3 dB into both sides and each back at -3 dB into its own, and drops the sub. The effect sends and a
    // multipass bin nothing reads are not heard. The Audio settings can take the backs at 0 dB and fold the sub at
    // -3 dB into both sides instead.
    static const uint8_t kSpeakerOf[10] = {0, 1, 2, 3, 4, 5, 0, 1, 4, 5};
    const app::Settings& set = app::Get();
    const float back = set.stereo_full_backs ? 1.0f : 0.707f;
    for (uint32_t b = 0; b < kBins; b++) {
        bool sub_heard = b == 3 && (speakers == 6 || set.stereo_sub);
        if (!(Speaker(b) || sub_heard) || (b > 5 && dsfx::Loaded())) continue;
        const float* src = &bus[size_t(b) * frames];
        if (speakers == 6) {
            float g = float(1 << g_bin_headroom[b]);
            const uint32_t o = kSpeakerOf[b];
            for (uint32_t i = 0; i < frames; i++) out[i * 6 + o] += src[i] * g;
            continue;
        }
        const bool both = b == 2 || b == 3;  // the centre and the sub go to both sides
        float g = float(1 << g_bin_headroom[b]) * (both ? 0.707f : kSpeakerOf[b] >= 4 ? back : 1.0f);
        for (uint32_t i = 0; i < frames; i++) {
            if (both || !(b & 1)) out[i * 2] += src[i] * g;
            if (both || (b & 1)) out[i * 2 + 1] += src[i] * g;
        }
    }
    // XBE_AUDIO_RECORD=<file>: the game's mix before the volume setting, raw interleaved float32 at 48 kHz, to compare
    // with a console recording (ffmpeg -f f32le -ar 48000 -ac <speakers> -i <file> out.wav)
    static FILE* record = [] {
        const char* p = std::getenv("XBE_AUDIO_RECORD");
        FILE* f = p && *p ? std::fopen(p, "wb") : nullptr;
        if (f) LOGR("dsound: recording the mix to %s (%u channels)", p, audio::Channels());
        return f;
    }();
    if (record) {
        std::fwrite(out, sizeof(float) * speakers, frames, record);
        std::fflush(record);
    }
    // the volume setting, squared to follow loudness, and silence in the background if asked; a change ramps over the
    // block so it doesn't click
    static float gain = 1.0f;
    const float v = set.volume / 100.0f, want = set.mute_in_background && app::Background() ? 0.0f : v * v;
    // past full scale the mix clips, or with the Audio setting bends from -2.5 dB towards it instead
    const bool soft = set.soft_clip;
    for (uint32_t i = 0; i < frames; i++) {
        float g = gain + (want - gain) * float(i + 1) / float(frames);
        for (uint32_t c = 0; c < speakers; c++) {
            float x = out[i * speakers + c] * g;
            if (soft && std::abs(x) > 0.75f) x = std::copysign(0.75f + 0.25f * std::tanh((std::abs(x) - 0.75f) * 4.0f), x);
            out[i * speakers + c] = std::clamp(x, -1.0f, 1.0f);
        }
    }
    gain = want;
    static uint64_t rendered = 0;
    if (g_trace && (rendered += frames) >= audio::kRate) {
        rendered = 0;
        std::string list;
        for (auto& [obj, v] : g_voices) {
            double rms = std::sqrt(v.energy / (2.0 * audio::kRate));
            v.energy = 0;
            if (rms < 1e-4) continue;
            char b[96];
            std::snprintf(b, sizeof b, " %08X%s%s(%.3f)", obj, v.stream ? "s" : "", v.looping ? "L" : "", rms);
            list += b;
            Describe(obj, v);
        }
        LOGR("dsound: audible%s", list.c_str());
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// an async FlushEx stops the voice at once and flushes later, so the packets the title still submits in between go
// with it. Their callbacks join the queue the next DirectSoundDoWork runs.
void FinishFlushes() {
    for (auto& [obj, v] : g_voices) {
        if (!v.flushing || v.in_release) continue;
        DSTRACE("dsound: %08X flush done, %zu packets", obj, v.packets.size());
        if (g_trace_rel && v.releasing) LOGR("release: %08X stream flush done, ENVELOPECOMPLETE reported", obj);
        for (auto& pk : v.packets) Complete(v, pk, kFlushed);
        v.packets.clear();
        v.pos = 0;
        v.flushing = false;
        v.released = v.releasing;
        v.releasing = false;
        Deactivate(v);
    }
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) {
        CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
        g_ran = NowMs();
    }
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput([](float* out, uint32_t frames) {
        Render(out, frames);
        audio::MixCues(out, frames);  // after Render, which returns early while a host menu is up
    });
    // the APU's free running 48 kHz sample counter (0xFE80200C): XACT times fades, stops and transitions with it, so
    // with the scratch register left at 0 its clock never moved and timed events never ended
    std::thread([] {
        auto* counter = static_cast<volatile uint32_t*>(HostPtr(0xFE80200C));
        for (;;) {
            *counter = uint32_t(uint64_t(ClockNs() / 1000) * audio::kRate / 1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                FinishFlushes();
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            // as the console's DSOUND DPC: never inside the title's own XACT calls, which lock by raising the IRQL
            DispatchLevel at;
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    DSTRACE("dsound: %08X format tag %X %u ch %u Hz align %u bits %u", v.obj, v.tag, v.channels, v.rate, v.align, v.bits);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// a voice with a mixin output buffer sends each of its channels to the buffer's bin after the bins it was given,
// which lose their last slots if the eight do not hold them all (the library's 0x4C3C71)
void AddSubmixBin(Voice& v) {
    uint32_t have = uint32_t(std::count(v.bin, v.bin + v.nbins, kMultipass));
    uint32_t want = std::min<uint32_t>(std::max<uint32_t>(v.channels, 1), 8);
    if (have >= want) return;
    uint32_t add = want - have;
    if (v.nbins + add > 8) v.nbins = uint8_t(8 - add);
    while (add--) v.bin[v.nbins++] = uint8_t(kMultipass);
}

// DSMIXBINS: the voice's bins and their volumes, and its mixin buffer's bin as the library adds it (0x4C54EF)
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: the speakers, as Routes plays them
    if (mixbins) {
        uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
        for (uint32_t i = 0; i < n; i++) {
            v.bin[i] = R32(pairs + i * 8) & 31;
            v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
        }
        v.nbins = uint8_t(n);
    }
    if (!Submix(v)) return;
    if (!v.nbins) {
        static const uint8_t kSpeakers[6] = {0, 1, 2, 3, 4, 5}, kQuad[4] = {0, 1, 4, 5};
        uint32_t ch = std::max<uint32_t>(v.channels, 1);
        v.nbins = uint8_t(ch == 4 ? 4 : std::clamp<uint32_t>(ch, 2, 6));
        std::memcpy(v.bin, ch == 4 ? kQuad : kSpeakers, v.nbins);
    }
    AddSubmixBin(v);
}

// SetOutputBuffer as the library has it (0x4C5559): the voice leaves its old mixin buffer's bin, and a new one takes
// all its bins, at 1 / channels; the title gives it its other bins again with SetMixBins
void SetOut(Voice& v, uint32_t out) {
    if (Submix(v)) v.nbins = uint8_t(std::remove(v.bin, v.bin + v.nbins, kMultipass) - v.bin);
    v.out = out;
    if (!Submix(v)) return;
    v.nbins = 0;
    AddSubmixBin(v);
    v.bin_mb[kMultipass] = int32_t(std::lround(2000 * std::log10(1.0 / std::max<uint32_t>(v.channels, 1))));
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.born = NowMs();
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);  // DSBUFFERDESC: size, flags, bytes, format, mix bins
    // a descriptor whose dwSize is not a DSBUFFERDESC, or a bogus byte count, would ask mem::AllocContiguous
    // for a multi-GB block; only trust a byte count from a well-formed descriptor
    if (R32(desc) != 0x18 || bytes > (16u << 20)) bytes = 0;
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    DSTRACE("dsound: %08X new buffer flags %X bytes %X", obj, flags, bytes);
    v.flags = flags;
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.mixin = flags & 0x2000;
    v.headroom = DefaultHeadroom(flags);
    if (bytes) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.play_len = v.loop_len = bytes;
    }
    W32(out, obj);
    return 0;
}

// DSSTREAMDESC: flags, max packets, format, callback, context, mix bins. The object carries the XDK's own
// vtables: the title dispatches IDirectSoundStream and XMediaObject methods through them.
uint32_t NewStream(uint32_t desc, uint32_t out) {
    XDK_REQUIRE("dsound", XDK_KEY(CDirectSoundStream_Vtable), XDK_KEY(CDirectSoundStream_Vtable2),
                XDK_DATA(DSound_StreamVtable), XDK_DATA(DSound_StreamVtable2));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj + XDK_CDirectSoundStream_Vtable, kStreamVtable);
    W32(obj + XDK_CDirectSoundStream_Vtable2, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.born = NowMs();
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    DSTRACE("dsound: %08X new stream flags %X packets %u callback %08X context %08X", obj, flags, v.max_packets, v.callback,
            v.context);
    v.flags = flags;
    v.is3d = flags & 0x10;  // DSSTREAMCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    W32(out, obj);
    return 0;
}

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

void dsound::SetVoice3d(uint32_t obj, uint32_t changed, const Voice3dTerms& t) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(obj);
    if (!v) return;
    v->calc3d = true;
    Voice3dTerms& h = v->t3d;
    if (changed & k3dDistance) h.dist = t.dist;
    if (changed & k3dCone) h.cone = t.cone;
    if (changed & k3dPairA) std::copy(t.pair_a, t.pair_a + 2, h.pair_a);
    if (changed & k3dPairB) std::copy(t.pair_b, t.pair_b + 2, h.pair_b);
    if (changed & k3dPitch) h.pitch = t.pitch;
    if (changed & k3dI3dl2) {
        h.direct = t.direct;
        h.room = t.room;
    }
    if (changed & k3dAzimuth) {
        h.azimuth = t.azimuth;
        v->pan3d = std::sin(t.azimuth * 3.14159265f / 180.0f);
    }
    if (changed & k3dFilter) std::copy(t.filter, t.filter + 2, h.filter);
}

void dsound::LogState() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    int64_t now = NowMs();
    auto ago = [&](int64_t t) { return t ? double(now - t) / 1000 : -1.0; };
    size_t streams = 0, playing = 0;
    for (auto& [obj, v] : g_voices) {
        streams += v.stream;
        playing += v.stream ? v.active : v.playing;
    }
    LOGE("dsound: %zu voices (%zu streams), %zu playing; mixed %.2fs ago, DoWork %.2fs ago, callback %.2fs ago, %zu "
         "callbacks queued", g_voices.size(), streams, playing, ago(g_mixed), ago(g_pumped), ago(g_ran), g_done.size());
    const dsfx::I3dl2& l = g_i3dl2;
    LOGE("dsound: I3DL2 room %d hf %d decay %.2f ratio %.2f reflections %d at %.3f reverb %d at %.3f diffusion %.0f "
         "density %.0f", l.room, l.room_hf, l.decay, l.decay_hf_ratio, l.reflections, l.reflections_delay, l.reverb,
         l.reverb_delay, l.diffusion, l.density);
    // seconds since: made, last packet in, last packet out (-1 never)
    for (auto& [obj, v] : g_voices) {
        if (!v.stream) {
            if (v.playing)
                LOGE("dsound:  %08X buf%s%s pos %u/%u refs %d", obj, v.looping ? " loop" : "", v.sync ? " sync" : "",
                     uint32_t(v.pos), FramesOf(v, v.play_len), v.refs);
            continue;
        }
        LOGE("dsound:  %08X str %u/%u pk%s%s%s%s%s%s%s%s%s refs %d cb %08X made %.1f in %.1f out %.1f", obj,
             uint32_t(v.packets.size()), v.max_packets, v.active ? " active" : "", v.paused ? " paused" : "",
             v.sync ? " sync" : "", v.hold ? " hold" : "", v.starved ? " starved" : "", v.ending ? " ending" : "",
             v.flushing ? " flushing" : "", v.releasing ? " releasing" : "", v.released ? " released" : "", v.refs,
             v.callback, ago(v.born), ago(v.fed), ago(v.done));
    }
    // and how each one that plays is heard, its level the last half second as mixed, in mB
    for (auto& [obj, v] : g_voices)
        if (Plays(v)) Describe(obj, v);
}

// device

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig),
        SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones), SYM(IDirectSound_SetMixBinHeadroom),
        SYM(IDirectSound_CommitDeferredSettings), SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor),
        SYM(IDirectSound_SetDistanceFactor), SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener),
        SYM(IDirectSound_SetPosition), SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion), SYM(IDirectSoundBuffer_SetAllParameters),
        SYM(IDirectSoundBuffer_SetFormat), SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx),
        SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer), SYM(IDirectSound_CreateSoundBuffer),
        SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock), SYM(IDirectSoundBuffer_SetBufferData),
        SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop), SYM(IDirectSoundBuffer_SetLoopRegion),
        SYM(IDirectSoundBuffer_GetStatus), SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom), SYM(IDirectSoundBuffer_SetMixBinVolumes),
        SYM(IDirectSoundBuffer_SetMixBins), SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity), SYM(IDirectSoundBuffer_SetI3DL2Source),
        SYM(DirectSoundCreateStream), SYM(CDirectSoundStream_AddRef), SYM(CDirectSoundStream_SetFormat),
        SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo), SYM(CDirectSoundStream_GetStatus),
        SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity), SYM(CDirectSoundStream_Flush),
        SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause), SYM(CDirectSoundStream_SetVolume),
        SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter), SYM(CDirectSoundStream_SetHeadroom),
        SYM(CDirectSoundStream_SetMixBinVolumes), SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition), SYM(CDirectSoundStream_SetVelocity),
        SYM(CDirectSoundStream_SetI3DL2Source), SYM(IDirectSoundStream_SetEG), SYM(IDirectSoundStream_SetOutputBuffer),
        SYM(IDirectSoundStream_SetPitch), SYM(IDirectSoundStream_SetLFO), SYM(IDirectSoundBuffer_SetPitch),
        SYM(IDirectSoundBuffer_SetLFO), SYM(IDirectSoundBuffer_SetEG), SYM(IDirectSoundBuffer_GetVoiceProperties),
        SYM(IDirectSoundStream_GetVoiceProperties), SYM(IDirectSound_CreateSoundStream)};
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0, hi = 0;
    for (auto& s : g_image->sections)
        if (SYM(DirectSoundCreate) - s.va < s.vsize) lo = s.va, hi = s.va + s.vsize;
    for (auto& s : g_image->sections)
        for (uint32_t o = 0; s.name == ".XTLID" && o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o), va = g_image->U32(s.raw + o + 4);
            auto done = [&](uint32_t f) { return std::find(std::begin(handled), std::end(handled), f) != std::end(handled); };
            // a jmp to a host version is covered by it
            if (id >> 16 != 0xB || va < lo || va >= hi || !dispatch::Find(va) || done(va) ||  // 0xB: DSOUND's library id
                (G<uint8_t>(va) == 0xE9 && done(va + 5 + R32(va + 1))))
                continue;
            char b[24];
            std::snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(DirectSoundUseLightHRTF, 0) { return 0; }
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], audio::ConsoleFlags());
    return 0;
}
// (this, effect, byte offset, data, bytes, apply): into the effect's state, as the GP reads it
HLE_STDCALL(IDirectSound_SetEffectData, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    DSTRACE("dsound: SetEffectData effect %u offset %X data %08X size %X flags %X", a[1], a[2], a[3], a[4], a[5]);
    std::vector<uint32_t> words(a[4] / 4);
    for (uint32_t i = 0; i < words.size(); i++) words[i] = R32(a[3] + i * 4);
    dsfx::SetData(a[1], a[2], words.data(), uint32_t(words.size()));
    return 0;
}
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
// (this, bin, headroom bits)
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    DSTRACE("dsound: SetMixBinHeadroom bin %u headroom %u", a[1], a[2]);
    if (a[1] < kBins) g_bin_headroom[a[1]] = uint8_t(a[2] & 7);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
// (this, DSI3DL2LISTENER, apply): the reverb's room
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    if (!a[1]) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto i = [&](uint32_t o) { return int32_t(R32(a[1] + o)); };
    auto f = [&](uint32_t o) { return F32(R32(a[1] + o)); };
    g_i3dl2 = {i(0), i(4), f(8), f(12), f(16), i(20), f(24), i(28), f(32), f(36), f(40), f(44)};
    dsfx::SetI3dl2(g_i3dl2);
    DSTRACE("dsound: I3DL2 room %d hf %d decay %.2f ratio %.2f reflections %d at %.3f reverb %d at %.3f", i(0), i(4),
            f(12), f(16), i(20), f(24), i(28), f(32));
    return 0;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_front[i] = F32(a[1 + i]);
        g_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    TraceStall(ctx, "DoWork");
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        FinishFlushes();
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) v.sync = false;
    return 0;
}

// Effects run on the console's DSP and are not reproduced.
// the descriptor keeps the image's effect list so the title finds its indices, every segment points at zeroed memory
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name) {
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    dsfx::Load(img, static_cast<uint32_t*>(HostPtr(kGpXmem)));
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4), seg;
            if (k == 1 && i < dsfx::Count()) {
                // the state, in the GP's X memory as the library points the title at it: the title reads its
                // effects' routing there and changes their parameters in place
                seg = kGpXmem + dsfx::StateOf(i) * 4;
            } else {
                seg = mem::AllocSystem(std::max(size, 0x40u));
                std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            }
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str());
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    std::vector<uint8_t> img(static_cast<uint8_t*>(HostPtr(a[1])), static_cast<uint8_t*>(HostPtr(a[1])) + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory");
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, byte offset, data, bytes): the effect's state
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    if (!a[3]) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::vector<uint32_t> words(a[4] / 4);
    dsfx::GetData(a[1], a[2], words.data(), uint32_t(words.size()));
    for (uint32_t i = 0; i < words.size(); i++) W32(a[3] + i * 4, words[i]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

// DSENVELOPEDESC (EG, mode, delay, attack, hold, decay, release, sustain, pitch scale, cutoff scale) as 0x4CA942 keeps
// it: EG 1 the amplitude envelope, 0 the multi one, which alone has the scales. A playing voice's state carries on.
void SetEnvelope(Voice& v, uint32_t desc) {
    if (!desc) return;
    const uint32_t eg = R32(desc);
    if (eg > 1) return;
    Envelope& e = eg ? v.ea : v.ef;
    e.start = uint8_t(R32(desc + 4) & 0xF);
    e.delay = uint16_t(R32(desc + 8) & 0xFFF);
    e.attack = uint16_t(R32(desc + 0xC) & 0xFFF);
    e.hold = uint16_t(R32(desc + 0x10) & 0xFFF);
    e.decay = uint16_t(R32(desc + 0x14) & 0xFFF);
    e.release = uint16_t(R32(desc + 0x18) & 0xFFF);
    e.sustain = uint8_t(R32(desc + 0x1C));
    if (!eg) {
        e.pitch = int8_t(R32(desc + 0x20));
        e.cutoff = int8_t(R32(desc + 0x24));
    }
}
// DSLFODESC (LFO, delay, delta, pitch, cutoff, amplitude) as 0x4CA813 keeps it: LFO 0 the multi one (pitch, cutoff
// and amplitude), 1 the pitch one. How the VP turns delta into a rate and the depths into pitch and level isn't
// known; Lfo and StepMods hold the guess.
void SetLfo(Voice& v, uint32_t desc) {
    if (!desc) return;
    const uint32_t k = R32(desc);
    if (k > 1) return;
    Lfo& l = v.lfo[k];
    l.delay = R32(desc + 4) & 0x7FFF;
    l.delta = R32(desc + 8) & 0x3FF;
    l.pitch = int8_t(R32(desc + 0xC));
    if (k == 0) {
        l.cutoff = int8_t(R32(desc + 0x10));
        l.amp = int8_t(R32(desc + 0x14));
    }
}
// DSFILTERDESC (mode, Q, coefficients) as 0x4CAAD4 keeps it: DLS2 takes its cutoff and damping from the first two
void SetFilter(Voice& v, uint32_t desc) {
    if (!desc) return;
    v.fmode = R32(desc) & 3;
    v.fc = int32_t(int16_t(R32(desc + 8) & 0xFFFF));
    v.fq = R32(desc + 0xC) & 0xFFFF;
}

// XBE_TRACE_DSOUND: what XACT asks of a voice's filter, envelope, LFO and pitch; logged when a voice's descriptor
// changes
static void TraceDesc(uint32_t obj, const char* name, uint32_t desc, uint32_t words) {
    if (!g_trace) return;
    static std::map<std::pair<uint32_t, std::string>, std::vector<uint32_t>> last;
    std::vector<uint32_t> w;
    for (uint32_t i = 0; i < words; i++) w.push_back(desc ? R32(desc + i * 4) : 0);
    auto& l = last[{obj, name}];
    if (l == w) return;
    l = w;
    std::string t;
    char b[16];
    for (uint32_t x : w) {
        std::snprintf(b, sizeof b, " %X", x);
        t += b;
    }
    LOGR("dsound: %08X %s%s", obj, name, t.c_str());
}

// XBE_TRACE_DSOUND: the calls that start, stop or reroute a voice
template <class A>
static void Trace(const char* kind, const char* name, int nargs, const A& a) {
    static const char* const kQuiet[] = {"GetStatus", "Process", "GetCurrentPosition", "GetInfo", "SetPosition",
                                         "SetMixBinVolumes", "SetVolume", "SetFrequency", "SetFilter", "SetHeadroom"};
    static const bool all = g_trace && !std::strcmp(std::getenv("XBE_TRACE_DSOUND"), "all");
    if (!g_trace) return;
    for (const char* q : kQuiet)
        if (!std::strcmp(q, name) && (!all || !std::strcmp(q, "GetStatus") || !std::strcmp(q, "Process"))) return;
    LOGR("dsound: %08X %s %s %X %X %X", a[0], kind, name, nargs > 1 ? a[1] : 0, nargs > 2 ? a[2] : 0, nargs > 3 ? a[3] : 0);
}

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        TraceStall(ctx, "buffer " #name);                   \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        Trace("buf", #name, nargs, a);                      \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(Unlock, 5) { return 0; }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // the library frees its own memory, and makes it again for a size with no data
    if (v.owned && v.owned != a[1]) {
        mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);
    v.bytes = v.play_len = v.loop_len = a[2];
    v.play_start = v.loop_start = 0;
    v.pos = 0;
    v.block = -1;
    return 0;
}
BUFFER(Play, 4) {
    v.stop_polls = 0;
    if (!v.playing || v.in_release) VoiceOn(v);
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    return 0;
}
BUFFER(Stop, 1) {
    TraceReleaseEnd(v, "Stop");
    v.playing = v.in_release = false;
    return 0;
}
// (this, time low, time high, flags): DSBSTOPEX_ENVELOPE (1) plays the release envelope out first,
// DSBSTOPEX_RELEASEWAVEFORM (2) leaves the loop and plays on to the end; neither stops it now. The time is not kept.
BUFFER(StopEx, 4) {
    v.stop_polls = 1;
    if (!v.playing) return 0;
    const bool tail = (a[3] & 2) && app::Get().envelopes && v.looping;
    if (tail) v.looping = false;
    if (!((a[3] & 1) && StartRelease(v)) && !tail) v.playing = false;
    return 0;
}
// (this, start, length) in bytes, 0 length plays to the end. The loop becomes the whole region.
BUFFER(SetPlayRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.play_start = a[1];
    v.play_len = v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    v.loop_start = 0;
    v.pos = 0;
    v.block = -1;
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.play_len || a[2] > v.play_len - a[1]) return 0x88780032u;
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.play_len - a[1];
    return 0;
}
BUFFER(GetStatus, 2) {
    if (v.in_release) v.rel_polls++;
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));  // DSBSTATUS_PLAYING, DSBSTATUS_LOOPING
    TraceStopPoll(v, R32(a[1]));
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.play_len ? (p + BytesOf(v, 64 * 4)) % v.play_len : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
BUFFER(SetFrequency, 2) {
    SetFrequency(v, a[1]);
    return 0;
}
BUFFER(SetFilter, 2) {
    TraceDesc(v.obj, "SetFilter", a[1], 6);  // DSFILTERDESC: mode, Q, four coefficients
    SetFilter(v, a[1]);
    return 0;
}
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
BUFFER(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
BUFFER(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
BUFFER(SetI3DL2Source, 3) { return 0; }
// (this, DS3DBUFFER, apply): position at +4, distances at +0x34 and +0x38, mode at +0x3C
BUFFER(SetAllParameters, 3) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(R32(a[1] + 4 + i * 4));
    v.min_dist = F32(R32(a[1] + 0x34));
    v.max_dist = F32(R32(a[1] + 0x38));
    v.mode = R32(a[1] + 0x3C);
    return 0;
}
// (this, points, count, apply)
BUFFER(SetRolloffCurve, 4) {
    v.curve.clear();
    for (uint32_t i = 0; a[1] && i < std::min<uint32_t>(a[2], 64); i++) v.curve.push_back(F32(R32(a[1] + i * 4)));
    return 0;
}
// (this, mixin buffer or none): the buffer the voice's multipass sends feed
BUFFER(SetOutputBuffer, 2) {
    SetOut(v, a[1]);
    return 0;
}
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.has_pitch = false;
    v.block = -1;
    return 0;
}

// streams, the object carries the XDK's own vtable

HLE_STDCALL(DirectSoundCreateStream, 2) {
    const uint32_t r = NewStream(a[0], a[1]);
    if (a[1]) TraceStreamCall(ctx, R32(a[1]), "made");
    return r;
}
// (this, pdsd, ppStream, pUnkOuter): the title's own stream factory. Ordinal 0x3B of the DSOUND library;
// the signature scan named the sibling (this, pdsb, ppBuffer, pUnkOuter) thunk CreateSoundBuffer, so this one
// is CreateSoundStream. Leaving it to the library built a console object; the runtime builds a host one.
HLE_STDCALL(IDirectSound_CreateSoundStream, 4) {
    const uint32_t r = NewStream(a[1], a[2]);
    if (a[2]) TraceStreamCall(ctx, R32(a[2]), "made");
    return r;
}

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
        TraceStall(ctx, "stream " #name);                       \
        if (!std::strcmp(#name, "FlushEx") || !std::strcmp(#name, "Pause") || !std::strcmp(#name, "Release")) \
            TraceStreamCall(ctx, a[0], #name);                  \
        uint32_t r;                                             \
        std::vector<Done> flushed;                              \
        {                                                       \
            std::lock_guard<std::recursive_mutex> lock(g_lock); \
            Voice* v = Find(a[0]);                              \
            Trace("str", #name, nargs, a);                      \
            r = v ? str_##name(*v, a) : 0x80004005u;            \
            flushed.swap(t_flushed);                            \
        }                                                       \
        Run(ctx, flushed);                                      \
        return r;                                               \
    }                                                           \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
static void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    TraceReleaseEnd(v, "Flush");
    v.flushing = v.releasing = v.released = v.in_release = false;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.has_pitch = false;  // the XDK sets the pitch back to the new format's own rate (0x4C5B95)
    v.block = -1;
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
// DSSTREAMSTATUS: READY (room for a packet), then for a started stream PLAYING, or PAUSED, with STARVED when it played
// out its packets. A stream started before its first packet plays (XACT starts a prepared sound, then feeds it, and
// takes "not playing" as finished). A stream releasing after an async flush still plays until the flush, and reports
// ENVELOPECOMPLETE once it is done (the library's 0x4C8943 reads its voice flag 0x2000).
STREAM(GetStatus, 2) {
    if (v.in_release || v.releasing) v.rel_polls++;
    uint32_t st = v.packets.size() < v.max_packets ? 1u : 0u;
    if (v.active && v.starved && !v.flushing) st |= 0x60000;
    else if (v.active) st |= v.paused || v.sync ? 0x20000u : 0x10000u;
    if (v.released) st |= 0x80000;
    W32(a[1], st);
    TraceStopPoll(v, st);
    static const char* watch_env = std::getenv("XBE_TRACE_VOICE");
    static const uint32_t watch = watch_env ? uint32_t(std::strtoul(watch_env, nullptr, 16)) : 0;
    if (watch == v.obj) LOGR("dsound: %08X GetStatus -> %X (%zu packets)", v.obj, st, v.packets.size());
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    if (g_trace) LOGR("dsound: %08X packet %08X %X%s", v.obj, pk.data, pk.size, v.active ? "" : " (starts)");
    v.packets.push_back(pk);
    v.fed = NowMs();
    if (!v.active) VoiceOn(v);
    v.active = true;
    v.ending = v.released = v.starved = false;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
// (this, time low, time high, flags): no flags flushes now, otherwise a started stream stops and flushes later
STREAM(FlushEx, 4) {
    v.stop_polls = 1;
    if (!a[3]) {
        FlushStream(v);
    } else if (v.active) {
        v.flushing = true;
        v.releasing = a[3] & 4;  // DSSTREAMFLUSHEX_ENVELOPE
        if (v.releasing) StartRelease(v);  // the flush waits for it
    }
    return 0;
}
// DSSTREAMPAUSE: 0 resume, 1 pause, 2 synch playback, 3 resume a started stream and hold one that has not started.
// Resuming a held stream starts it, packets or not (the library's 0x4C9426 activates it through 0x4C90A0).
STREAM(Pause, 2) {
    if (a[1] == 3 && !v.active) {
        v.hold = true;
        return 0;
    }
    if (a[1] == 0 && v.hold) {
        if (!v.active) VoiceOn(v);
        v.active = true;
        v.starved = false;
    }
    v.hold = false;
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
STREAM(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency, 2) {
    SetFrequency(v, a[1]);
    return 0;
}
STREAM(SetFilter, 2) {
    TraceDesc(v.obj, "SetFilter", a[1], 6);
    SetFilter(v, a[1]);
    return 0;
}
HLE_STDCALL(IDirectSoundStream_SetEG, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TraceDesc(a[0], "SetEG", a[1], 10);  // DSENVELOPEDESC
    if (Voice* v = Find(a[0])) SetEnvelope(*v, a[1]);
    return 0;
}
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) { return 0; }
// (this, pOutputBuffer): the stream's mixin buffer, as the buffer's. The library body reads a console voice
// sub-object the host's stream object does not hold (it faulted at stream + 0x14 -> +0xb0).
HLE_STDCALL(IDirectSoundStream_SetOutputBuffer, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Trace("str", "SetOutputBuffer", 2, a);
    if (Voice* v = Find(a[0])) SetOut(*v, a[1]);
    return 0;
}
// (this, lPitch / pLfoDesc / pEnvelopeDesc): the library's bodies write a console voice sub-object the host's objects
// never build. The pitch is kept as the library keeps it (0x4C3D2C): XACT sets each voice's rate with it, and varies
// it per sound. The envelopes and LFOs are kept as the library writes them to the VP (SetEnvelope, SetLfo).
// XBE_TRACE_DSOUND: a voice's pitch when it changes
static void TracePitch(uint32_t obj, uint32_t pitch) {
    if (!g_trace) return;
    static std::map<uint32_t, uint32_t> last;
    auto it = last.find(obj);
    if (it != last.end() && it->second == pitch) return;
    last[obj] = pitch;
    LOGR("dsound: %08X SetPitch %d", obj, int32_t(pitch));
}
HLE_STDCALL(IDirectSoundStream_SetPitch, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TracePitch(a[0], a[1]);
    if (Voice* v = Find(a[0])) {
        v->has_pitch = true;
        v->pitch = int32_t(a[1]);
    }
    return 0;
}
HLE_STDCALL(IDirectSoundStream_SetLFO, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TraceDesc(a[0], "SetLFO", a[1], 6);  // DSLFODESC
    if (Voice* v = Find(a[0])) SetLfo(*v, a[1]);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetPitch, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TracePitch(a[0], a[1]);
    if (Voice* v = Find(a[0])) {
        v->has_pitch = true;
        v->pitch = int32_t(a[1]);
    }
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetLFO, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TraceDesc(a[0], "SetLFO", a[1], 6);
    if (Voice* v = Find(a[0])) SetLfo(*v, a[1]);
    return 0;
}
HLE_STDCALL(IDirectSoundBuffer_SetEG, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TraceDesc(a[0], "SetEG", a[1], 10);
    if (Voice* v = Find(a[0])) SetEnvelope(*v, a[1]);
    return 0;
}
// (this, DSVOICEPROPS): no voice is allocated on the host, so every field reads as zero (the struct is
// dwMixBinCount, 8 volume pairs, then 6 LONGs = 92 bytes)
HLE_STDCALL(IDirectSoundBuffer_GetVoiceProperties, 2) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 92);
    return 0;
}
HLE_STDCALL(IDirectSoundStream_GetVoiceProperties, 2) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 92);
    return 0;
}
