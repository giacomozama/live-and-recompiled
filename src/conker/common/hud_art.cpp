// HUD art redrawn as vectors (settings.toml redrawn_hud): src/conker/common/hud_art/svg/<name>.svg, each traced from the
// game's texture of that name (globaleffects/sights/vertexSight), rasterized by tools/hud_art.py into the PNGs under
// hud_art/ (8 to 16 times the texture's size), which art.inc embeds with the name and size of the texture each stands in
// for. The renderer draws the picture in that texture's place (gpu::Replacement), so the game's draws, their sizes and
// texture coordinates stay its own.
//
// The textures are found by name in the resource table 0x84D458, which holds every resource the game loaded by path,
// a package's files among them (see menus.cpp's ResourcePath): 0x65 buckets of 8 bytes from +8, each the first entry's
// link; an entry has its path at +8 (aid\texture\<name>\default.rba and the like), its link at +0x10 (the next
// entry's link at +0x14) and its resource at +0x18, whose D3D texture is at +0x24. A few times a second the table is
// walked: each texture found is tagged for the renderer with its texels' hash then, one gone (or whose texels changed)
// untagged or tagged again; with the setting off, every one untagged.
//
// A picture whose SVG canvas reaches past the texture's square has a second picture of the whole canvas
// (<name>.full.png, with so many texels past the square on each side). Where a sprite routine draws the texture (the
// kSpriteDraws hooks, DrawVerticesUP of the sprite's quad, built on the stack each draw), the quad is drawn larger by
// that much, from a copy of its vertices, the square where it was, its texture coordinates as they were, and with a
// stand-in texture of ours tagged with the larger picture (d3d::TextureForNextDraw). The screen quads of 0x15D280
// (vertexSight's, 48-byte vertices) are drawn so too, and the HUD's quads of 0x1CAD90 (boostMarker's among them),
// which go into the push buffer, by their corners (HudQuad). Any other draw of the texture has the square's picture. The
// DrawVerticesUP quads (padded or not) are also narrowed about their middle against the widescreen stretch of the 4:3 frame, as the
// multiplayer HUD's split does.
#include "common/hud_art.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <cmath>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/diag.h"
#include "core/log.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "cpu/x86_context.h"
#include "xdk/d3d8.h"
#include "app/app.h"
#include "gpu/shaders.h"

#if __has_include(<SDL3/SDL.h>) && !defined(XBE_HEADLESS)
#include <SDL3/SDL.h>
#define HUD_ART_PICTURES 1
#endif

#ifdef HUD_ART_PICTURES

namespace {

struct Art {
    const char* name;  // the texture's, its path's parts by backslashes
    uint32_t w, h;    // the game's texture
    uint32_t pw, ph;  // its picture
    const unsigned char* png;
    size_t size;
    const unsigned char* full_png;  // the whole canvas, null where it is the square
    size_t full_size;
    uint32_t pad[4];  // texels of the full picture past the square: left, top, right, bottom
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#include "hud_art/art.inc"
#pragma clang diagnostic pop

constexpr size_t kCount = sizeof(kArt) / sizeof(kArt[0]);
int g_index[kCount];  // each picture's gpu::AddReplacement index
int g_full_index[kCount];  // its full picture's, -1 none
constexpr int kFull = 0x10000;  // a ReplacementSource id: the full picture of id - kFull

// the picture's PNG as BGRA texels (gpu::ReplacementSource)
bool Decode(int id, std::vector<uint8_t>& out, uint32_t& w, uint32_t& h) {
    const bool full = id >= kFull;
    const Art& s = kArt[full ? id - kFull : id];
    SDL_Surface* loaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(full ? s.full_png : s.png, full ? s.full_size : s.size), true);
    if (!loaded) {
        LOGR("hud art: %s does not decode: %s", s.name, SDL_GetError());
        return false;
    }
    SDL_Surface* bgra = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);  // B, G, R, A in memory
    SDL_DestroySurface(loaded);
    if (!bgra) return false;
    w = uint32_t(bgra->w);
    h = uint32_t(bgra->h);
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        std::memcpy(&out[size_t(y) * w * 4], static_cast<const uint8_t*>(bgra->pixels) + size_t(y) * size_t(bgra->pitch),
                    size_t(w) * 4);
    SDL_DestroySurface(bgra);
    return true;
}

