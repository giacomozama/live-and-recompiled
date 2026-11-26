// The OpenGL renderer's HLSL to GLSL (opengl_glsl.h): glslang, then SPIRV-Cross
#include "gpu/opengl_glsl.h"

#include "gpu/post_hlsl.h"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_glsl.hpp>

#include <cstring>
#include <mutex>
#include <utility>

namespace gpu {
namespace {

EShLanguage LanguageOf(char profile) {
    switch (profile) {
        case 'v': return EShLangVertex;
        case 'g': return EShLangGeometry;
        case 'h': return EShLangTessControl;
        case 'd': return EShLangTessEvaluation;
        default: return EShLangFragment;
    }
}

}  // namespace

void GlslInit() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

bool HlslToGlsl(const std::string& src, const char* entry, const char* profile, uint32_t version, std::string& glsl,
                std::vector<Bind>& binds, std::string& err, bool flip) {
    const EShLanguage stage = LanguageOf(profile[0]);
    glslang::TShader sh(stage);
    const char* text = src.c_str();
    int len = int(src.size());
    sh.setStringsWithLengths(&text, &len, 1);
    sh.setEntryPoint("main");
    sh.setSourceEntryPoint(entry);
    sh.setPreamble("#define VK_LOC(n) [[vk::location(n)]]\n");
    sh.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapLocations(true);
    const auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, msgs)) {
        err = sh.getInfoLog();
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs) || !prog.mapIO()) {
        err = prog.getInfoLog();
        return false;
    }
    std::vector<uint32_t> spirv;
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;
    glslang::GlslangToSpv(*prog.getIntermediate(stage), spirv, &opt);
    if (spirv.empty()) {
        err = "no SPIR-V";
        return false;
    }
    try {
        spirv_cross::CompilerGLSL c(std::move(spirv));
        auto o = c.get_common_options();
        o.version = version;
        o.es = false;
        o.vulkan_semantics = false;
        o.enable_420pack_extension = true;
        // GLSL before 4.10 names stage inputs and outputs by location only with the extension
        o.separate_shader_objects = version < 410;
        // depth 0 to 1 and the rows are glClipControl's, without it the last vertex stage's (a geometry shader's
        // below: SPIRV-Cross fixes up at the entry point's end only, past its vertices)
        o.vertex.fixup_clipspace = flip && (stage == EShLangVertex || stage == EShLangTessEvaluation);
        o.vertex.flip_vert_y = o.vertex.fixup_clipspace;
        c.set_common_options(o);
        const std::string suffix = std::string("_") + profile[0];
        const auto resources = c.get_shader_resources();
        for (const auto& ub : resources.uniform_buffers)
            c.set_name(ub.base_type_id, c.get_name(ub.base_type_id) + suffix);
        const uint32_t dummy = c.build_dummy_sampler_for_combined_images();
        c.build_combined_image_samplers();
        binds.clear();
        for (const auto& r : c.get_combined_image_samplers()) {
            const uint32_t t = c.get_decoration(r.image_id, spv::DecorationBinding);
            const uint32_t s = dummy && r.sampler_id == dummy ? kNoSampler
                                                              : c.get_decoration(r.sampler_id, spv::DecorationBinding);
            const int unit = Unit(t, s);
            if (unit < 0) {
                err = "texture t" + std::to_string(t) + " with sampler s" + std::to_string(s) + " has no unit";
                return false;
            }
            const auto& type = c.get_type_from_variable(r.combined_id);
            uint32_t target = kGlTexture2D;
            if (type.image.dim == spv::DimCube) target = kGlTextureCube;
            else if (type.image.dim == spv::Dim3D) target = kGlTexture3D;
            else if (type.image.ms) target = kGlTexture2DMs;
            c.set_decoration(r.combined_id, spv::DecorationBinding, uint32_t(unit));
            c.set_name(r.combined_id, "u" + std::to_string(unit) + "_" + std::to_string(r.combined_id) + suffix);
            binds.push_back({uint8_t(unit), uint8_t(t), uint8_t(s), 0, target});
        }
        glsl = c.compile();
        if (flip && stage == EShLangGeometry) {
            const std::string emit = "EmitVertex();";
            const std::string fixup = "gl_Position.z = 2.0 * gl_Position.z - gl_Position.w; "
                                      "gl_Position.y = -gl_Position.y; ";
            for (size_t at = 0; (at = glsl.find(emit, at)) != std::string::npos; at += fixup.size() + emit.size())
                glsl.insert(at, fixup);
        }
    } catch (const std::exception& e) {
        err = std::string("SPIRV-Cross: ") + e.what();
        return false;
    }
    return true;
}

const std::string& GlDepthBytesMultisampled() {
    static const std::string source = [] {
        std::string s = std::string("#define DEPTH_MULTISAMPLED\n") + kDepthBytesHlsl;
        const std::pair<const char*, const char*> edits[] = {
            {"sten.Load(p,0).y", "sten.Load(p,0).x"},
            {"cbuffer B : register(b1) { float4 order; float4 fur; };",
             "cbuffer B : register(b1) { float4 order; float4 fur; float4 gl_size; };"},
            {"uint2 DepthSize() { uint2 s; uint samples; img.GetDimensions(s.x,s.y,samples); return s; }",
             "uint2 DepthSize() { return uint2(gl_size.xy); }"},
        };
        for (const auto& [from, to] : edits)
            if (auto at = s.find(from); at != std::string::npos) s.replace(at, std::strlen(from), to);
        return s;
    }();
    return source;
}

}  // namespace gpu
