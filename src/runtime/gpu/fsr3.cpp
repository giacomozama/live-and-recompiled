// AMD FSR 3.1's upscaler run on the renderer's device (gpu/fsr3.h). What AMD's SDK does on the CPU for it
// (ffx_fsr3upscaler.cpp: its images, constants, clears and the order of its passes) is redone here in its terms, with
// its names in the comments; the shaders are AMD's GLSL unchanged but for three declarations (Patched).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "gpu/fsr3.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"

namespace gpu::fsr3 {

struct SourceFile {
    const char* path;
    const char* text;
    size_t size;
};
extern const SourceFile kSources[];  // xbe_ffx_sources.cpp, made from src/runtime/gpu/ffx by CMake
extern const size_t kSourceCount;

namespace {

// the images AMD's dispatch names by FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_*, the history pairs as theirs (1, 2), and
// what a pass binds that is the frame's (the inputs, the output) or one of a pair by the frame's parity
enum Res : uint8_t {
    kInColor,
    kInDepth,
    kInMotion,
    kOutput,
    kAccumRead,       // ACCUMULATION as SRV: the pair's by parity
    kAccumWrite,      // as UAV: the other
    kLumaCurrent,     // CURRENT_LUMA (LUMA_1 / LUMA_2 by parity)
    kLumaPrevious,    // PREVIOUS_LUMA: the other
    kUpscaledRead,    // INTERNAL_UPSCALED_COLOR as SRV
    kUpscaledWrite,   // as UAV
    kHistoryRead,     // LUMA_HISTORY as SRV
    kHistoryWrite,    // as UAV
    kIntermediate,    // INTERMEDIATE_FP16x1: FARTHEST_DEPTH, later LUMA_INSTABILITY
    kShadingChange,   // SHADING_CHANGE
    kNewLocks,        // NEW_LOCKS
    kSpdMips,         // SPD_MIPS, every level
    kSpdMip0,         // SPD_MIPS_LEVEL_0..5, one level each
    kSpdMip5 = kSpdMip0 + 5,
    kFarthestMip1,    // FARTHEST_DEPTH_MIP1
    kSpdAtomic,       // SPD_ATOMIC_COUNT
    kDilatedReactive, // DILATED_REACTIVE_MASKS
    kLanczos,         // LANCZOS_LUT
    kDefaultReactive, // INTERNAL_DEFAULT_REACTIVITY: reactive and transparency masks, none given
    kExposure,        // INTERNAL_DEFAULT_EXPOSURE: none given, no auto exposure
    kFrameInfo,       // FRAME_INFO
    kDilatedDepth,    // the shared resources the app makes: DILATED_DEPTH,
    kDilatedMotion,   // DILATED_MOTION_VECTORS,
    kReconDepth,      // RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH
    kCbMain,          // cbFSR3Upscaler
    kCbSpd,           // cbSPD
    kInReactive,      // INPUT_REACTIVE_MASK: the picture's (Evaluate::reactive), else the default
    kInOpaque,        // the reactive mask's pass (Reactive): INPUT_OPAQUE_ONLY,
    kReactiveColor,   // INPUT_COLOR (the picture with what blends over it),
    kReactiveOut,     // AUTOREACTIVE (and AUTOCOMPOSITION, which it doesn't write)
    kCbReactive,      // cbGenerateReactive
};

enum Kind : uint8_t { kSrv, kUav, kCb };

struct Bind {
    uint8_t binding;
    Res res;
    Kind kind;
};

// a pass: its shader (ffx/passes), its bindings as the shader's FSR3UPSCALER_BIND_* defines number them
struct PassDesc {
    const char* name;
    std::vector<Bind> binds;
};

enum Pass { kPrepareInputs, kLumaPyramid, kShadingChangePyramid, kShadingChangePass, kPrepareReactivity,
            kLumaInstability, kAccumulate, kAutogenReactive, kPassCount };

const PassDesc& Desc(int pass) {
    static const PassDesc kPasses[kPassCount] = {
        {"prepare_inputs",
         {{0, kInMotion, kSrv}, {1, kInDepth, kSrv}, {2, kInColor, kSrv}, {3, kDilatedMotion, kUav},
          {4, kDilatedDepth, kUav}, {5, kReconDepth, kUav}, {6, kIntermediate, kUav}, {7, kLumaCurrent, kUav},
          {8, kCbMain, kCb}}},
        {"luma_pyramid",
         {{0, kLumaCurrent, kSrv}, {1, kIntermediate, kSrv}, {2, kSpdAtomic, kUav}, {3, kFrameInfo, kUav},
          {4, Res(kSpdMip0 + 0), kUav}, {5, Res(kSpdMip0 + 1), kUav}, {6, Res(kSpdMip0 + 2), kUav},
          {7, Res(kSpdMip0 + 3), kUav}, {8, Res(kSpdMip0 + 4), kUav}, {9, Res(kSpdMip0 + 5), kUav},
          {10, kFarthestMip1, kUav}, {11, kCbMain, kCb}, {12, kCbSpd, kCb}}},
        {"shading_change_pyramid",
         {{0, kLumaCurrent, kSrv}, {1, kLumaPrevious, kSrv}, {2, kDilatedMotion, kSrv}, {3, kExposure, kSrv},
          {4, kSpdAtomic, kUav}, {5, Res(kSpdMip0 + 0), kUav}, {6, Res(kSpdMip0 + 1), kUav},
          {7, Res(kSpdMip0 + 2), kUav}, {8, Res(kSpdMip0 + 3), kUav}, {9, Res(kSpdMip0 + 4), kUav},
          {10, Res(kSpdMip0 + 5), kUav}, {11, kCbMain, kCb}, {12, kCbSpd, kCb}}},
        {"shading_change", {{0, kSpdMips, kSrv}, {1, kShadingChange, kUav}, {2, kCbMain, kCb}}},
        {"prepare_reactivity",
         {{0, kReconDepth, kSrv}, {1, kDilatedMotion, kSrv}, {2, kDilatedDepth, kSrv}, {3, kInReactive, kSrv},
          {4, kDefaultReactive, kSrv}, {5, kAccumRead, kSrv}, {6, kShadingChange, kSrv}, {7, kLumaCurrent, kSrv},
          {8, kExposure, kSrv}, {9, kDilatedReactive, kUav}, {10, kNewLocks, kUav}, {11, kAccumWrite, kUav},
          {12, kCbMain, kCb}}},
        {"luma_instability",
         {{0, kExposure, kSrv}, {1, kDilatedReactive, kSrv}, {2, kDilatedMotion, kSrv}, {3, kFrameInfo, kSrv},
          {4, kHistoryRead, kSrv}, {5, kFarthestMip1, kSrv}, {6, kLumaCurrent, kSrv}, {7, kHistoryWrite, kUav},
          {8, kIntermediate, kUav}, {9, kCbMain, kCb}}},
        {"accumulate",
         {{0, kExposure, kSrv}, {1, kDilatedReactive, kSrv}, {2, kDilatedMotion, kSrv}, {3, kUpscaledRead, kSrv},
          {4, kLanczos, kSrv}, {5, kFarthestMip1, kSrv}, {6, kLumaCurrent, kSrv}, {7, kIntermediate, kSrv},
          {8, kInColor, kSrv}, {9, kUpscaledWrite, kUav}, {10, kOutput, kUav}, {11, kNewLocks, kUav},
          {12, kCbMain, kCb}}},
        {"autogen_reactive",
         {{0, kInOpaque, kSrv}, {1, kReactiveColor, kSrv}, {2, kReactiveOut, kUav}, {3, kReactiveOut, kUav},
          {4, kCbMain, kCb}, {5, kCbReactive, kCb}}},
    };
    return kPasses[pass];
}

// the samplers' bindings, past every pass's own (AMD's are 1000 and 1001, Patched)
constexpr uint32_t kBindPoint = 30, kBindLinear = 31;

// the permutation AMD's backend would pick here: SDR color, motion vectors at the render size and without the jitter,
// depth 0 near, no sharpening (the renderer's RCAS runs after), reference Lanczos (the LUT is AMD's on wave64 GPUs)
constexpr const char* kPreamble =
    "#define FFX_GPU 1\n"
    "#define FFX_GLSL 1\n"
    "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 1\n"
    "#define FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2\n"
    "#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1\n"
    "#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 0\n";

const SourceFile* FindSource(const std::string& path) {
    for (size_t i = 0; i < kSourceCount; i++)
        if (path == kSources[i].path) return &kSources[i];
    return nullptr;
}

// the callbacks header with three declarations changed: the luma history's storage format as the image's (AMD's
// says rgba8 of an RGBA16F image, which their backend's typed stores pass over), the output's format given (AMD's
// leaves it to shaderStorageImageWriteWithoutFormat), and the samplers' bindings within a push descriptor set's reach
const std::string* Patched() {
    static const std::string text = [] {
        const SourceFile* f = FindSource("fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h");
        if (!f) return std::string();
        std::string s(f->text, f->size);
        const std::pair<const char*, const char*> edits[] = {
            {"FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8)", "FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f)"},
            {"FSR3UPSCALER_BIND_UAV_UPSCALED_OUTPUT /* app controlled format */)",
             "FSR3UPSCALER_BIND_UAV_UPSCALED_OUTPUT, rgba8)"},
            {"binding = 1000)", "binding = 30)"},
            {"binding = 1001)", "binding = 31)"},
        };
        for (const auto& [from, to] : edits) {
            const size_t at = s.find(from);
            if (at == std::string::npos) {
                LOGE("fsr3: the callbacks header has no '%s'", from);
                return std::string();
            }
            s.replace(at, std::strlen(from), to);
        }
        return s;
    }();
    return text.empty() ? nullptr : &text;
}

// #include "..." from the embedded files: beside the includer, then from ffx/ and ffx/fsr3upscaler/ (the SDK's -I)
class Includer : public glslang::TShader::Includer {
public:
    IncludeResult* includeSystem(const char* name, const char* from, size_t) override { return Find(name, from); }
    IncludeResult* includeLocal(const char* name, const char* from, size_t) override { return Find(name, from); }
    void releaseInclude(IncludeResult* r) override { delete r; }

private:
    IncludeResult* Find(const char* name, const char* from) {
        std::string dir = from ? from : "";
        const size_t slash = dir.rfind('/');
        dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
        for (const std::string& path : {dir + name, std::string(name), "fsr3upscaler/" + std::string(name)}) {
            const SourceFile* f = FindSource(path);
            if (!f) continue;
            if (path == "fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h") {
                const std::string* p = Patched();
                if (!p) return nullptr;
                return new IncludeResult(f->path, p->data(), p->size(), nullptr);
            }
            return new IncludeResult(f->path, f->text, f->size, nullptr);
        }
        return nullptr;
    }
};

bool Compile(const char* pass, std::vector<uint32_t>& spv, std::string& err) {
    const std::string path = std::string("passes/ffx_fsr3upscaler_") + pass + "_pass.glsl";
    const SourceFile* f = FindSource(path);
    if (!f) {
        err = path + " isn't in the executable";
        return false;
    }
    glslang::TShader sh(EShLangCompute);
    const char* text = f->text;
    const int len = int(f->size);
    const char* name = f->path;
    sh.setStringsWithLengthsAndNames(&text, &len, &name, 1);
    sh.setPreamble(kPreamble);
    sh.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    Includer inc;
    if (!sh.parse(GetDefaultResources(), 450, false, msgs, inc)) {
        err = sh.getInfoLog();
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs)) {
        err = prog.getInfoLog();
        return false;
    }
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;
    spv.clear();
    glslang::GlslangToSpv(*prog.getIntermediate(EShLangCompute), spv, &opt);
    if (spv.empty()) err = "no SPIR-V";
    return !spv.empty();
}

struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;           // every level
    std::array<VkImageView, 6> mips{};          // SPD's: one level each, the last again past the image's
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, levels = 1;
};

