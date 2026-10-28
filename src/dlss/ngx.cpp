// The DLSS module (gpu/dlss.h): NVIDIA's NGX SDK behind the table the renderer loads. A library of its own, since NGX's
// static library is built for another C++ runtime than the game's (libstdc++ on Linux, Microsoft's on Windows: see
// msvc_runtime.cpp) and is NVIDIA's code under NVIDIA's licence. No C++ library in here either: only C.
#include "gpu/dlss.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvsdk_ngx_helpers_vk.h"

#ifdef _WIN32
#define XBE_DLSS_EXPORT __declspec(dllexport)
#else
#define XBE_DLSS_EXPORT __attribute__((visibility("default")))
#endif

namespace {

// NGX knows the game by a project id, any GUID for an engine of its own
constexpr const char* kProjectId = "4f6c2a1e-9b3d-4e57-a8c0-6d1f2b7e9a35";
constexpr const char* kEngineVersion = "1";

XbeDlssLog g_log = nullptr;
NVSDK_NGX_FeatureCommonInfo g_common{};
const wchar_t* g_library_dirs[1] = {};
const wchar_t* g_data_dir = L".";
VkDevice g_device = VK_NULL_HANDLE;
bool g_init = false;
NVSDK_NGX_Parameter* g_caps = nullptr;    // what NGX can do on this GPU
NVSDK_NGX_Parameter* g_params = nullptr;  // the upscaler's, made and evaluated with
NVSDK_NGX_Handle* g_feature = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_log(line);
}

// NGX's own lines, warnings and errors only unless XBE_DLSS_LOG asks for more
void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!g_log) return;
    char line[512];
    snprintf(line, sizeof(line), "dlss: ngx: %s", message);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    g_log(line);
}

const NVSDK_NGX_FeatureCommonInfo* Common() {
    g_common.PathListInfo.Path = g_library_dirs[0] ? g_library_dirs : nullptr;
    g_common.PathListInfo.Length = g_library_dirs[0] ? 1 : 0;
    g_common.LoggingInfo.LoggingCallback = NgxLog;
    g_common.LoggingInfo.MinimumLoggingLevel =
        getenv("XBE_DLSS_LOG") ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
    g_common.LoggingInfo.DisableOtherLoggingSinks = true;
    return &g_common;
}

NVSDK_NGX_FeatureDiscoveryInfo Discovery() {
    NVSDK_NGX_FeatureDiscoveryInfo d{};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    d.ApplicationDataPath = g_data_dir;
    d.FeatureInfo = Common();
    return d;
}

// extension names copied out of what NGX hands back
struct Names {
    char text[32][VK_MAX_EXTENSION_NAME_SIZE];
    const char* list[32];
    uint32_t count = 0;
    void Take(uint32_t n, const VkExtensionProperties* p) {
        count = 0;
        for (uint32_t i = 0; i < n && count < 32; i++) {
            snprintf(text[count], sizeof(text[count]), "%s", p[i].extensionName);
            list[count] = text[count];
            count++;
        }
    }
};
Names g_instance_names, g_device_names;

void Setup(XbeDlssLog log, const wchar_t* library_dir, const wchar_t* data_dir) {
    g_log = log;
    g_library_dirs[0] = library_dir;
    g_data_dir = data_dir;
}

int InstanceExtensions(uint32_t* count, const char* const** names) {
    NVSDK_NGX_FeatureDiscoveryInfo d = Discovery();
    uint32_t n = 0;
    VkExtensionProperties* p = nullptr;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &n, &p);
    if (NVSDK_NGX_FAILED(r)) {
        Log("dlss: no NGX on this system (instance extensions: %08X)", unsigned(r));
        return 0;
    }
    g_instance_names.Take(n, p);
    *count = g_instance_names.count;
    *names = g_instance_names.list;
    return 1;
}

int DeviceExtensions(VkInstance instance, VkPhysicalDevice gpu, uint32_t* count, const char* const** names) {
    NVSDK_NGX_FeatureDiscoveryInfo d = Discovery();
    uint32_t n = 0;
    VkExtensionProperties* p = nullptr;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, gpu, &d, &n, &p);
    if (NVSDK_NGX_FAILED(r)) {
        Log("dlss: no NGX for this GPU (device extensions: %08X)", unsigned(r));
        return 0;
    }
    g_device_names.Take(n, p);
    *count = g_device_names.count;
    *names = g_device_names.list;
    return 1;
}

void Shutdown();