// the picture a resource path names (...\<name>, then its end or a further part or extension; any case, either
// slash), -1 none
int ArtOfPath(std::string_view path) {
    auto same = [](char a, char b) {
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    for (size_t s = 0; s < kCount; s++) {
        const std::string_view name = kArt[s].name;
        for (size_t at = 0; at + name.size() <= path.size(); at++) {
            if (at && !same(path[at - 1], '\\')) continue;
            size_t k = 0;
            while (k < name.size() && same(path[at + k], name[k])) k++;
            if (k != name.size()) continue;
            const size_t end = at + k;
            if (end == path.size() || path[end] == '\\' || path[end] == '/' || path[end] == '.') return int(s);
        }
    }
    return -1;
}

struct Tagged {
    int art;
    uint64_t hash;
};
std::unordered_map<uint32_t, Tagged> g_tagged;  // by the texture's data; game thread
std::unordered_map<uint32_t, uint32_t> g_headers;  // the textures found: their D3D texture to their data; game thread
// each table entry's picture by its path's address, matched once: the table holds thousands of resources, and matching
// every path against every name each walk took the game thread long enough to miss frames (a multiplayer match at 120)
struct Known {
    uint32_t path;
    int art;
    uint32_t walk;  // the last walk that saw it
};
std::unordered_map<uint32_t, Known> g_known;  // by the entry; game thread

void UntagAll() {
    for (const auto& [data, t] : g_tagged) gpu::TagReplacement(data, -1, 0, 0, 0, 0);
    g_tagged.clear();
    g_headers.clear();
    g_known.clear();
}

void Walk() {
    if (!hud_art::g_on) {
        if (!g_tagged.empty()) UntagAll();
        return;
    }
    constexpr uint32_t kTable = 0x84D458, kBuckets = 0x65, kLinkNext = 4, kEntryPath = 8, kEntryLink = 0x10;
    constexpr uint32_t kEntryResource = 0x18, kResourceTexture = 0x24;
    static bool warned[kCount] = {};
    static uint32_t walk = 0;
    walk++;
    std::unordered_set<uint32_t> seen;
    g_headers.clear();
    int left = 50000;  // links walked at most
    for (uint32_t b = 0; b < kBuckets; b++) {
        for (uint32_t link = kernel::R32(kTable + 8 + b * 8); link && left-- > 0; link = kernel::R32(link + kLinkNext)) {
            if (!mem::IsCommitted(link) || !mem::IsCommitted(link + 7)) break;
            const uint32_t entry = link - kEntryLink;
            const uint32_t path = kernel::R32(entry + kEntryPath);
            Known& known = g_known[entry];
            if (known.walk == 0 || known.path != path) {
                char text[256];
                size_t n = 0;
                for (; path && n + 1 < sizeof(text) && mem::IsCommitted(path + uint32_t(n)); n++) {
                    text[n] = char(mem::Base()[path + n]);
                    if (!text[n]) break;
                }
                known.path = path;
                known.art = ArtOfPath(std::string_view(text, n));
            }
            known.walk = walk;
            const int s = known.art;
            if (s < 0) continue;
            const uint32_t res = kernel::R32(entry + kEntryResource);
            gpu::TextureDesc d;
            if (!res || !mem::IsCommitted(res + kResourceTexture) || !gpu::Describe(res + kResourceTexture, d)) continue;
            const Art& art = kArt[s];
            if (d.width != art.w || d.height != art.h || d.cube || d.depth != 1 || !d.data) {
                if (!warned[s]) LOGR("hud art: %s is %ux%u, its picture drawn for %ux%u: left as the game's", art.name,
                                     d.width, d.height, art.w, art.h);
                warned[s] = true;
                continue;
            }
            const uint32_t bytes = gpu::LevelSize(d, 0);
            if (!mem::IsCommitted(d.data) || !mem::IsCommitted(d.data + bytes - 1)) continue;
            const uint64_t hash = gpu::TextureHash(mem::Base() + d.data, bytes);
            seen.insert(d.data);
            g_headers[res + kResourceTexture] = d.data;
            const auto it = g_tagged.find(d.data);
            if (it != g_tagged.end() && it->second.art == s && it->second.hash == hash) continue;
            g_tagged[d.data] = {s, hash};
            gpu::TagReplacement(d.data, g_index[s], d.format, d.width, d.height, hash);
        }
    }
    for (auto it = g_known.begin(); it != g_known.end();)  // entries gone from the table
        it = it->second.walk != walk ? g_known.erase(it) : std::next(it);
    for (auto it = g_tagged.begin(); it != g_tagged.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        gpu::TagReplacement(it->first, -1, 0, 0, 0, 0);
        it = g_tagged.erase(it);
    }
}

// the stand-in texture the full picture of art s is drawn with: a 4x4 A8R8G8B8 of ours, tagged with that picture
uint32_t StandIn(int s) {
    static uint32_t made[kCount] = {};
    if (made[s] || g_full_index[s] < 0) return made[s];
    constexpr uint32_t kTextureCommon = 0x00040001, kA8R8G8B8 = 0x06, kSide = 4, kLog2 = 2;
    const uint32_t bytes = kSide * kSide * 4;
    const uint32_t data = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 0x1000);
    const uint32_t header = data ? mem::AllocSystem(20) : 0;
    if (!header) return 0;
    std::memset(mem::Base() + data, 0, bytes);
    // D3DPixelContainer: Common, Data (physical), Lock, Format (DMA channel A, two dimensions, the format, one level,
    // log2 sizes), Size (0: swizzled)
    uint32_t* hdr = reinterpret_cast<uint32_t*>(mem::Base() + header);
    hdr[0] = kTextureCommon;
    hdr[1] = mem::PhysicalFromVirtual(data);
    hdr[2] = 0;
    hdr[3] = 0x01 | 2u << 4 | kA8R8G8B8 << 8 | 1u << 16 | kLog2 << 20 | kLog2 << 24;
    hdr[4] = 0;
    gpu::TagReplacement(data, g_full_index[s], kA8R8G8B8, kSide, kSide, gpu::TextureHash(mem::Base() + data, bytes));
    return made[s] = header;
}

}  // namespace