// the formats FSR's images are stored as, each needing storage use (the atomic one atomics)
constexpr VkFormat kStorageFormats[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
                                        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,
                                        VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT};

// cbFSR3Upscaler (Fsr3UpscalerConstants), std140 as the C struct packs
struct Constants {
    int32_t render_size[2];
    int32_t previous_render_size[2];
    int32_t upscale_size[2];
    int32_t previous_upscale_size[2];
    int32_t max_render_size[2];
    int32_t max_upscale_size[2];
    float device_to_view_depth[4];
    float jitter[2];
    float previous_jitter[2];
    float motion_vector_scale[2];
    float downscale_factor[2];
    float motion_vector_jitter_cancellation[2];
    float tan_half_fov;
    float jitter_phase_count;
    float delta_time;
    float delta_pre_exposure;
    float view_space_to_meters;
    float frame_index;
    float velocity_factor;
    float reactiveness_scale;
    float shading_change_scale;
    float accumulation_added_per_frame;
    float min_disocclusion_accumulation;
};
static_assert(sizeof(Constants) == 148);

// cbSPD (Fsr3UpscalerSpdConstants)
struct SpdConstants {
    uint32_t mips;
    uint32_t num_work_groups;
    uint32_t work_group_offset[2];
    uint32_t render_size[2];
};

