// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "xbe/xbe.h"
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
    uint32_t freq = 0;
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
    float att3d = 1, pan3d = 0;
    double energy = 0;  // XBE_TRACE_DSOUND: sum of squares mixed since the last report
    int64_t fed = 0, done = 0, born = 0;  // F3: steady ms of the last packet in, the last packet out, creation
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

const bool g_trace = std::getenv("XBE_TRACE_DSOUND") != nullptr;
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
        att = v.att3d;
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
            float x = Db(v.bin_mb[b]) * v.volume * v.headroom * att;
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

bool Plays(const Voice& v) {
    return !(v.paused || v.sync || v.hold || v.flushing || (!v.stream && !v.playing));
}

// the voice's next frames, its channels interleaved (at most kChannels); silence once it runs out. A mixin buffer
// reads its submix, one channel, as the VP reads the multipass bin.
void Pull(Voice& v, float* ab, uint32_t frames) {
    uint32_t ch = std::min<uint32_t>(std::max<uint32_t>(v.channels, 1), kChannels);
    std::memset(ab, 0, frames * ch * sizeof(float));
    if (v.mixin) {
        for (uint32_t i = 0; i < frames && i < v.submix.size(); i++) ab[i * ch] = v.submix[i];
        return;
    }
    uint32_t base = v.data + v.play_start;
    double step = double(v.freq ? v.freq : v.rate) / audio::kRate;
    float now[kChannels], next[kChannels];  // this frame and the next, interpolated between
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t f = uint32_t(v.pos);
        if (v.stream) {
            while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
                Packet pk = v.packets.front();
                v.packets.pop_front();
                pk.used = pk.size;
                Complete(v, pk, 0);
                v.pos -= FramesOf(v, pk.size);
                f = uint32_t(v.pos);
                if (v.packets.empty() && !v.ending) v.starved = true;
            }
            if (v.packets.empty()) {
                v.pos = 0;
                if (v.ending) Deactivate(v);
                return;
            }
            Packet& pk = v.packets.front();
            bool last = f + 1 >= FramesOf(v, pk.size);
            for (uint32_t c = 0; c < ch; c++) {
                now[c] = Sample(v, pk.data, f, int(c));
                next[c] = !last                  ? Sample(v, pk.data, f + 1, int(c))
                          : v.packets.size() > 1 ? Sample(v, v.packets[1].data, 0, int(c))
                                                 : now[c];
            }
        } else {
            uint32_t start = v.looping ? FramesOf(v, v.loop_start) : 0;
            uint32_t end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
            if (f >= end) {
                if (!v.looping || end <= start) {
                    v.playing = false;
                    v.pos = 0;
                    return;
                }
                v.pos = start + std::fmod(v.pos - start, double(end - start));
                f = uint32_t(v.pos);
            }
            if (!v.data) return;
            uint32_t after = f + 1 < end ? f + 1 : v.looping ? start : f;
            for (uint32_t c = 0; c < ch; c++) {
                now[c] = Sample(v, base, f, int(c));
                next[c] = Sample(v, base, after, int(c));
            }
        }
        float t = float(v.pos - f);
        for (uint32_t c = 0; c < ch; c++) ab[i * ch + c] = now[c] + (next[c] - now[c]) * t;
        v.pos += step;
    }
}

