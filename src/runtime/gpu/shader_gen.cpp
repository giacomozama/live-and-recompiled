// Shader records from the disc's static push buffers, see shader_gen.h
#include "gpu/shader_gen.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/disc_programs.h"
#include "gpu/draw.h"
#include "gpu/shader_record.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

namespace gpu::shader_gen {

namespace {

namespace sr = shader_record;

// D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA and D3DBLENDOP_ADD as the chip's blend methods take them
constexpr uint32_t kBlendSrcAlpha = 0x302, kBlendInvSrcAlpha = 0x303, kBlendOpAdd = 0x8006;

// a stage's TexInfo for a texture of the title's own (not a render target: the blocks sample none), as
// VulkanBackend::BindTexture and BindGuestTexture fill it. A depth format in a block is a stand-in the title patches
// with the draw's own texture before it runs (a model's stage 0 at a fixed address): an ordinary swizzled 2D texture,
// as the recorded draws of those blocks have it
TexInfo GuestTexInfo(const State& s, int stage, const TextureDesc& d) {
    TexInfo info;
    if (Format(d.format).depth) {
        info.kind = kTex2D;
        return info;
    }
    info.kind = TextureKind(d);
    info.rect = d.linear;
    if (Format(d.format).depth) info.shadow = ShadowDepthKind(d.format);
    info.sign = OffsetSigned(d.format, s.tss[size_t(stage)][kTssColorSign]);
    return info;
}

// Draws nothing: each draw's records as VulkanBackend::Draw makes them, kept once each in the order first made
class Recorder final : public Backend {
public:
    explicit Recorder(const Context& ctx) : ctx_(ctx) {}

    bool Init(uint32_t, uint32_t) override { return true; }
    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t) override {}
    void Present(const Surface&) override {}
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

    void Draw(const State& s, uint32_t prim, uint32_t /*start*/, uint32_t count, const uint8_t* /*indices*/) override {
        draws_++;
        if (FixedSprite(s, prim)) return;  // point sprites: none in the blocks
        const bool patches = ctx_.tess && TessellatedDraw(s, prim);
        Assembled& a = asm_;
        if (!AssembleLayout(s, prim, a, patches)) return;

        ShaderChoice sh;
        ClassifyShaders(s, a, count, sh);
        if (sh.prog) EnsureProgramHash(s);
        sh.shading = DrawShading(s, sh);
        const sr::VertexEntry v = MakeVertexEntry(s, a, sh);
        sh.vid = Add(sr::kVertex, &v, sizeof(v));

        // the stages as VulkanBackend::BindTexture keys them: those the pixel shader reads (DrawTextures::kept)
        TexInfo info[4] = {};
        const uint32_t sampled = SampledStages(s) | ShadingStages(sh.shading);
        for (int i = 0; i < 4; i++) {
            TextureDesc d;
            if (((sampled >> i) & 1) && s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d))
                info[i] = GuestTexInfo(s, i, d);
        }
        PixelKey pk = MakePixelKey(s, info);
        pk.quad = sh.quad;
        pk.shading = sh.shading;
        sh.pid = Add(sr::kPixel, &pk, sizeof(pk));

        // VulkanBackend::BuildPipelineKey, the frame's targets bound
        PsoKey k;
        std::memset(&k, 0, sizeof(k));
        k.regs = a.regs;
        for (int r = 0; r < 16; r++) {
            if ((a.regs >> r) & 1) {
                const VertexElement& e = a.elements[size_t(r)];
                k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
            }
        }
        k.topo = uint32_t(a.topology);
        k.has_rt = 1;
        k.has_ds = 1;
        k.rtfmt = ctx_.rtfmt;
        k.samples = ctx_.samples;
        KeyBlend(s, k);
        KeyDepthStencil(s, a, k);
        KeyRasterizer(s, a, k);

        // the color write mask's renderer bits (VulkanBackend::Draw): ProgramShading::unsharpened's alpha mark while
        // the frame is sharpened or blurred (settings, and the depth of field lately), coarse's shading rate on its
        // shells (Adaptive fur shading's, the device's): each way it can be
        const ProgramShading* zps = sh.shading ? &g_program_shading[sh.shading - 1] : nullptr;
        std::vector<uint32_t> masks{k.wmask};
        if (zps && zps->unsharpened && (k.wmask & 7)) masks.push_back(k.wmask | 8 | kWmaskAlphaMax);
        if (zps && zps->coarse && ctx_.vrs)
            for (size_t i = 0, n = masks.size(); i < n; i++) {
                masks.push_back(masks[i] | kWmaskCoarseWide);
                masks.push_back(masks[i] | kWmaskCoarseWide | kWmaskCoarseTall);
            }
        for (uint32_t mask : masks) {
            PsoKey km = k;
            km.wmask = mask;
            AddDraw(km, sh, pk, zps);
            // an opaque material faded: the title's code blends a model it fades in or out over what lies behind
            // (source alpha, add), its blocks as they are
            if (!km.blend && km.has_rt) {
                km.blend = 1;
                km.src = kBlendSrcAlpha;
                km.dst = kBlendInvSrcAlpha;
                km.op = kBlendOpAdd;
                AddDraw(km, sh, pk, zps);
            }
        }
    }

