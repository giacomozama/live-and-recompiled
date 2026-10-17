// The renderer the settings name
#include "gpu/backend.h"

#include "app/app.h"
#include "core/log.h"

namespace gpu {

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
    if (!d3d->Init(width, height)) return nullptr;
    return d3d;
}

}  // namespace gpu