void Render(float* out, uint32_t frames) {
    static std::vector<float> bus, ab;
    static std::vector<Voice*> order;
    std::memset(out, 0, frames * 2 * sizeof(float));
    bus.assign(size_t(kBins) * frames, 0.0f);
    ab.resize(frames * kChannels);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_mixed = NowMs();
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
        if (!Plays(*v)) continue;
        Route r[16];
        int routes = Routes(*v, r);
        Pull(*v, ab.data(), frames);
        Voice* sub = Submix(*v);
        uint32_t ch = std::min<uint32_t>(std::max<uint32_t>(v->channels, 1), kChannels);
        for (int k = 0; k < routes; k++) {
            float* dst = r[k].bin == kMultipass && sub ? sub->submix.data() : &bus[size_t(r[k].bin) * frames];
            for (uint32_t i = 0; i < frames; i++) {
                float x = 0;
                for (uint32_t c = 0; c < ch; c++) x += ab[i * ch + c] * r[k].g[c];
                dst[i] += x;
                v->energy += x * x;
            }
        }
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
    // the GP's output: the speaker bins with their headroom back, the centre at -3 dB to both sides. With an effects
    // image the GP outputs bins 0-5 and the crosstalk inputs reach them through its canceller; without one they are
    // heard as the sides they stand for. The sub, the effect sends and a multipass bin nothing reads are not heard.
    for (uint32_t b = 0; b < kBins; b++) {
        if (!Speaker(b) || (b > 5 && dsfx::Loaded())) continue;
        float g = float(1 << g_bin_headroom[b]) * (b == 2 ? 0.707f : 1.0f);
        const float* src = &bus[size_t(b) * frames];
        for (uint32_t i = 0; i < frames; i++) {
            if (b == 2 || !(b & 1)) out[i * 2] += src[i] * g;
            if (b == 2 || (b & 1)) out[i * 2 + 1] += src[i] * g;
        }
    }
    for (uint32_t i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
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
            // how it is routed: volume and headroom in mB, then bin:channel=mB for each channel it takes, before
            // bin headroom
            Route r[16];
            int routes = Routes(v, r);
            auto mb = [](float x) { return x > 0 ? int(std::lround(2000 * std::log10(x))) : -10000; };
            std::string bins;
            for (int k = 0; k < routes; k++) {
                float hr = float(1 << g_bin_headroom[r[k].bin]);
                std::snprintf(b, sizeof b, " %u:", r[k].bin);
                bins += b;
                for (uint32_t c = 0; c < std::min<uint32_t>(v.channels, kChannels); c++) {
                    if (r[k].g[c] <= 0) continue;
                    std::snprintf(b, sizeof b, "%s%u=%d", bins.back() == ':' ? "" : ",", c, mb(r[k].g[c] * hr));
                    bins += b;
                }
            }
            LOGR("dsound:  %08X %uch %uHz%s out %08X vol %d headroom %d%s bins%s 3d %s att %d pan %.2f", obj,
                 v.channels, v.freq ? v.freq : v.rate, v.mixin ? " mixin" : "", v.out, mb(v.volume), mb(v.headroom),
                 v.nbins ? "" : " default", bins.c_str(), v.calc3d ? "calc" : v.is3d ? "ds" : "no", mb(v.att3d),
                 v.pan3d);
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
        if (!v.flushing) continue;
        DSTRACE("dsound: %08X flush done, %zu packets", obj, v.packets.size());
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
    audio::StartOutput(Render);
    // the APU's free running 48 kHz sample counter (0xFE80200C): XACT times fades, stops and transitions with it, so
    // with the scratch register left at 0 its clock never moved and timed events never ended
    std::thread([] {
        auto* counter = static_cast<volatile uint32_t*>(HostPtr(0xFE80200C));
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            *counter = uint32_t(uint64_t(us) * audio::kRate / 1000000);
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

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
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

void dsound::SetVoice3d(uint32_t obj, int32_t attenuation_mb, float azimuth) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(obj);
    if (!v) return;
    v->calc3d = true;
    v->att3d = Db(attenuation_mb);
    v->pan3d = std::sin(azimuth * 3.14159265f / 180.0f);
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
    W32(a[1], 0);  // DSSPEAKER_STEREO
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
    dsfx::SetI3dl2({i(0), i(4), f(8), f(12), f(16), i(20), f(24), i(28), f(32), f(36), f(40), f(44)});
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
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    return 0;
}
// (this, time low, time high, flags): stops now, the release envelope is not reproduced
BUFFER(StopEx, 4) {
    v.playing = false;
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
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));  // DSBSTATUS_PLAYING, DSBSTATUS_LOOPING
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
    v.freq = a[1];
    return 0;
}
BUFFER(SetFilter, 2) { return 0; }
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
// (this, mixin buffer or none): where the voice's multipass sends go; the title routes them with its mix bins
BUFFER(SetOutputBuffer, 2) {
    v.out = a[1];
    return 0;
}
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.freq = 0;
    v.block = -1;
    return 0;
}

// streams, the object carries the XDK's own vtable

HLE_STDCALL(DirectSoundCreateStream, 2) { return NewStream(a[0], a[1]); }
// (this, pdsd, ppStream, pUnkOuter): the title's own stream factory. Ordinal 0x3B of the DSOUND library;
// the signature scan named the sibling (this, pdsb, ppBuffer, pUnkOuter) thunk CreateSoundBuffer, so this one
// is CreateSoundStream. Leaving it to the library built a console object; the runtime builds a host one.
HLE_STDCALL(IDirectSound_CreateSoundStream, 4) { return NewStream(a[1], a[2]); }

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
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
    v.flushing = v.releasing = v.released = false;
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
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
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
    uint32_t st = v.packets.size() < v.max_packets ? 1u : 0u;
    if (v.active && v.starved && !v.flushing) st |= 0x60000;
    else if (v.active) st |= v.paused || v.sync ? 0x20000u : 0x10000u;
    if (v.released) st |= 0x80000;
    W32(a[1], st);
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
    if (!a[3]) {
        FlushStream(v);
    } else if (v.active) {
        v.flushing = true;
        v.releasing = a[3] & 4;  // DSSTREAMFLUSHEX_ENVELOPE
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
    v.freq = a[1];
    return 0;
}
STREAM(SetFilter, 2) { return 0; }
// the library's own writes voice registers a host stream does not have; envelopes are not played
HLE_STDCALL(IDirectSoundStream_SetEG, 2) { return 0; }
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
    if (Voice* v = Find(a[0])) v->out = a[1];
    return 0;
}
// (this, lPitch / pLfoDesc / pEnvelopeDesc): pitch, LFO and envelope writes all reach a console voice
// sub-object the host's objects never build. The host mixer plays at the format rate (or the SetFrequency
// override) and has no envelope or LFO, so these are dropped the way the XDK's own SetEG already is.
HLE_STDCALL(IDirectSoundStream_SetPitch, 2) { return 0; }
HLE_STDCALL(IDirectSoundStream_SetLFO, 2) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetPitch, 2) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetLFO, 2) { return 0; }
HLE_STDCALL(IDirectSoundBuffer_SetEG, 2) { return 0; }
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
