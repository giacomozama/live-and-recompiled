#pragma once
#include <cstdint>
#include <vector>

// The GP's effects image on the host. The console runs the title's DSP image on the GP: its effects read the mix
// bins the voices fill, write back into them, and the GP outputs bins 0-5. Each effect is recognised by its code and
// state sizes and replaced by a host version that reads its parameters from the same state words its DSP code reads
// (loaded from the image, changed by SetEffectData), so the routing and levels are the image's own. The reverb is
// the exception: its host version is driven by the I3DL2 listener instead. Callers hold dsound's lock.
namespace dsfx {

constexpr uint32_t kFrame = 32;  // samples per GP frame

// the image as the title hands it to DownloadEffectsImage, its states loaded into xmem, the GP's X memory as the
// title sees it (one dword per word); false when none of its effects are known
bool Load(const std::vector<uint8_t>& image, uint32_t* xmem);
bool Loaded();
uint32_t Count();
uint32_t StateOf(uint32_t effect);  // x: address of the effect's state

// one GP frame: 32 mix bins of n <= kFrame samples, bin b at bins + b * kFrame
void Process(float* bins, uint32_t n);

// IDirectSound_SetEffectData / GetEffectData: count dwords at a byte offset into the effect's state
void SetData(uint32_t effect, uint32_t offset, const uint32_t* data, uint32_t count);
void GetData(uint32_t effect, uint32_t offset, uint32_t* data, uint32_t count);

// DSI3DL2LISTENER
struct I3dl2 {
    int32_t room, room_hf;
    float rolloff, decay, decay_hf_ratio;
    int32_t reflections;
    float reflections_delay;
    int32_t reverb;
    float reverb_delay, diffusion, density, hf_reference;
};
void SetI3dl2(const I3dl2& listener);

}  // namespace dsfx
