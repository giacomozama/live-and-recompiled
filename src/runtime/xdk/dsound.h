#pragma once
#include <cstdint>

namespace dsound {

// A title that runs the XDK's own 3D calculator (XACT does) hands each voice the result through the library's
// voice-descriptor setter, which writes console voice registers the host objects do not have. The title's hook
// passes it on here: obj is the buffer or stream the runtime returned, the volumes in hundredths of a dB and the
// azimuth in degrees, positive to the listener's right. changed holds the descriptor's own flags for the terms the
// call sets; the others keep what the voice last had.
enum : uint32_t {
    k3dDistance = 0x01,
    k3dCone = 0x02,
    k3dPairA = 0x04,
    k3dPairB = 0x08,
    k3dI3dl2 = 0x10,
    k3dAzimuth = 0x40,
    k3dFilter = 0x80
};
struct Voice3dTerms {
    int32_t dist, cone;  // every bin
    int32_t pair_a[2];   // [0] the 3D fronts and the centre, [1] the 3D backs
    int32_t pair_b[2];   // [0] the 3D fronts, [1] the centre
    int32_t direct;      // I3DL2 direct path (occlusion): the 3D bins and the centre
    int32_t room;        // I3DL2 room: the reverb send
    float azimuth;
    // the I3DL2 one-pole lowpasses, direct path and room (reverb send), as the calculator words them (0x4CC935):
    // the pole times 65536, 0 for none; the low and high halves of the voice's +0x3C (0x4CAAD4)
    int32_t filter[2];
};
void SetVoice3d(uint32_t obj, uint32_t changed, const Voice3dTerms& t);

// F3: every voice's state and how long since the mixer, DirectSoundDoWork and each voice last moved
void LogState();

}  // namespace dsound
