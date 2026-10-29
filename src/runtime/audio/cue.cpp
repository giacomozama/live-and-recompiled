// the host's own short sounds (the settings menu's), mixed over the game's output
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "audio/audio.h"

namespace audio {

namespace {

// the sound as it was given: a 44-byte header and 16-bit stereo PCM
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
const unsigned char kWav[] = {
#embed "menu_select.wav"
};
#pragma clang diagnostic pop

constexpr float kGain = 0.5f;
constexpr int kVoices = 4;  // sounds at once; one more takes the oldest's place

// the sound at kRate, mono, played at this speed (a lower one is a lower pitch); empty if the file is not what is expected
std::vector<float> Load(float speed) {
    uint16_t channels, bits;
    uint32_t rate;
    if (sizeof kWav < 44 || std::memcmp(kWav + 36, "data", 4)) return {};
    std::memcpy(&channels, kWav + 22, 2);
    std::memcpy(&rate, kWav + 24, 4);
    std::memcpy(&bits, kWav + 34, 2);
    if (bits != 16 || channels == 0) return {};
    const size_t frames = (sizeof kWav - 44) / (2 * channels);
    auto at = [&](size_t i) {
        float sum = 0;
        for (uint16_t c = 0; c < channels; c++) {
            int16_t v;
            std::memcpy(&v, kWav + 44 + (i * channels + c) * 2, 2);
            sum += v;
        }
        return sum / (32768.0f * channels);
    };
    const double step = double(rate) / kRate * speed;
    std::vector<float> out;
    for (double p = 0; p + 1 < double(frames); p += step) {
        const size_t i = size_t(p);
        const float f = float(p - double(i));
        out.push_back((at(i) * (1 - f) + at(i + 1) * f) * kGain);
    }
    return out;
}

struct Voice {
    const std::vector<float>* sound = nullptr;
    size_t at = 0;
};

std::mutex g_lock;
Voice g_voices[kVoices];
int g_next = 0;

}  // namespace

void Play(Cue cue) {
    static const std::vector<float> select = Load(1.0f), move = Load(0.8f);
    std::lock_guard<std::mutex> lock(g_lock);
    g_voices[g_next] = {cue == Cue::Select ? &select : &move, 0};
    g_next = (g_next + 1) % kVoices;
}

void MixCues(float* out, uint32_t frames) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (Voice& v : g_voices) {
        if (!v.sound) continue;
        const size_t n = std::min<size_t>(frames, v.sound->size() - v.at);
        for (size_t i = 0; i < n; i++) out[i * 2] += (*v.sound)[v.at + i], out[i * 2 + 1] += (*v.sound)[v.at + i];
        v.at += n;
        if (v.at >= v.sound->size()) v.sound = nullptr;
    }
}

}  // namespace audio