int Init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
         PFN_vkGetDeviceProcAddr gdpa) {
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                              g_data_dir, instance, gpu, device, gipa, gdpa, Common(),
                                                              NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        Log("dlss: NGX failed to start (%08X)", unsigned(r));
        return 0;
    }
    g_init = true;
    g_device = device;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps);
    if (NVSDK_NGX_FAILED(r)) {
        Log("dlss: NGX has no capabilities to give (%08X)", unsigned(r));
        Shutdown();
        return 0;
    }
    int available = 0, driver = 0;
    unsigned major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &driver)) &&
        driver) {
        NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        Log("dlss: the driver is too old for DLSS, %u.%u or newer needed", major, minor);
    }
    if (!available) {
        int result = 0;
        NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &result);
        Log("dlss: DLSS isn't available on this GPU (%08X)", unsigned(result));
        Shutdown();
        return 0;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&g_params))) {
        Log("dlss: NGX has no parameters to give");
        Shutdown();
        return 0;
    }
    return 1;
}

NVSDK_NGX_PerfQuality_Value Perf(int quality) {
    switch (quality) {
        case XBE_DLSS_DLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
        case XBE_DLSS_QUALITY: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case XBE_DLSS_BALANCED: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case XBE_DLSS_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}

int RenderSize(uint32_t out_w, uint32_t out_h, int quality, uint32_t* w, uint32_t* h) {
    if (!g_caps) return 0;
    if (quality == XBE_DLSS_DLAA) {
        *w = out_w;
        *h = out_h;
        return 1;
    }
    void* callback = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(g_caps, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
    if (!callback) return 0;
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Width, out_w);
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Height, out_h);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_PerfQualityValue, Perf(quality));
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_RTXValue, 0);
    if (NVSDK_NGX_FAILED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(g_caps))) return 0;
    unsigned ow = 0, oh = 0;
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutWidth, &ow);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutHeight, &oh);
    if (!ow || !oh) return 0;
    *w = ow;
    *h = oh;
    return 1;
}

void Release() {
    if (g_feature) NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
    g_feature = nullptr;
}

int Create(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h, int quality) {
    if (!g_params) return 0;
    Release();
    NVSDK_NGX_DLSS_Create_Params p{};
    p.Feature.InWidth = w;
    p.Feature.InHeight = h;
    p.Feature.InTargetWidth = out_w;
    p.Feature.InTargetHeight = out_h;
    p.Feature.InPerfQualityValue = Perf(quality);
    // the picture as the game shows it (no HDR), motion at the render size and without the jitter, exposure DLSS's own
    p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, cmd, 1, 1, &g_feature, g_params, &p);
    if (NVSDK_NGX_FAILED(r)) {
        Log("dlss: the upscaler for %ux%u to %ux%u failed (%08X)", w, h, out_w, out_h, unsigned(r));
        g_feature = nullptr;
        return 0;
    }
    Log("dlss: upscaling %ux%u to %ux%u", w, h, out_w, out_h);
    return 1;
}

NVSDK_NGX_Resource_VK Resource(const XbeDlssImage& im, bool write) {
    VkImageSubresourceRange range{im.aspect, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, range, im.format, im.width, im.height, write);
}

int Evaluate(VkCommandBuffer cmd, const XbeDlssEvaluate* e) {
    if (!g_feature) return 0;
    NVSDK_NGX_Resource_VK color = Resource(e->color, false), depth = Resource(e->depth, false),
                          motion = Resource(e->motion, false), output = Resource(e->output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params p{};
    p.Feature.pInColor = &color;
    p.Feature.pInOutput = &output;
    p.pInDepth = &depth;
    p.pInMotionVectors = &motion;
    p.InJitterOffsetX = e->jitter[0];
    p.InJitterOffsetY = e->jitter[1];
    p.InRenderSubrectDimensions.Width = e->color.width;
    p.InRenderSubrectDimensions.Height = e->color.height;
    p.InReset = e->reset;
    p.InMVScaleX = e->motion_scale[0];
    p.InMVScaleY = e->motion_scale[1];
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, g_feature, g_params, &p);
    if (NVSDK_NGX_FAILED(r)) {
        static int logged = 0;
        if (logged++ < 8) Log("dlss: upscaling failed (%08X)", unsigned(r));
        return 0;
    }
    return 1;
}

void Shutdown() {
    Release();
    if (g_params) NVSDK_NGX_VULKAN_DestroyParameters(g_params);
    if (g_caps) NVSDK_NGX_VULKAN_DestroyParameters(g_caps);
    g_params = g_caps = nullptr;
    if (g_init) NVSDK_NGX_VULKAN_Shutdown1(g_device);
    g_init = false;
    g_device = VK_NULL_HANDLE;
}

const XbeDlssApi kApi = {XBE_DLSS_API_VERSION, Setup, InstanceExtensions, DeviceExtensions, Init, RenderSize,
                         Create, Evaluate, Release, Shutdown};

}  // namespace

extern "C" XBE_DLSS_EXPORT const XbeDlssApi* xbe_dlss_api(uint32_t version) {
    return version == XBE_DLSS_API_VERSION ? &kApi : nullptr;
}