constexpr uint32_t kRingSlots = 512;  // constant buffer slots, two a picture: far more than are ever in flight

float Lanczos2(float x) {
    constexpr float kPi = 3.14159265358979f;
    if (std::fabs(x) < 1e-6f) return 1.0f;
    return (std::sin(kPi * x) / (kPi * x)) * (std::sin(0.5f * kPi * x) / (0.5f * kPi * x));
}

}  // namespace

struct Upscaler::State {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VmaAllocator vma = nullptr;
    // the passes', made once and kept across sizes
    VkSampler point = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, kPassCount> set_layouts{};
    std::array<VkPipelineLayout, kPassCount> layouts{};
    std::array<VkPipeline, kPassCount> pipes{};
    bool compiled = false, failed = false;
    // constant buffers: a host-visible ring of slots
    VkBuffer ring = VK_NULL_HANDLE;
    VmaAllocation ring_alloc = nullptr;
    uint8_t* ring_map = nullptr;
    VkDeviceSize slot_bytes = 256;
    uint32_t ring_next = 0;
    // the sizes' images
    std::array<Tex, 2> accum, luma, upscaled, history;
    Tex intermediate, shading_change, new_locks, spd_mips, farthest_mip1, spd_atomic, dilated_reactive, lanczos,
        default_reactive, exposure, frame_info, dilated_depth, dilated_motion, recon_depth;
    VkBuffer lut_upload = VK_NULL_HANDLE;
    VmaAllocation lut_upload_alloc = nullptr;
    uint32_t w = 0, h = 0, out_w = 0, out_h = 0;
    // the dispatch's own (FfxFsr3UpscalerContext_Private's)
    Constants c{};
    bool first = true;
    uint32_t frame_index = 0;  // resourceFrameIndex

