#pragma once
#include <cstdint>

namespace dsound {

// A title that runs the XDK's own 3D calculator (XACT does) hands each voice the result through the library's
// voice-descriptor setter, which writes console voice registers the host objects do not have. The title's hook
// passes it on here: obj is the buffer or stream the runtime returned, the attenuation is the sum of the distance,
// cone and occlusion volumes in hundredths of a dB, and azimuth is in degrees, positive to the listener's right.
void SetVoice3d(uint32_t obj, int32_t attenuation_mb, float azimuth);

}  // namespace dsound
