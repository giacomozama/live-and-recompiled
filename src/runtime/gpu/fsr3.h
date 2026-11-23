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
// reset: no history. depth_to_view turns depth into the view (AMD's fDeviceToViewDepth), which FSR weighs
// disocclusions by: distance = [1] / (depth - [0]), and across, ndc x and y (y up) times [2] and [3] times distance;
// meters_per_unit its units' size; frame_ms the time since the picture before
struct Evaluate {
    XbeDlssImage color, depth, motion, output;
    float jitter[2] = {};
    float motion_scale[2] = {};
    bool reset = false;
    float depth_to_view[4] = {};
    float meters_per_unit = 1;
    float frame_ms = 16.7f;
};

// depth_to_view for a projection by its planes and vertical field of view (radians), depth 0 at the near plane
// (AMD's setupDeviceDepthToViewSpaceDepthParams)
void DepthToView(float near_plane, float far_plane, float fov_y, float aspect, float out[4]);

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
