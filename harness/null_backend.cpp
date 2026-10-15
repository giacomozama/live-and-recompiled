// headless renderer: counts frames and draws, logs draw lists, draws chosen frames with the reference renderer
// REF=N: every Nth frame drawn on the CPU into guest memory and saved as cap/ref_<frame>.tga
// CAP=N: every Nth frame's draws listed in cap/draws_<frame>.txt
// snap in the run folder: the next two frames drawn and saved as snap_1.tga and snap_2.tga, then snap is removed
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "harness.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "reference_renderer.h"

namespace gpu {

namespace {

class NullBackend final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        LOGI("renderer: headless %ux%u", w, h);
        std::filesystem::create_directories(app::ExeDir() / "cap");
        return true;
    }
    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        clears_++;
        if (list_) std::fprintf(list_, "CLEAR rt=%08X zs=%08X flags=%X color=%08X z=%g\n", s.rt.data, s.zs.data, flags, color, z);
        if (drawing_) reference::Clear(s, n, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        draws_++;
        if (std::getenv("XBE_DUMP_METHODS") && s.rt.header) {
            uint32_t common = kernel::R32(s.rt.header);
            if ((common & 0x70000) != 0x40000 && (common & 0x70000) != 0x50000) {
                static std::unordered_set<uint32_t> seen;
                if (seen.insert(s.rt.header).second)
                    std::fprintf(stderr, "BADRT hdr=%08X common=%08X data=%08X fmt=%08X vs=%08X prim=%u\n", s.rt.header,
                                 common, kernel::R32(s.rt.header + 4), kernel::R32(s.rt.header + 12), s.vs_handle, prim);
            }
        }
        if (drawing_) reference::Draw(s, prim, start, count, indices);
        if (list_) List(s, prim, start, count);
        if (drawing_ && std::getenv("DUMP_RT") && s.rt.data) rt_headers_[s.rt.data] = s.rt.header;
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        if (drawing_) reference::Copy(src, dst, n, rects, points);
        else CopySurfaceMemory(src, dst, n, rects, points);
    }
    void Present(const Surface& front) override {
        if (std::getenv("XBE_DUMP_METHODS")) {
            static std::unordered_set<uint32_t> seen;
            if (seen.insert(front.data).second)
                LOGI("present front=%08X header=%08X fmt=%X size=%u", front.data, front.header, front.format, front.size);
        }
        if (list_) {
            std::fclose(list_);
            list_ = nullptr;
        }
        if (draws_ == last_draws_) empty_++;
        last_draws_ = draws_;
        if (drawing_ && !snap_) reference::WriteTga(front, File("cap/ref_%05llu.tga", frames_).c_str());
        if (drawing_ && std::getenv("DUMP_RT")) {
            for (auto& [data, header] : rt_headers_) {
                gpu::TextureDesc d;
                bool known = header && gpu::DescribeSurface(gpu::Surface{header, data, 0, 0}, d);
                if (!known && header && !memo_.count(header)) {
                    memo_.insert(header);
                    std::fprintf(stderr, "RTHDR %08X:", header);
                    for (int i = 0; i < 8; i++) std::fprintf(stderr, " %08X", kernel::R32(header + i * 4));
                    std::fprintf(stderr, "\n");
                }
                std::fprintf(stderr, "RTDUMP %08X hdr %08X known=%d %ux%u fmt %X data %08X\n", data, header, int(known),
                             d.width, d.height, d.format, d.data);
            }
            rt_headers_.clear();
        }
        // snap asks for the next two frames drawn
        std::string snap = (app::ExeDir() / "snap").string();
        if (snap_ == 2) {
            reference::WriteTga(front, (app::ExeDir() / "snap_2.tga").string().c_str());
            std::remove(snap.c_str());
            snap_ = 0;
        } else if (snap_ == 1) {
            reference::WriteTga(front, (app::ExeDir() / "snap_1.tga").string().c_str());
            snap_ = 2;
        } else if (FILE* f = std::fopen(snap.c_str(), "r")) {
            std::fclose(f);
            snap_ = 1;
        }
        static const long every = harness::Env("REF"), cap = harness::Env("CAP");
        drawing_ = snap_ || (every && (frames_ + 1) % every == 0);
        if (drawing_) reference::BeginFrame();
        harness::g_frames = frames_ + 1;
        if (++frames_ % 60 == 0) LOGI("renderer: frame %llu, %llu draws, %llu empty frames, %llu clears", frames_, draws_, empty_, clears_);
        if (cap && frames_ % cap == 0) list_ = std::fopen(File("cap/draws_%05llu.txt", frames_).c_str(), "w");
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    static std::string File(const char* fmt, unsigned long long n) {
        char name[64];
        std::snprintf(name, sizeof name, fmt, n);
        return (app::ExeDir() / name).string();
    }

    // one draw's state, textures, transforms, declaration and first vertex
    void List(const State& s, uint32_t prim, uint32_t start, uint32_t count) {
        FILE* f = list_;
        std::fprintf(f, "DRAW prim=%u n=%u vs=%X ps=%X rt=%08X rthdr=%08X tex0=%08X light=%u blend=%u %X %X cw=%X at=%u z=%u nvtex0=%08X nvfmt0=%08X nvctl0=%08X\n", prim, count,
                     s.vs_handle, s.ps ? s.ps->handle : 0, s.rt.data, s.rt.header, s.textures[0], s.rs[kRsLighting], s.simple[kMBlendEnable >> 2],
                     s.simple[kMSrcBlend >> 2], s.simple[kMDestBlend >> 2], s.simple[kMColorWriteEnable >> 2],
                     s.simple[kMAlphaTestEnable >> 2], s.rs[kRsZEnable], s.simple[0x1B00 >> 2], s.simple[0x1B04 >> 2], s.simple[0x1B0C >> 2]);
        const auto& t0 = s.tss[0];
        TextureDesc d;
        if (s.textures[0] && Describe(s.textures[0], d)) {
            std::fprintf(f, "  tex %ux%u fmt %X data %08X cop %u %X %X aop %u %X %X\n", d.width, d.height, d.format, d.data, t0[kTssColorOp],
                         t0[kTssColorArg1], t0[kTssColorArg2], t0[kTssAlphaOp], t0[kTssAlphaArg1], t0[kTssAlphaArg2]);
            if (std::getenv("DUMP_TEX")) {
                static std::unordered_set<uint32_t> done;
                if (done.insert(s.textures[0]).second)
                    reference::DumpTexture(s.textures[0], File("cap/tex_%08X.tga", s.textures[0]).c_str());
            }
        }
        std::fprintf(f, "  ttf %X tci %X addr %u %u t16", t0[kTssTextureTransformFlags], t0[kTssTexCoordIndex], t0[kTssAddressU],
                     t0[kTssAddressV]);
        auto matrix = [&](const char* name, uint32_t which) {
            std::fprintf(f, "%s", name);
            for (float v : s.transforms[which]) std::fprintf(f, " %g", v);
        };
        matrix("", 16);
        matrix("\n  world", 256);
        matrix("\n  view", 2);
        matrix("\n  proj", 3);
        std::fprintf(f, "\n");
        if (s.vs) {
            std::fprintf(f, "  const");
            for (int r = 96; r < 108; r++) {
                std::fprintf(f, " c%d", r - 96);
                for (int c = 0; c < 4; c++) std::fprintf(f, " %g", s.vsc[r][c]);
            }
            std::fprintf(f, "\n  decl");
            for (int r = 0; r < 16; r++) {
                const auto& in = s.vs->inputs[size_t(r)];
                if (in.stream >= 0) std::fprintf(f, " v%d:%d+%u/%X", r, in.stream, in.offset, in.type);
            }
            std::fprintf(f, "\n");
        }
        if (s.streams[0].vb) {
            // the vertex buffer's Data at +4 is a physical address. Print the stride the fetch actually uses: the
            // declaration's when the bound one cannot cover it, which is the case for the title's shader draws.
            uint32_t stride = s.streams[0].stride;
            uint32_t phys = kernel::R32(s.streams[0].vb + 4), data = mem::VirtualFromPhysical(phys) + start * stride;
            std::fprintf(f, "  v0 stride %u vb=%08X phys=%08X data=%08X:", stride, s.streams[0].vb, phys, data);
            for (uint32_t i = 0; i < stride / 4 && i < 10; i++) {
                uint32_t v = kernel::R32(data + i * 4);
                float x;
                std::memcpy(&x, &v, 4);
                std::fprintf(f, " %g(%08X)", x, v);
            }
            std::fprintf(f, "\n");
        }
    }

    unsigned long long frames_ = 0, draws_ = 0, last_draws_ = 0, empty_ = 0, clears_ = 0;
    std::unordered_map<uint32_t, uint32_t> rt_headers_;
    std::unordered_set<uint32_t> memo_;
    bool drawing_ = false;
    int snap_ = 0;
    FILE* list_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }

}  // namespace gpu
