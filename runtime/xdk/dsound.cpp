// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>

#include <fstream>

#include "audio/audio.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;

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
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0;
    bool playing = false, looping = false, paused = false;
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1, gain_l = 1, gain_r = 1;
    bool is3d = false;
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    // ADPCM block cache
    int64_t block = -1;
    int16_t decoded[65 * 8];
};

struct Done {
    uint32_t callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
bool g_started = false;
uint32_t g_ds = 0;

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 65;
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 65 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, 65 frames
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
        int64_t at = int64_t(data) + int64_t(f / 65) * 36 * v.channels;
        if (at != v.block) {
            DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded);
            v.block = at;
        }
        return v.decoded[(f % 65) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

void Levels(Voice& v, float& l, float& r) {
    l = v.gain_l * v.volume;
    r = v.gain_r * v.volume;
    if (!v.is3d) return;
    float d[3] = {v.p[0] - g_listener[0], v.p[1] - g_listener[1], v.p[2] - g_listener[2]};
    float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float att = dist <= v.min_dist ? 1.0f : v.min_dist / std::min(dist, v.max_dist);
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    float pan = dist > 1e-3f ? (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / dist : 0.0f;
    l *= att * std::min(1.0f, 1.0f - pan);
    r *= att * std::min(1.0f, 1.0f + pan);
}

void Render(float* out, uint32_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(float));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) {
        if (v.paused || (!v.stream && !v.playing)) continue;
        float l, r;
        Levels(v, l, r);
        double step = double(v.freq ? v.freq : v.rate) / audio::kRate;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f = uint32_t(v.pos);
            float a, b, a1, b1;  // this frame and the next, interpolated between
            if (v.stream) {
                while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
                    Packet pk = v.packets.front();
                    v.packets.pop_front();
                    pk.used = pk.size;
                    Complete(v, pk, 0);
                    v.pos -= FramesOf(v, pk.size);
                    f = uint32_t(v.pos);
                }
                if (v.packets.empty()) {
                    v.pos = 0;
                    break;
                }
                Packet& pk = v.packets.front();
                a = Sample(v, pk.data, f, 0);
                b = Sample(v, pk.data, f, 1);
                if (f + 1 < FramesOf(v, pk.size)) {
                    a1 = Sample(v, pk.data, f + 1, 0);
                    b1 = Sample(v, pk.data, f + 1, 1);
                } else if (v.packets.size() > 1) {
                    a1 = Sample(v, v.packets[1].data, 0, 0);
                    b1 = Sample(v, v.packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t end = v.looping && v.loop_len ? FramesOf(v, v.loop_start + v.loop_len) : FramesOf(v, v.bytes);
                if (f >= end) {
                    if (!v.looping || !end) {
                        v.playing = false;
                        v.pos = 0;
                        break;
                    }
                    uint32_t start = v.loop_len ? FramesOf(v, v.loop_start) : 0;
                    v.pos -= end - start;
                    f = uint32_t(v.pos);
                }
                if (!v.data) break;
                a = Sample(v, v.data, f, 0);
                b = Sample(v, v.data, f, 1);
                uint32_t g = f + 1 < end ? f + 1 : v.looping && end ? (v.loop_len ? FramesOf(v, v.loop_start) : 0) : f;
                a1 = Sample(v, v.data, g, 0);
                b1 = Sample(v, v.data, g, 1);
            }
            float t = float(v.pos - f);
            out[i * 2] += (a + (a1 - a) * t) * l;
            out[i * 2 + 1] += (b + (b1 - b) * t) * r;
            v.pos += step;
        }
    }
    for (uint32_t i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // stream callbacks run on a guest capable thread, like the DPCs they are on the console
    StartHostThread("dsound", [](X86Context& ctx) {
        for (;;) {
            Done d;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait(lock, [] { return !g_done.empty(); });
                d = g_done.front();
                g_done.pop_front();
            }
            CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
        }
    });
}

void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS volumes onto the two host channels
void MixBins(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = R32(mixbins), pairs = R32(mixbins + 4);
    float l = 0, r = 0;
    for (uint32_t i = 0; i < n && i < 16; i++) {
        uint32_t bin = R32(pairs + i * 8);
        float g = Db(int32_t(R32(pairs + i * 8 + 4)));
        if (bin == 0 || bin == 4) l = std::max(l, g);
        if (bin == 1 || bin == 5) r = std::max(r, g);
        if (bin == 2) {
            l = std::max(l, g * 0.707f);
            r = std::max(r, g * 0.707f);
        }
    }
    if (l == 0 && r == 0)
        for (uint32_t i = 0; i < n && i < 16; i++) l = r = std::max(l, Db(int32_t(R32(pairs + i * 8 + 4))));
    v.gain_l = l;
    v.gain_r = r;
}

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
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);
    ReadFormat(v, R32(desc + 12));
    MixBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;
    if (bytes) {
        v.data = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = bytes;
    }
    W32(out, obj);
    return 0;
}

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

// ---------------------------------------------------------------- device

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], 2);  // stereo
    return 0;
}
HLE_STDCALL(IDirectSound_SetEffectData, 6) { return 0; }
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) { return 0; }
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) { return 0; }
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
HLE_STDCALL(DirectSoundDoWork, 0) { return 0; }

// Effects run on the console's DSP and are not reproduced. The descriptor keeps the image's effect list so the
// title finds its indices, every segment points at zeroed memory.
HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        std::ifstream f(ResolvePath(name, 0), std::ios::binary);
        img.assign(std::istreambuf_iterator<char>(f), {});
    }
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = 0x818 + size_t(u32(0x804) + u32(0x80C)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name.c_str(), count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    if (a[3]) W32(a[3], desc);
    return 0;
}

// ---------------------------------------------------------------- buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(Unlock, 5) { return 0; }
BUFFER(SetBufferData, 3) {
    v.data = a[1];
    v.bytes = a[2];
    v.pos = 0;
    v.block = -1;
    return 0;
}
BUFFER(Play, 4) {
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    v.loop_start = a[1];
    v.loop_len = a[2];
    return 0;
}
BUFFER(GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.bytes ? (p + BytesOf(v, 65 * 4)) % v.bytes : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;
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
BUFFER(SetHeadroom, 2) { return 0; }
BUFFER(SetMixBinVolumes, 2) {
    MixBins(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    MixBins(v, a[1]);
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

// ---------------------------------------------------------------- streams, the object carries the XDK's own vtable

HLE_STDCALL(DirectSoundCreateStream, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t desc = a[0];
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj, kStreamVtable);
    W32(obj + 4, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    MixBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;
    W32(a[1], obj);
    return 0;
}

#define STREAM(name, nargs)                                 \
    static uint32_t str_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? str_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

static void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(SetFormat, 2) {
    ReadFormat(v, a[1]);
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
STREAM(GetStatus, 2) {
    W32(a[1], v.packets.size() < v.max_packets ? 1u : 0u);
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    return 0;
}
STREAM(Discontinuity, 1) { return 0; }
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
STREAM(FlushEx, 4) {
    FlushStream(v);
    return 0;
}
STREAM(Pause, 2) {
    v.paused = a[1] == 1;
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
STREAM(SetHeadroom, 2) { return 0; }
STREAM(SetMixBinVolumes, 2) {
    MixBins(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    MixBins(v, a[1]);
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