    std::vector<Tex*> AllTex() {
        std::vector<Tex*> all{&intermediate, &shading_change,   &new_locks, &spd_mips,      &farthest_mip1,
                              &spd_atomic,   &dilated_reactive, &lanczos,   &default_reactive, &exposure,
                              &frame_info,   &dilated_depth,    &dilated_motion, &recon_depth};
        for (auto* pair : {&accum, &luma, &upscaled, &history})
            for (Tex& t : *pair) all.push_back(&t);
        return all;
    }

    bool MakeTex(Tex& t, VkFormat format, uint32_t tw, uint32_t th, bool storage, uint32_t levels = 1) {
        t.format = format;
        t.w = std::max(tw, 1u);
        t.h = std::max(th, 1u);
        t.levels = levels;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {t.w, t.h, 1};
        ici.mipLevels = levels;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(vma, &ici, &aci, &t.image, &t.alloc, nullptr) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        if (vkCreateImageView(dev, &vci, nullptr, &t.view) != VK_SUCCESS) return false;
        if (levels > 1) {
            for (uint32_t m = 0; m < 6; m++) {
                vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, std::min(m, levels - 1), 1, 0, 1};
                if (vkCreateImageView(dev, &vci, nullptr, &t.mips[m]) != VK_SUCCESS) return false;
            }
        }
        return true;
    }

    void FreeTex(Tex& t) {
        for (VkImageView v : t.mips)
            if (v) vkDestroyImageView(dev, v, nullptr);
        if (t.view) vkDestroyImageView(dev, t.view, nullptr);
        if (t.alloc) vmaDestroyImage(vma, t.image, t.alloc);
        t = {};
    }

    bool MakePipelines() {
        if (compiled) return true;
        if (failed) return false;
        failed = true;  // until it has them all
        const auto t0 = std::chrono::steady_clock::now();
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (vkCreateSampler(dev, &sci, nullptr, &point) != VK_SUCCESS) return false;
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (vkCreateSampler(dev, &sci, nullptr, &linear) != VK_SUCCESS) return false;
        for (int p = 0; p < kPassCount; p++) {
            const PassDesc& d = Desc(p);
            std::vector<uint32_t> spv;
            std::string err;
            if (!Compile(d.name, spv, err)) {
                LOGE("fsr3: %s: %s", d.name, err.c_str());
                return false;
            }
            std::vector<VkDescriptorSetLayoutBinding> b;
            for (const Bind& bind : d.binds) {
                const VkDescriptorType type = bind.kind == kSrv   ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                              : bind.kind == kUav ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                  : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                b.push_back({bind.binding, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            }
            b.push_back({kBindPoint, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &point});
            b.push_back({kBindLinear, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &linear});
            VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dci.bindingCount = uint32_t(b.size());
            dci.pBindings = b.data();
            if (vkCreateDescriptorSetLayout(dev, &dci, nullptr, &set_layouts[p]) != VK_SUCCESS) return false;
            VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            lci.setLayoutCount = 1;
            lci.pSetLayouts = &set_layouts[p];
            if (vkCreatePipelineLayout(dev, &lci, nullptr, &layouts[p]) != VK_SUCCESS) return false;
            VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mci.codeSize = spv.size() * 4;
            mci.pCode = spv.data();
            VkShaderModule mod = VK_NULL_HANDLE;
            if (vkCreateShaderModule(dev, &mci, nullptr, &mod) != VK_SUCCESS) return false;
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = mod;
            cpi.stage.pName = "main";
            cpi.layout = layouts[p];
            const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipes[p]);
            vkDestroyShaderModule(dev, mod, nullptr);
            if (r != VK_SUCCESS) {
                LOGE("fsr3: %s: pipeline not made (%d)", d.name, int(r));
                return false;
            }
        }
        // the constant buffers' ring
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        slot_bytes = std::max<VkDeviceSize>(256, props.limits.minUniformBufferOffsetAlignment);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = slot_bytes * kRingSlots;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(vma, &bci, &aci, &ring, &ring_alloc, &info) != VK_SUCCESS) return false;
        ring_map = static_cast<uint8_t*>(info.pMappedData);
        failed = false;
        compiled = true;
        LOGI("fsr3: shaders compiled in %.0f ms",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return true;
    }

    // a slot of the ring with these bytes in it
    VkDescriptorBufferInfo Stage(const void* data, size_t bytes) {
        const VkDeviceSize at = slot_bytes * ring_next;
        ring_next = (ring_next + 1) % kRingSlots;
        std::memcpy(ring_map + at, data, bytes);
        vmaFlushAllocation(vma, ring_alloc, at, slot_bytes);
        return {ring, at, VkDeviceSize(bytes)};
    }

    void Release() {
        for (Tex* t : AllTex()) FreeTex(*t);
        if (lut_upload_alloc) vmaDestroyBuffer(vma, lut_upload, lut_upload_alloc);
        lut_upload = VK_NULL_HANDLE;
        lut_upload_alloc = nullptr;
        w = h = out_w = out_h = 0;
    }

    ~State() {
        Release();
        for (VkPipeline p : pipes)
            if (p) vkDestroyPipeline(dev, p, nullptr);
        for (VkPipelineLayout l : layouts)
            if (l) vkDestroyPipelineLayout(dev, l, nullptr);
        for (VkDescriptorSetLayout l : set_layouts)
            if (l) vkDestroyDescriptorSetLayout(dev, l, nullptr);
        for (VkSampler s : {point, linear})
            if (s) vkDestroySampler(dev, s, nullptr);
        if (ring_alloc) vmaDestroyBuffer(vma, ring, ring_alloc);
    }
};

