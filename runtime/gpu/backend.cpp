// The renderer the settings name
#include "gpu/backend.h"

#include "app/app.h"
#include "core/log.h"

namespace gpu {

// A backend is picked by name; each one a platform has gets a line here. Vulkan is the only one so far, and what an
// unknown name gets.
std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height) {
    const std::string& name = app::Get().renderer;
    if (name != "vulkan") LOGW("renderer \"%s\" unknown, drawing with Vulkan", name.c_str());
    auto backend = CreateVulkanBackend();
    if (!backend->Init(width, height)) LOGE("vulkan: can't start, nothing will be drawn");
    return backend;
}

}  // namespace gpu
