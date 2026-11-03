#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/app.h"

namespace audio {

constexpr uint32_t kRate = 48000;

// the speakers the game mixes for and the output carries, from the surround setting as the title boots (it reads the
// console's audio setting once): 2, or 6 for 5.1 in WAVE order: front left and right, centre, sub, back left and right
inline uint32_t Channels() {
    static const uint32_t n = app::Get().surround ? 6 : 2;
    return n;
}
// the console's audio setting for Channels, XC_AUDIO_FLAGS and DSSPEAKER_* alike: the low word 0 stereo, 1 mono, 2
// surround, 0x10000 Dolby Digital. The library mixes for 5.1 with surround or Dolby Digital (0x4C56AC, 0x4CD77E) and
// for two speakers otherwise; the title's own sound reads the low word (0x4010E2).
inline uint32_t ConsoleFlags() { return Channels() == 6 ? 0x00010002u : 0u; }

struct Device {
    std::string id, name;  // id is what SelectDevice and the audio_device setting take
};

// the outputs there are; any thread
std::vector<Device> Devices();
// moves the output to the device with this id, the system's default when "" or gone; any thread, before or after
// StartOutput (which starts on app::Get().audio_device)
void SelectDevice(const std::string& id);

// the host's own sounds, played over the game; any thread
enum class Cue { Select, Move };  // a tab or setting changed; the cursor moved
void Play(Cue cue);
// adds the playing ones to the front pair of interleaved Channels() frames; the output's callback calls it after the
// game's mix
void MixCues(float* out, uint32_t frames);

// the platform output pulls interleaved float frames of Channels() channels from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

}  // namespace audio