void DepthToView(float near_plane, float far_plane, float fov_y, float aspect, float out[4]) {
    const float n = std::min(near_plane, far_plane), f = std::max(near_plane, far_plane);
    const float q = f / (n - f);
    out[0] = -q;
    out[1] = q * n;
    const float b = 1.0f / std::tan(0.5f * fov_y), a = b / aspect;
    out[2] = 1.0f / a;
    out[3] = 1.0f / b;
}

float Ratio(uint32_t quality) {
    constexpr float kRatios[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return kRatios[std::min<uint32_t>(quality, 4)];
}

// ffxFsr3UpscalerGetRenderResolutionFromQualityMode
void RenderSize(uint32_t out_w, uint32_t out_h, uint32_t quality, uint32_t& w, uint32_t& h) {
    const float ratio = Ratio(quality);
    w = std::max(uint32_t(float(out_w) / ratio), 1u);
    h = std::max(uint32_t(float(out_h) / ratio), 1u);
}

std::unique_ptr<Upscaler> Upscaler::Make(VkPhysicalDevice gpu, VkDevice dev, VmaAllocator_T* vma) {
    if (!vkCmdPushDescriptorSetKHR) {
        LOGW("fsr3: the device has no push descriptors");
        return nullptr;
    }
    for (VkFormat f : kStorageFormats) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (f == VK_FORMAT_R32_UINT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            LOGW("fsr3: the GPU can't store format %d in a shader", int(f));
            return nullptr;
        }
    }
    auto s = std::make_unique<State>();
    s->gpu = gpu;
    s->dev = dev;
    s->vma = vma;
    return std::unique_ptr<Upscaler>(new Upscaler(std::move(s)));
}

Upscaler::Upscaler(std::unique_ptr<State> s) : s_(std::move(s)) {}
Upscaler::~Upscaler() = default;

void Upscaler::Release() { s_->Release(); }
bool Upscaler::Prepare() { return s_->MakePipelines(); }