bool hud_art::g_on;

void hud_art::Init() {
    for (size_t i = 0; i < kCount; i++) {
        g_index[i] = gpu::AddReplacement(Decode, int(i));
        g_full_index[i] = kArt[i].full_png ? gpu::AddReplacement(Decode, kFull + int(i)) : -1;
    }
}

void hud_art::QuadDraw(X86Context& ctx, int uv_offset) {
    if (!g_on || g_headers.empty()) return;
    uint32_t tex;
    {
        std::lock_guard lock(d3d::Lock());
        tex = d3d::State().textures[0];
    }
    const auto at = g_headers.find(tex);
    if (at == g_headers.end() || mem::VirtualFromPhysical(kernel::R32(tex + 4)) != at->second) return;
    const auto tag = g_tagged.find(at->second);
    if (tag == g_tagged.end()) return;
    const int s = tag->second.art;
    const Art& art = kArt[s];
    // DrawVerticesUP(type, count, data, stride): the vertices' x, y first, their texture coordinates last
    const uint32_t count = kernel::R32(ctx.esp + 4), data = kernel::R32(ctx.esp + 8), stride = kernel::R32(ctx.esp + 12);
    constexpr uint32_t kMaxVertices = 16, kMaxStride = 64;
    if (count < 3 || count > kMaxVertices || stride < 24 || stride > kMaxStride || stride % 4 || !data) return;
    if (!mem::IsCommitted(data) || !mem::IsCommitted(data + count * stride - 1)) return;
    auto F = [&](uint32_t i, uint32_t off) { return kernel::G<float>(data + i * stride + off); };
    float m[2][3];  // screen = m * (u, v, 1)
    // the affine map from the texture coordinates at uv to the screen, from three vertices that span the square; true
    // only when it puts every vertex where it is, so a wrong guess at where the coordinates lie is caught
    auto fit = [&](uint32_t uv) {
        if (uv < 8 || uv + 8 > stride) return false;
        // only a quad of the whole texture: one of a piece (the on-screen keyboard's keys are profileKey cut in nine,
        // the corners kept and the edges stretched) would be narrowed about the middle of its own extrapolated square,
        // each piece about another, and the pieces' seams torn apart
        float umin = F(0, uv), umax = umin, vmin = F(0, uv + 4), vmax = vmin;
        for (uint32_t i = 1; i < count; i++) {
            umin = std::min(umin, F(i, uv)), umax = std::max(umax, F(i, uv));
            vmin = std::min(vmin, F(i, uv + 4)), vmax = std::max(vmax, F(i, uv + 4));
        }
        if (!(umax - umin >= 0.9f && umax - umin <= 1.1f && vmax - vmin >= 0.9f && vmax - vmin <= 1.1f)) return false;
        int k[3] = {-1, -1, -1};
        float det = 0;
        for (uint32_t a = 0; a < count && k[0] < 0; a++)
            for (uint32_t b = a + 1; b < count && k[0] < 0; b++)
                for (uint32_t c = b + 1; c < count && k[0] < 0; c++) {
                    const float d = (F(b, uv) - F(a, uv)) * (F(c, uv + 4) - F(a, uv + 4)) -
                                    (F(c, uv) - F(a, uv)) * (F(b, uv + 4) - F(a, uv + 4));
                    if (std::fabs(d) > 1e-4f) k[0] = int(a), k[1] = int(b), k[2] = int(c), det = d;
                }
        if (k[0] < 0) return false;
        const float u0 = F(k[0], uv), v0 = F(k[0], uv + 4);
        const float du1 = F(k[1], uv) - u0, dv1 = F(k[1], uv + 4) - v0;
        const float du2 = F(k[2], uv) - u0, dv2 = F(k[2], uv + 4) - v0;
        for (int c = 0; c < 2; c++) {
            const float p0 = F(k[0], c * 4), p1 = F(k[1], c * 4) - p0, p2 = F(k[2], c * 4) - p0;
            m[c][0] = (p1 * dv2 - p2 * dv1) / det;
            m[c][1] = (p2 * du1 - p1 * du2) / det;
            m[c][2] = p0 - m[c][0] * u0 - m[c][1] * v0;
        }
        for (uint32_t i = 0; i < count; i++)
            for (int c = 0; c < 2; c++)
                if (!(std::fabs(m[c][0] * F(i, uv) + m[c][1] * F(i, uv + 4) + m[c][2] - F(i, c * 4)) < 0.5f))
                    return false;
        return true;
    };
    // the sprite routines' 28-byte vertices (x y z, colour, u v, 4 bytes more) have them at 16; else the last 8 bytes
    uint32_t uv = uv_offset >= 0 ? uint32_t(uv_offset) : 16;
    if (!fit(uv) && (uv_offset >= 0 || !fit(uv = stride - 8))) return;
    // the full picture over texture coordinates 0..1 is the square over -l..1+r and -t..1+b
    const float l = float(art.pad[0]) / float(art.pw), t = float(art.pad[1]) / float(art.ph);
    const float r = float(art.pad[2]) / float(art.pw), b = float(art.pad[3]) / float(art.ph);
    // the frame (640x480, 4:3) is shown at the display's shape, which widens what is drawn on it: the picture narrowed
    // about the square's middle by as much, unless the multiplayer HUD's split places it unstretched already
    const auto [scope, mode] = d3d::HudScopeNow();
    const bool split = gpu::g_hud_split.load(std::memory_order_relaxed) && scope && mode != gpu::kHudStretched;
    // ... and the on-screen keyboard's keys stay stretched with the grid they sit in, the rest of it profileKey in pieces
    const bool in_grid = std::string_view(art.name).starts_with("frontend\\profileKey");
    const float narrow = split || in_grid ? 1.0f : std::min(1.0f, (4.0f / 3.0f) / app::FrameAspect());
    const float mid = m[0][0] * 0.5f + m[0][1] * 0.5f + m[0][2];
    if (!art.full_png && narrow == 1.0f) return;
    static uint32_t copy = 0;
    if (!copy) copy = mem::AllocSystem(kMaxVertices * kMaxStride);
    const uint32_t standin = art.full_png ? StandIn(s) : 0;
    if (!copy || (art.full_png && !standin)) return;
    std::memcpy(mem::Base() + copy, mem::Base() + data, count * stride);
    for (uint32_t i = 0; i < count; i++) {
        const float u = F(i, uv) * (1 + l + r) - l, v = F(i, uv + 4) * (1 + t + b) - t;
        const float x = m[0][0] * u + m[0][1] * v + m[0][2], y = m[1][0] * u + m[1][1] * v + m[1][2];
        kernel::G<float>(copy + i * stride) = mid + (x - mid) * narrow;
        kernel::G<float>(copy + i * stride + 4) = y;
    }
    kernel::W32(ctx.esp + 8, copy);
    if (standin) d3d::TextureForNextDraw(0, standin);
}