    std::vector<sr::Entry> Take() { return std::move(entries_); }
    size_t Draws() const { return draws_; }

private:
    // ProgramShading::depth_alpha and surface_alpha: the depth in a pass of its own (VulkanBackend::Draw). And
    // g_surface_depth's, which takes a draw by where its vertices lie: any it might take, made both ways
    void AddDraw(PsoKey k, const ShaderChoice& sh, const PixelKey& pk, const ProgramShading* zps) {
        float zalpha = 0;
        if (zps && zps->depth_alpha > 0 && k.blend && k.zen && k.zwrite) zalpha = zps->depth_alpha;
        else if (zps && zps->surface_alpha > 0 && k.blend && k.zen && !k.zwrite) zalpha = zps->surface_alpha;
        else if (!zps && k.blend && k.zen && !k.zwrite && g_surface_depth.takes &&
                 (!g_surface_depth.on || *g_surface_depth.on)) {
            AddPipeline(k, sh);
            zalpha = g_surface_depth.alpha;
        }
        if (zalpha > 0) {
            PixelKey zpk = pk;
            zpk.depth_pass = std::bit_cast<uint32_t>(zalpha);
            ShaderChoice zsh = sh;
            zsh.pid = Add(sr::kPixel, &zpk, sizeof(zpk));
            PsoKey zk = k;
            zk.blend = 0;
            zk.wmask = k.wmask & (kWmaskCoarseWide | kWmaskCoarseTall);
            zk.sten = 0;
            zk.zwrite = 1;
            AddPipeline(zk, zsh);
            k.zwrite = 0;
        }
        AddPipeline(k, sh);
    }

    // its key PipelineKey's (MakePipelineEntry): the culling, depth and stencil the title's code and patches give
    // each object are the draw's dynamic state, not the pipeline's
    void AddPipeline(const PsoKey& k, const ShaderChoice& sh) {
        const PipelineEntry e = MakePipelineEntry(k, sh);
        Add(sr::kPipeline, &e, sizeof(e));
    }

    uint64_t Add(sr::Kind kind, const void* p, size_t n) {
        const uint64_t id = sr::Id(kind, p, n);
        if (ids_.insert(id).second) {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            entries_.push_back({kind, id, std::vector<uint8_t>(b, b + n)});
        }
        return id;
    }

    Context ctx_;
    Assembled asm_;
    std::unordered_set<uint64_t> ids_;
    std::vector<sr::Entry> entries_;
    size_t draws_ = 0;
};

// What the title's code sets around its static push buffers, which they don't set themselves and a pipeline keeps:
// the color write mask, red, green and blue, or with alpha too (the depth test and stencil it sets are the draws'
// dynamic state, PipelineKey)
constexpr uint32_t kAroundMasks[] = {0x00010101, 0x01010101};

void SetAround(State& s, uint32_t color_mask) {
    s.zscale = 16777215.0f;  // a D24S8 depth buffer
    s.rs.set(kRsZEnable, 1u);
    s.simple.set(kMZFunc >> 2, 0x203u);  // LEQUAL
    s.simple.set(kMZWriteEnable >> 2, 1u);
    s.simple.set(kMColorWriteEnable >> 2, color_mask);
}

constexpr uint32_t kNopHeader = 0x42400100;  // NV097_NO_OPERATION, 144 times, not incrementing
constexpr uint32_t kNopWords = 144;

// the blocks in a package's data: from each run of 144 no-operations to the next (or the data's end)
std::vector<std::pair<size_t, size_t>> Blocks(const std::vector<uint8_t>& data) {
    std::vector<size_t> starts;
    const size_t words = data.size() / 4;
    auto word = [&](size_t i) {
        uint32_t w;
        std::memcpy(&w, &data[i * 4], 4);
        return w;
    };
    for (size_t i = 0; i + 1 + kNopWords <= words; i++) {
        if (word(i) != kNopHeader) continue;
        bool zeros = true;
        for (size_t j = 1; zeros && j <= kNopWords; j++) zeros = word(i + j) == 0;
        if (zeros) starts.push_back(i * 4);
    }
    std::vector<std::pair<size_t, size_t>> blocks;
    for (size_t i = 0; i < starts.size(); i++)
        blocks.push_back({starts[i], (i + 1 < starts.size() ? starts[i + 1] : words * 4) - starts[i]});
    return blocks;
}