// ffxFsr3UpscalerContextGenerateReactiveMask with the parameters AMD's samples give it: tonemapped, the largest
// channel's change, 0.9 past 0.2 and 0 under
bool Upscaler::Reactive(VkCommandBuffer cmd, const XbeDlssImage& opaque, const XbeDlssImage& color,
                        const XbeDlssImage& out) {
    State& s = *s_;
    if (!s.compiled || opaque.width != color.width || opaque.height != color.height || out.width != color.width ||
        out.height != color.height)
        return false;
    struct {
        float scale, threshold, binary;
        uint32_t flags;
    } k{1.0f, 0.2f, 0.9f, 1u | 4u | 8u};  // APPLY_TONEMAP | APPLY_THRESHOLD | USE_COMPONENTS_MAX
    const VkDescriptorBufferInfo cb_main = s.Stage(&s.c, sizeof s.c);
    const VkDescriptorBufferInfo cb_reactive = s.Stage(&k, sizeof k);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    const PassDesc& d = Desc(kAutogenReactive);
    std::array<VkDescriptorImageInfo, 8> images{};
    std::array<VkWriteDescriptorSet, 8> writes{};
    uint32_t n = 0;
    for (const Bind& b : d.binds) {
        VkWriteDescriptorSet& w = writes[n];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        if (b.kind == kCb) {
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = b.res == kCbReactive ? &cb_reactive : &cb_main;
        } else {
            w.descriptorType = b.kind == kSrv ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            images[n] = b.res == kInOpaque       ? VkDescriptorImageInfo{VK_NULL_HANDLE, opaque.view,
                                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                        : b.res == kReactiveColor ? VkDescriptorImageInfo{VK_NULL_HANDLE, color.view,
                                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                                                  : VkDescriptorImageInfo{VK_NULL_HANDLE, out.view,
                                                                          VK_IMAGE_LAYOUT_GENERAL};
            w.pImageInfo = &images[n];
        }
        n++;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipes[kAutogenReactive]);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layouts[kAutogenReactive], 0, n, writes.data());
    vkCmdDispatch(cmd, (color.width + 7) / 8, (color.height + 7) / 8, 1);
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    return true;
}

// fsr3upscalerCreate: its images (FfxInternalResourceDescription's and the shared ones), and their first contents
bool Upscaler::Create(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h) {
    State& s = *s_;
    if (!s.MakePipelines()) return false;
    s.Release();
    const uint32_t w2 = std::max(w / 2, 1u), h2 = std::max(h / 2, 1u);
    const uint32_t spd_levels = uint32_t(std::floor(std::log2(float(std::max(w2, h2))))) + 1;
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        ok = ok && s.MakeTex(s.accum[i], VK_FORMAT_R8_UNORM, w, h, true);
        ok = ok && s.MakeTex(s.luma[i], VK_FORMAT_R16_SFLOAT, w, h, true);
        ok = ok && s.MakeTex(s.upscaled[i], VK_FORMAT_R16G16B16A16_SFLOAT, out_w, out_h, true);
        ok = ok && s.MakeTex(s.history[i], VK_FORMAT_R16G16B16A16_SFLOAT, w, h, true);
    }
    ok = ok && s.MakeTex(s.intermediate, VK_FORMAT_R16_SFLOAT, w, h, true) &&
         s.MakeTex(s.shading_change, VK_FORMAT_R8_UNORM, w2, h2, true) &&
         s.MakeTex(s.new_locks, VK_FORMAT_R8_UNORM, out_w, out_h, true) &&
         s.MakeTex(s.spd_mips, VK_FORMAT_R16G16_SFLOAT, w2, h2, true, spd_levels) &&
         s.MakeTex(s.farthest_mip1, VK_FORMAT_R16_SFLOAT, w2, h2, true) &&
         s.MakeTex(s.spd_atomic, VK_FORMAT_R32_UINT, 1, 1, true) &&
         s.MakeTex(s.dilated_reactive, VK_FORMAT_R8G8B8A8_UNORM, w, h, true) &&
         s.MakeTex(s.lanczos, VK_FORMAT_R16_SNORM, 128, 1, false) &&
         s.MakeTex(s.default_reactive, VK_FORMAT_R8_UNORM, 1, 1, false) &&
         s.MakeTex(s.exposure, VK_FORMAT_R32G32_SFLOAT, 1, 1, false) &&
         s.MakeTex(s.frame_info, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, true) &&
         s.MakeTex(s.dilated_depth, VK_FORMAT_R32_SFLOAT, w, h, true) &&
         s.MakeTex(s.dilated_motion, VK_FORMAT_R16G16_SFLOAT, w, h, true) &&
         s.MakeTex(s.recon_depth, VK_FORMAT_R32_UINT, w, h, true);
    // the Lanczos table's 128 weights, through a buffer
    std::array<int16_t, 128> lut{};
    for (uint32_t i = 0; i < 128; i++)
        lut[i] = int16_t(std::lround(Lanczos2(2.0f * float(i) / 127.0f) * 32767.0f));
    if (ok) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizeof lut;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        ok = vmaCreateBuffer(s.vma, &bci, &aci, &s.lut_upload, &s.lut_upload_alloc, &info) == VK_SUCCESS;
        if (ok) {
            std::memcpy(info.pMappedData, lut.data(), sizeof lut);
            vmaFlushAllocation(s.vma, s.lut_upload_alloc, 0, VK_WHOLE_SIZE);
        }
    }
    if (!ok) {
        LOGE("fsr3: its images couldn't be made for %ux%u to %ux%u", w, h, out_w, out_h);
        s.Release();
        return false;
    }
    s.w = w;
    s.h = h;
    s.out_w = out_w;
    s.out_h = out_h;
    // every image GENERAL from here on, where each stays; the read-only ones filled
    std::vector<VkImageMemoryBarrier> to_general;
    for (Tex* t : s.AllTex()) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels, 0, 1};
        to_general.push_back(b);
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, uint32_t(to_general.size()), to_general.data());
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {128, 1, 1};
    vkCmdCopyBufferToImage(cmd, s.lut_upload, s.lanczos.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    const VkClearColorValue zero{};
    for (Tex* t : s.AllTex()) {
        if (t == &s.lanczos) continue;
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels, 0, 1};
        vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    // defaults (fsr3upscalerCreate)
    s.c = {};
    s.c.max_upscale_size[0] = int32_t(out_w);
    s.c.max_upscale_size[1] = int32_t(out_h);
    s.c.velocity_factor = 1.0f;
    s.c.reactiveness_scale = 1.0f;
    s.c.shading_change_scale = 1.0f;
    s.c.accumulation_added_per_frame = 1.0f / 3.0f;
    s.c.min_disocclusion_accumulation = -1.0f / 3.0f;
    s.first = true;
    s.frame_index = 0;
    LOGI("fsr3: upscaling %ux%u to %ux%u", w, h, out_w, out_h);
    return true;
}

