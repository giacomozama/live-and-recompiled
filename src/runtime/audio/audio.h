#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace audio {

constexpr uint32_t kRate = 48000;

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
// adds the playing ones to interleaved stereo frames; the output's callback calls it after the game's mix
void MixCues(float* out, uint32_t frames);

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

}  // namespace audio