// A block's ways to run: some set up their vertex arrays and program more than one way, each setup behind a
// no-operation the title patches into a jump over the others (one per pass: a model's shadow, its body), the
// materials and draws after them shared. Each way is the block with one setup alone; a block with one is itself
std::vector<std::vector<uint8_t>> Ways(const uint8_t* block, size_t size) {
    auto word = [&](size_t at) {
        uint32_t w;
        std::memcpy(&w, block + at, 4);
        return w;
    };
    // a setup: a no-operation, then only vertex arrays, program and constant methods
    auto setup_method = [](uint32_t m) {
        return (m >= 0x1720 && m < 0x17A0) || m == 0x1E9C || m == 0x1EA0 || m == 0x1EA4 || (m >= 0xB00 && m < 0xC00);
    };
    std::vector<size_t> starts;
    size_t end = 0, at = (1 + kNopWords) * 4;
    while (at + 4 <= size) {
        const uint32_t h = word(at);
        if (h == 0) {
            at += 4;
            continue;
        }
        if (h & 0xA0030003) break;  // a jump, call or anything but a method header: the walk stops there too
        const uint32_t m = h & 0x1FFC, n = (h >> 18) & 0x7FF;
        const size_t next = at + 4 + size_t(n) * 4;
        if (!n || next > size) break;
        if (h == 0x00040100 && next + 4 <= size && ((word(next) & 0x1FFC) >= 0x1720 && (word(next) & 0x1FFC) < 0x1760)) {
            starts.push_back(at);
        } else if (!starts.empty() && !setup_method(m)) {
            end = at;  // the materials begin
            break;
        }
        at = next;
    }
    std::vector<std::vector<uint8_t>> ways;
    if (starts.size() < 2 || !end) {
        ways.emplace_back(block, block + size);
        return ways;
    }
    starts.push_back(end);
    for (size_t i = 0; i + 1 < starts.size(); i++) {
        std::vector<uint8_t> w(block, block + starts[0]);
        w.insert(w.end(), block + starts[i], block + starts[i + 1]);
        w.insert(w.end(), block + end, block + size);
        ways.push_back(std::move(w));
    }
    return ways;
}

// A way's materials: a model's draws may come after several materials, each opened by a no-operation the title
// patches into a jump to the draws (one per pass: its base, a glow over it), so only the last is in effect in the
// block as it lies. Each earlier one is the way cut after it and joined again at the draws (the no-operations ahead
// of the draws that follow it); the way itself is the last's
std::vector<std::vector<uint8_t>> MaterialWays(std::vector<uint8_t> way) {
    struct Packet {
        size_t at;
        uint32_t head;
        uint32_t first;  // its first value, 0 without one
    };
    std::vector<Packet> packets;
    for (size_t at = 0; at + 4 <= way.size();) {
        uint32_t h;
        std::memcpy(&h, &way[at], 4);
        if (h & 0xA0030003) break;
        const uint32_t n = (h >> 18) & 0x7FF;
        if (h && (!n || at + 4 + size_t(n) * 4 > way.size())) break;
        uint32_t first = 0;
        if (n) std::memcpy(&first, &way[at + 4], 4);
        packets.push_back({at, h, first});
        at += 4 + size_t(n) * 4;
    }
    auto method = [&](size_t i) { return packets[i].head & 0x1FFC; };
    auto nop = [&](size_t i) { return packets[i].head == 0x00040100 && packets[i].first == 0; };
    auto draw = [&](size_t i) { return method(i) == 0x17FC && packets[i].first != 0; };
    std::vector<std::vector<uint8_t>> ways;
    bool material = false;  // one since the last draw
    for (size_t i = 0; i + 1 < packets.size(); i++) {
        if (draw(i)) material = false;
        if (!nop(i) || method(i + 1) != 0x1B0C) continue;  // a material opens: a no-operation, stage 0 turned off
        if (material) {
            // the draws after it: from the no-operations right ahead of the next draw
            size_t d = i + 1;
            while (d < packets.size() && !draw(d)) d++;
            if (d < packets.size()) {
                size_t r = d;
                while (r > i + 1 && nop(r - 1)) r--;
                std::vector<uint8_t> w(way.begin(), way.begin() + ptrdiff_t(packets[i].at));
                w.insert(w.end(), way.begin() + ptrdiff_t(packets[r].at), way.end());
                ways.push_back(std::move(w));
            }
        }
        material = true;
    }
    ways.push_back(std::move(way));
    return ways;
}

}  // namespace