// fsr3upscalerDispatch
bool Upscaler::Run(VkCommandBuffer cmd, const Evaluate& e) {
    State& s = *s_;
    if (!s.compiled || !s.w || e.color.width != s.w || e.color.height != s.h || e.output.width != s.out_w ||
        e.output.height != s.out_h)
        return false;
    const bool odd = s.frame_index & 1;
    Tex& accum_read = s.accum[odd ? 1 : 0];
    Tex& accum_write = s.accum[odd ? 0 : 1];
    Tex& upscaled_read = s.upscaled[odd ? 1 : 0];
    Tex& upscaled_write = s.upscaled[odd ? 0 : 1];
    Tex& history_read = s.history[odd ? 1 : 0];
    Tex& history_write = s.history[odd ? 0 : 1];
    Tex& luma_current = s.luma[odd ? 1 : 0];
    Tex& luma_previous = s.luma[odd ? 0 : 1];
    const bool reset = e.reset || s.first;

    // what wrote the inputs and read this upscaler's images last, done before the clears and passes
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    auto clear = [&](Tex& t, VkClearColorValue v) {
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, 1};
        vkCmdClearColorImage(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &all);
    };
    const VkClearColorValue zero{};
    if (s.first) {
        for (Tex* t : {&s.accum[0], &s.accum[1], &s.luma[0], &s.luma[1]}) clear(*t, zero);
    }
    s.first = false;

    // constants
    Constants& c = s.c;
    const float rw = float(e.color.width), rh = float(e.color.height);
    c.previous_jitter[0] = c.jitter[0];
    c.previous_jitter[1] = c.jitter[1];
    c.jitter[0] = e.jitter[0];
    c.jitter[1] = e.jitter[1];
    c.previous_render_size[0] = c.render_size[0];
    c.previous_render_size[1] = c.render_size[1];
    c.render_size[0] = int32_t(e.color.width);
    c.render_size[1] = int32_t(e.color.height);
    c.max_render_size[0] = int32_t(s.w);
    c.max_render_size[1] = int32_t(s.h);
    std::copy_n(e.depth_to_view, 4, c.device_to_view_depth);
    c.tan_half_fov = e.depth_to_view[2];  // the horizontal's: ndc x 1 across the distance
    c.view_space_to_meters = e.meters_per_unit > 0 ? e.meters_per_unit : 1.0f;
    c.previous_upscale_size[0] = c.upscale_size[0];
    c.previous_upscale_size[1] = c.upscale_size[1];
    c.upscale_size[0] = int32_t(e.output.width);
    c.upscale_size[1] = int32_t(e.output.height);
    c.downscale_factor[0] = rw / float(e.output.width);
    c.downscale_factor[1] = rh / float(e.output.height);
    c.delta_pre_exposure = 1.0f;
    c.motion_vector_scale[0] = e.motion_scale[0] / rw;
    c.motion_vector_scale[1] = e.motion_scale[1] / rh;
    c.motion_vector_jitter_cancellation[0] = c.motion_vector_jitter_cancellation[1] = 0;
    // ffxFsr3UpscalerGetJitterPhaseCount, stepped toward
    const float ratio = float(e.output.width) / rw;
    const int32_t phases = int32_t(8.0f * ratio * ratio);
    if (reset || c.jitter_phase_count == 0) c.jitter_phase_count = float(phases);
    else if (phases > int32_t(c.jitter_phase_count)) c.jitter_phase_count += 1;
    else if (phases < int32_t(c.jitter_phase_count)) c.jitter_phase_count -= 1;
    c.delta_time = std::clamp(e.frame_ms / 1000.0f, 0.0f, 1.0f);
    c.frame_index = reset ? 0.0f : c.frame_index + 1.0f;

    // ffxSpdSetup over the render size
    const uint32_t spd_x = (e.color.width - 1) / 64 + 1, spd_y = (e.color.height - 1) / 64 + 1;
    SpdConstants spd{};
    spd.num_work_groups = spd_x * spd_y;
    spd.mips = uint32_t(std::min(std::floor(std::log2(float(std::max(e.color.width, e.color.height)))), 12.0f));
    spd.render_size[0] = e.color.width;
    spd.render_size[1] = e.color.height;

    // the clears each picture
    if (reset) {
        clear(accum_read, zero);
        clear(s.spd_mips, zero);
        VkClearColorValue info{};
        info.float32[0] = -1.0f;
        info.float32[1] = 1.0f;
        clear(s.frame_info, info);
    }
    VkClearColorValue far_depth{};
    far_depth.uint32[0] = 0x3F800000u;  // 1.0f's bits: the depth kept as a uint, nearest by atomics
    clear(s.recon_depth, far_depth);
    clear(s.spd_atomic, zero);
    clear(s.spd_mips, zero);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);

    const VkDescriptorBufferInfo cb_main = s.Stage(&c, sizeof c);
    const VkDescriptorBufferInfo cb_spd = s.Stage(&spd, sizeof spd);

    // a binding's image as the pass reads or writes it
    auto image = [&](Res r, VkDescriptorImageInfo& info) {
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        switch (r) {
            case kInColor: info = {VK_NULL_HANDLE, e.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; return;
            case kInDepth: info = {VK_NULL_HANDLE, e.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; return;
            case kInMotion: info = {VK_NULL_HANDLE, e.motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; return;
            case kOutput: info.imageView = e.output.view; return;
            case kAccumRead: info.imageView = accum_read.view; return;
            case kAccumWrite: info.imageView = accum_write.view; return;
            case kLumaCurrent: info.imageView = luma_current.view; return;
            case kLumaPrevious: info.imageView = luma_previous.view; return;
            case kUpscaledRead: info.imageView = upscaled_read.view; return;
            case kUpscaledWrite: info.imageView = upscaled_write.view; return;
            case kHistoryRead: info.imageView = history_read.view; return;
            case kHistoryWrite: info.imageView = history_write.view; return;
            case kIntermediate: info.imageView = s.intermediate.view; return;
            case kShadingChange: info.imageView = s.shading_change.view; return;
            case kNewLocks: info.imageView = s.new_locks.view; return;
            case kSpdMips: info.imageView = s.spd_mips.view; return;
            case kFarthestMip1: info.imageView = s.farthest_mip1.view; return;
            case kSpdAtomic: info.imageView = s.spd_atomic.view; return;
            case kDilatedReactive: info.imageView = s.dilated_reactive.view; return;
            case kLanczos: info.imageView = s.lanczos.view; return;
            case kDefaultReactive: info.imageView = s.default_reactive.view; return;
            case kExposure: info.imageView = s.exposure.view; return;
            case kFrameInfo: info.imageView = s.frame_info.view; return;
            case kDilatedDepth: info.imageView = s.dilated_depth.view; return;
            case kDilatedMotion: info.imageView = s.dilated_motion.view; return;
            case kReconDepth: info.imageView = s.recon_depth.view; return;
            case kInReactive: info.imageView = e.reactive.view ? e.reactive.view : s.default_reactive.view; return;
            default:
                if (r >= kSpdMip0 && r <= kSpdMip5) info.imageView = s.spd_mips.mips[r - kSpdMip0];
                return;
        }
    };
    auto dispatch = [&](int pass, uint32_t x, uint32_t y) {
        const PassDesc& d = Desc(pass);
        std::array<VkDescriptorImageInfo, 16> images{};
        std::array<VkWriteDescriptorSet, 16> writes{};
        uint32_t n = 0;
        for (const Bind& b : d.binds) {
            VkWriteDescriptorSet& w = writes[n];
            w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = b.binding;
            w.descriptorCount = 1;
            if (b.kind == kCb) {
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo = b.res == kCbSpd ? &cb_spd : &cb_main;
            } else {
                w.descriptorType = b.kind == kSrv ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                image(b.res, images[n]);
                w.pImageInfo = &images[n];
            }
            n++;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipes[pass]);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layouts[pass], 0, n, writes.data());
        vkCmdDispatch(cmd, std::max(x, 1u), std::max(y, 1u), 1);
        // the next pass reads what this one wrote
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &after, 0, nullptr, 0, nullptr);
    };
    const uint32_t src_x = (e.color.width + 7) / 8, src_y = (e.color.height + 7) / 8;
    const uint32_t dst_x = (e.output.width + 7) / 8, dst_y = (e.output.height + 7) / 8;
    const uint32_t sc_x = (uint32_t(rw * 0.5f) + 7) / 8, sc_y = (uint32_t(rh * 0.5f) + 7) / 8;
    dispatch(kPrepareInputs, src_x, src_y);
    dispatch(kLumaPyramid, spd_x, spd_y);
    dispatch(kShadingChangePyramid, spd_x, spd_y);
    dispatch(kShadingChangePass, sc_x, sc_y);
    dispatch(kPrepareReactivity, src_x, src_y);
    dispatch(kLumaInstability, src_x, src_y);
    dispatch(kAccumulate, dst_x, dst_y);
    // the output for whatever reads it next (the renderer's own barrier moves its layout)
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    s.frame_index = (s.frame_index + 1) % 16;
    return true;
}

}  // namespace gpu::fsr3