uint32_t hud_art::HudQuad(X86Context& ctx) {
    if (!g_on || g_headers.empty()) return 0;
    constexpr uint32_t kCorners = 0xC, kTexture = 0x1C, kSetTexture = 0x539960;  // D3DDevice_SetTexture, eax the stage
    const uint32_t tex = kernel::R32(ctx.esp + kTexture);
    const auto at = g_headers.find(tex);
    if (at == g_headers.end() || mem::VirtualFromPhysical(kernel::R32(tex + 4)) != at->second) return 0;
    const auto tag = g_tagged.find(at->second);
    if (tag == g_tagged.end()) return 0;
    const int s = tag->second.art;
    const Art& art = kArt[s];
    if (!art.full_png) return 0;
    const uint32_t standin = StandIn(s);
    static uint32_t grown = 0;
    if (!grown) grown = mem::AllocSystem(32);
    if (!standin || !grown) return 0;
    // the corners: top left, top right, bottom left, bottom right, each an x and a y; the quad's texture coordinates
    // are 0..1 over them (0x1CA7F0), so the full picture's corners lie at -l..1+r by -t..1+b of the square
    float c[4][2];
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t p = kernel::R32(ctx.esp + kCorners + i * 4);
        if (!p || !mem::IsCommitted(p) || !mem::IsCommitted(p + 7)) return 0;
        c[i][0] = kernel::G<float>(p), c[i][1] = kernel::G<float>(p + 4);
    }
    const float l = float(art.pad[0]) / float(art.pw), t = float(art.pad[1]) / float(art.ph);
    const float r = float(art.pad[2]) / float(art.pw), b = float(art.pad[3]) / float(art.ph);
    const float uv[4][2] = {{-l, -t}, {1 + r, -t}, {-l, 1 + b}, {1 + r, 1 + b}};
    for (uint32_t i = 0; i < 4; i++) {
        const float u = uv[i][0], v = uv[i][1];
        for (int k = 0; k < 2; k++)  // bilinear over the four corners, carried past them
            kernel::G<float>(grown + i * 8 + k * 4) = c[0][k] * (1 - u) * (1 - v) + c[1][k] * u * (1 - v) +
                                                       c[2][k] * (1 - u) * v + c[3][k] * u * v;
        kernel::W32(ctx.esp + kCorners + i * 4, grown + i * 8);
    }
    ctx.eax = 0;
    kernel::CallGuest(ctx, kSetTexture, {standin});
    return tex;
}

void hud_art::Poll() {
    constexpr uint32_t kEvery = 8;  // frames between walks
    static uint32_t frame = 0;
    if (frame++ % kEvery != 0) return;
    if (!diag::Perf()) {
        Walk();
        return;
    }
    // XBE_PERF: the walks' longest time a second, on the game thread
    using Clock = std::chrono::steady_clock;
    static Clock::time_point since = Clock::now();
    static double most = 0, total = 0;
    static uint32_t walks = 0;
    const auto start = Clock::now();
    Walk();
    const auto end = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    most = std::max(most, ms);
    total += ms;
    walks++;
    if (end - since >= std::chrono::seconds(1)) {
        LOGR("perf: hud art walks: %u, longest %.2f ms, %.2f ms on average, %zu entries known, %zu textures tagged",
             walks, most, total / walks, g_known.size(), g_tagged.size());
        since = end;
        most = total = 0;
        walks = 0;
    }
}

#else

bool hud_art::g_on;
void hud_art::Init() {}
void hud_art::Poll() {}
void hud_art::QuadDraw(X86Context&, int) {}
uint32_t hud_art::HudQuad(X86Context&) { return 0; }

#endif