int Run(const std::filesystem::path& out, const Context& ctx) {
    namespace dp = disc_programs;
    const auto t = std::chrono::steady_clock::now();
    std::vector<dp::File> files;
    if (const kernel::Disc* disc = kernel::MountedDisc()) files = dp::DiscFiles(*disc);
    else files = dp::FolderFiles(app::GameDir());
    std::sort(files.begin(), files.end(), [](const dp::File& a, const dp::File& b) { return a.path < b.path; });

    // a block in guest memory for the walk, the buffer grown as needed
    constexpr uint32_t kMaxBlock = 64u << 20;
    uint32_t buffer = 0, buffer_size = 0;
    std::unique_ptr<Backend> rec = std::make_unique<Recorder>(ctx);
    size_t packages = 0, blocks = 0, ways = 0, skipped = 0;
    std::vector<uint8_t> file, data, head;
    for (const dp::File& f : files) {
        if (!f.read(head, 4) || head.size() < 4 || std::memcmp(head.data(), "CAFF", 4)) continue;
        if (!f.read(file, SIZE_MAX) || !dp::UnpackCaff(file, data)) continue;
        packages++;
        for (auto [at, size] : Blocks(data)) {
            if (size > kMaxBlock) {
                skipped++;
                continue;
            }
            if (size > buffer_size) {
                if (buffer) mem::FreeContiguous(buffer);
                buffer_size = std::max<uint32_t>(uint32_t(size), 1u << 20);
                buffer = mem::AllocContiguous(buffer_size, 0, 0xFFFFFFFF, 0x1000);
                if (!buffer) {
                    LOGE("shader_gen: no guest memory for a block of %zu bytes", size);
                    return 1;
                }
            }
            for (std::vector<uint8_t>& setup : Ways(&data[at], size)) {
                for (const std::vector<uint8_t>& way : MaterialWays(std::move(setup))) {
                    std::memcpy(kernel::HostPtr(buffer), way.data(), way.size());
                    for (uint32_t mask : kAroundMasks)
                        d3d::DryWalk(rec, [mask](State& s) { SetAround(s, mask); }, true, buffer, uint32_t(way.size()));
                    ways++;
                }
            }
            blocks++;
        }
    }
    auto& r = static_cast<Recorder&>(*rec);
    const size_t draws = r.Draws();
    std::vector<sr::Entry> entries = r.Take();
    size_t kinds[7] = {};
    for (const sr::Entry& e : entries) kinds[e.kind]++;
    const bool ok = sr::WriteRecords(out, Stamp(ctx), entries);
    LOGR("shader_gen: %zu packages, %zu blocks (%zu ways, %zu too big), %zu draws: %zu vertex shaders, %zu pixel "
         "shaders, %zu pipelines in %.0f ms%s",
         packages, blocks, ways, skipped, draws, kinds[sr::kVertex], kinds[sr::kPixel], kinds[sr::kPipeline],
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(),
         ok ? "" : ", not written");
    return ok ? 0 : 1;
}

// what a record made here means changed (a new way of reading the blocks): bump to make them again
constexpr uint32_t kGenRevision = 2;  // 2: faded opaque materials
constexpr const char* kFileName = "disc_shaders.rec";

uint64_t Stamp(const Context& ctx) {
    const uint32_t v[] = {kGenRevision, ctx.rtfmt, ctx.samples, ctx.tess, ctx.vrs, uint32_t(app::Get().fog)};
    uint64_t h = Hash(v, sizeof(v), RecordLayout());
    // which shading entries are on (DrawShading picks the first that is, so the keys follow) and whether
    // g_surface_depth takes draws: the game's own settings (super fur, per-pixel lighting, dynamic water, ...)
    std::vector<uint8_t> on;
    for (const ProgramShading& e : g_program_shading) on.push_back(!e.on || *e.on);
    on.push_back(g_surface_depth.takes && (!g_surface_depth.on || *g_surface_depth.on));
    return Hash(on.data(), on.size(), h);
}

bool Load(const Context& ctx, bool make, const std::atomic<bool>& stop, std::vector<sr::Entry>& out) {
    const std::filesystem::path path = app::CacheDir() / kFileName;
    const uint64_t stamp = Stamp(ctx);
    if (sr::ReadRecords(path, stamp, out)) return true;
    if (!make || app::Instance() > 1) return false;
    const auto t = std::chrono::steady_clock::now();
    const std::u8string p8 = path.u8string();
    std::vector<std::string> args{"--make-shader-records", std::string(p8.begin(), p8.end()), "--gen-rtfmt",
                                  std::to_string(ctx.rtfmt), "--gen-samples", std::to_string(ctx.samples)};
    if (ctx.tess) args.push_back("--gen-tess");
    if (ctx.vrs) args.push_back("--gen-vrs");
    const int code = app::RunSelf(args, &stop);
    const bool ok = code == 0 && sr::ReadRecords(path, stamp, out);
    LOGR("shaders: the disc's shader records %s in %.0f ms (%zu)", ok ? "made" : "not made, see game-shadergen.log",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(), out.size());
    return ok;
}

}  // namespace gpu::shader_gen
