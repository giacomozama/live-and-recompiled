// The renderer the settings name
#include "gpu/backend.h"
#include "core/diag.h"

#include "app/app.h"
#include "core/log.h"

#include <atomic>
#include <cstdlib>

namespace gpu {

std::vector<std::string>& Adapters() {
    static std::vector<std::string> list;
    return list;
}

static std::atomic<bool> g_hdr_available{false};
bool HdrAvailable() { return g_hdr_available.load(std::memory_order_relaxed); }
void SetHdrAvailable(bool on) { g_hdr_available.store(on, std::memory_order_relaxed); }

std::string RendererName() {
    const char* override_name = diag::Env("XBE_RENDERER");
    return override_name && *override_name ? override_name : app::Get().renderer;
}

// An unknown renderer name selects Vulkan. OpenGL everywhere but Android, Windows can also select D3D11.
std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height, bool& ok) {
    const std::string name = RendererName();
#ifndef __ANDROID__  // no desktop OpenGL on Android: Vulkan draws there
    if (name == "opengl") {
        auto backend = CreateOpenGLBackend();
        ok = backend->Init(width, height);
        if (!ok) LOGE("opengl: cannot start the renderer");
        return backend;
    }
#endif
#ifdef _WIN32
    if (name == "d3d11") {
        auto backend = CreateD3D11Backend();
        ok = backend->Init(width, height);
        if (!ok) LOGE("d3d11: cannot start the renderer");
        return backend;
    }
#endif
    if (name != "vulkan") LOGW("renderer \"%s\" unknown, drawing with Vulkan", name.c_str());
    auto backend = CreateVulkanBackend();
    ok = backend->Init(width, height);
#ifdef _WIN32
    if (!ok) {
        LOGW("vulkan: unavailable; trying D3D11");
        backend.reset();
        backend = CreateD3D11Backend();
        ok = backend->Init(width, height);
        if (!ok) LOGE("d3d11: cannot start the renderer");
        return backend;
    }
#endif
    if (!ok) LOGE("vulkan: can't start, nothing will be drawn");
    return backend;
}

std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height) {
    bool ok;
    return CreateBackend(width, height, ok);
}

void Backend::MemoryReport() { g_mem_report = true; }

}  // namespace gpu
