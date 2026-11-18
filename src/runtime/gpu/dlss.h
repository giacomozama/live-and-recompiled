#pragma once
// The DLSS module: a library of its own beside the game (libxbe_dlss.so, xbe_dlss.dll, built from src/dlss), the
// only code that links NVIDIA's NGX SDK. The renderer loads it when DLSS is asked for and it is there; its one export,
// xbe_dlss_api, hands over this table. Plain C across the boundary: the module's C++ runtime is not the game's.
#include <stdint.h>
#include <wchar.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBE_DLSS_API_VERSION 2
#define XBE_DLSS_API_EXPORT "xbe_dlss_api"

// DLSS's quality modes, the render size's share of the output's from all to least
enum XbeDlssQuality {
    XBE_DLSS_DLAA = 0,
    XBE_DLSS_QUALITY,
    XBE_DLSS_BALANCED,
    XBE_DLSS_PERFORMANCE,
    XBE_DLSS_ULTRA_PERFORMANCE,
};

// the model DLSS runs: K (transformer), M or L (the newer ones, dearer, more so on cards without FP8), or the SDK's
// pick for the mode (K for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance)
enum XbeDlssModel {
    XBE_DLSS_MODEL_K = 0,
    XBE_DLSS_MODEL_M,
    XBE_DLSS_MODEL_L,
    XBE_DLSS_MODEL_BY_MODE,
};

// an image as NGX takes it: the whole of one level and layer, aspect the view's
typedef struct XbeDlssImage {
    VkImage image;
    VkImageView view;
    VkFormat format;
    VkImageAspectFlags aspect;
    uint32_t width, height;
} XbeDlssImage;

// a picture to upscale: color, depth and motion at the render size and read only (SHADER_READ_ONLY_OPTIMAL), output
// at the output size (GENERAL, a storage image). jitter: the render pixels the picture's 3D was moved by, x right and y
// down; motion_scale: render pixels per unit of motion (motion is where a pixel was less where it is); reset: no history
typedef struct XbeDlssEvaluate {
    XbeDlssImage color, depth, motion, output;
    float jitter[2];
    float motion_scale[2];
    int reset;
} XbeDlssEvaluate;

// a line for the game's log
typedef void (*XbeDlssLog)(const char* line);

typedef struct XbeDlssApi {
    uint32_t version;
    // first: where its lines go, the folder NVIDIA's DLSS library is in (the game's) and one for NGX's own files; the
    // strings are kept
    void (*setup)(XbeDlssLog log, const wchar_t* library_dir, const wchar_t* data_dir);
    // the Vulkan extensions NGX needs, the instance's before it is made and the device's before it is made; the names
    // stay valid until the next call. False: NGX isn't on this system
    int (*instance_extensions)(uint32_t* count, const char* const** names);
    int (*device_extensions)(VkInstance instance, VkPhysicalDevice gpu, uint32_t* count, const char* const** names);
    // NGX on the device; false when NGX or DLSS isn't there (the log says why)
    int (*init)(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                PFN_vkGetDeviceProcAddr gdpa);
    // the render size DLSS asks for the output size at quality
    int (*render_size)(uint32_t out_w, uint32_t out_h, int quality, uint32_t* w, uint32_t* h);
    // the upscaler for these sizes at quality running model (XbeDlssModel; any made before released first: the device
    // must be idle), recorded into cmd
    int (*create)(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h, int quality, int model);
    // a picture upscaled, recorded into cmd, outside a render pass; NGX leaves cmd's bindings its own
    int (*evaluate)(VkCommandBuffer cmd, const XbeDlssEvaluate* e);
    // the upscaler released, and NGX shut down: the device idle
    void (*release)(void);
    void (*shutdown)(void);
} XbeDlssApi;

typedef const XbeDlssApi* (*PFN_xbe_dlss_api)(uint32_t version);

#ifdef __cplusplus
}
#endif
