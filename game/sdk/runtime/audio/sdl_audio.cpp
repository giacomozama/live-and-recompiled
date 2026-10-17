// mixer output through SDL's audio, everywhere but Windows (audio/xaudio2.cpp)
#include <SDL3/SDL.h>

#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

// what the device asks for, rendered 10 ms at a time as on XAudio2
void Pull(void* user, SDL_AudioStream* stream, int additional, int) {
    auto& render = *static_cast<std::function<void(float*, uint32_t)>*>(user);
    constexpr uint32_t kFrames = kRate / 100;
    static std::vector<float> buf(kFrames * 2);
    for (int left = additional; left > 0; left -= int(kFrames * 8)) {
        render(buf.data(), kFrames);
        SDL_PutAudioStreamData(stream, buf.data(), int(kFrames * 8));
    }
}

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
        return;
    }
    static std::function<void(float*, uint32_t)> held;  // the device's callback reads it for the run
    held = std::move(render);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Pull, &held);
    if (!stream) {
        LOGE("audio: no output device, no sound: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
    LOGI("audio: SDL %s", SDL_GetCurrentAudioDriver());
}

}  // namespace audio
