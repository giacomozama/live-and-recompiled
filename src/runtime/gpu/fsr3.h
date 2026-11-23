#pragma once
// AMD FSR 3.1's temporal upscaler: its shaders AMD's own (src/runtime/gpu/ffx, FidelityFX SDK 1.1.4, MIT), embedded
// and compiled with glslang the first time it is made, their passes run here the way the SDK's own dispatch and
// Vulkan backend run them (ffx_fsr3upscaler.cpp, ffx_vk.cpp), which only come for Windows. The renderer hands it
// what it hands DLSS (gpu/dlss.h): color, depth and motion at the render size, the jitter, and an output to write.
#include <memory>
#include <stdint.h>

#include "gpu/dlss.h"

struct VmaAllocator_T;

namespace gpu::fsr3 {

// FSR 3.1's quality modes, as XbeDlssQuality's: native AA (1x), Quality (1.5x), Balanced (1.7x), Performance (2x),
// Ultra Performance (3x)
float Ratio(uint32_t quality);
// the render size the quality takes for the output's
void RenderSize(uint32_t out_w, uint32_t out_h, uint32_t quality, uint32_t& w, uint32_t& h);

// a picture to upscale: color, depth and motion at the render size and read only (SHADER_READ_ONLY_OPTIMAL), output
// at the output size (GENERAL, a storage image, RGBA8). jitter: the render pixels the picture's 3D was moved by, x
// right and y down; motion_scale: render pixels per unit of motion (motion is where a pixel was less where it is);
// reset: no history. The camera's near and far planes and vertical field of view (radians) turn depth into distance,
// which FSR weighs disocclusions by; frame_ms the time since the picture before
struct Evaluate {
    XbeDlssImage color, depth, motion, output;
    float jitter[2] = {};
    float motion_scale[2] = {};
    bool reset = false;
    float near_plane = 1, far_plane = 10000, fov_y = 1.0f;
    float frame_ms = 16.7f;
};

class Upscaler {
public:
    // null when the device can't run it (the log says why)
    static std::unique_ptr<Upscaler> Make(VkPhysicalDevice gpu, VkDevice dev, VmaAllocator_T* vma);
    ~Upscaler();
    // its shaders compiled and pipelines made, the first time; false when they can't be (the log says why)
    bool Prepare();
    // its images for these sizes (any made before released first: the device must be idle), cleared in cmd; the
    // passes' pipelines compiled the first time. False: it can't run (the log says why)
    bool Create(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h);
    // a picture upscaled, recorded into cmd outside a render pass; cmd's compute bindings are its own after
    bool Run(VkCommandBuffer cmd, const Evaluate& e);
    // its images released: the device idle
    void Release();

    struct State;

private:
    explicit Upscaler(std::unique_ptr<State> s);
    std::unique_ptr<State> s_;
};

}  // namespace gpu::fsr3
