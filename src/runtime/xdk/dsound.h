#pragma once
#include <cstdint>

namespace dsound {

// A title that runs the XDK's own 3D calculator (XACT does) hands each voice the result through the library's
// voice-descriptor setter, which writes console voice registers the host objects do not have. The title's hook
// passes it on here: obj is the buffer or stream the runtime returned, the distance, cone and I3DL2 direct path
// (occlusion) volumes in hundredths of a dB, which the voice plays at the sum of, and azimuth in degrees, positive
// to the listener's right. changed holds the descriptor's own flags for the terms the call sets; the others keep
// what the voice last had.
enum : uint32_t { k3dDistance = 0x01, k3dCone = 0x02, k3dDirect = 0x10, k3dAzimuth = 0x40 };
void SetVoice3d(uint32_t obj, uint32_t changed, int32_t dist_mb, int32_t cone_mb, int32_t direct_mb, float azimuth);

// F3: every voice's state and how long since the mixer, DirectSoundDoWork and each voice last moved
void LogState();

}  // namespace dsound
