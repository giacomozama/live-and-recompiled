// The renderer the settings name
#include "gpu/backend.h"

#include "app/app.h"
#include "core/log.h"

namespace gpu {

#ifdef _WIN32

std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height) {
    const std::string& name = app::Get().renderer;
    if (name == "vulkan") {
        // a failed Init leaves nothing behind once destroyed: the window takes a D3D12 swap chain after
        if (auto vk = CreateVulkanBackend(); vk->Init(width, height)) return vk;
        LOGE("vulkan: can't start, drawing with D3D12");
    } else if (name != "d3d12") {
        LOGW("renderer \"%s\" unknown, drawing with D3D12", name.c_str());
    }
    auto d3d = CreateD3D12Backend();
    if (!d3d->Init(width, height)) LOGE("d3d12: can't start, nothing will be drawn");
    return d3d;
}

#else

// Vulkan is the one renderer here
std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height) {
    auto vk = CreateVulkanBackend();
    if (!vk->Init(width, height)) LOGE("vulkan: can't start, nothing will be drawn");
    return vk;
}

#endif

}  // namespace gpu
