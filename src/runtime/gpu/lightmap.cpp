// Baked lightmaps made larger with their edges smoothed along their own direction. The level's lightmaps (the texture
// a level draw multiplies its surface by, doubled) were rendered aliased: a shadow's edge across a surface is a row of
// steps a texel or two high, which linear filtering only softens. Each texel of the larger picture averages the
// lightmap along the edge through it, following the edge's direction (the structure tensor of the lightmap's
// luminance, gathered over a few texels so that the steps of one edge read as one slope) for a few texels each way, as
// much as the direction is clear; across the edge it is the lightmap filtered as before. Flat light and corners,
// where no one direction holds, stay as they were.

#include "gpu/texture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "gpu/shaders.h"  // Hash
#if !XBE_HEADLESS
#include <miniz.h>

#include "app/app.h"
#endif

namespace gpu {

namespace {

struct Field {
    uint32_t w, h;
    std::vector<float> v;  // per texel, n channels
    int n;
    const float* At(int x, int y) const {
        x = std::clamp(x, 0, int(w) - 1);
        y = std::clamp(y, 0, int(h) - 1);
        return &v[(size_t(y) * w + size_t(x)) * size_t(n)];
    }
    // bilinear at texel coordinates (texel centres at integers), clamped
    void Sample(float x, float y, float* out) const {
        float fx = std::floor(x), fy = std::floor(y);
        float ax = x - fx, ay = y - fy;
        int ix = int(fx), iy = int(fy);
        const float *a = At(ix, iy), *b = At(ix + 1, iy), *c = At(ix, iy + 1), *d = At(ix + 1, iy + 1);
        for (int k = 0; k < n; k++)
            out[k] = (a[k] * (1 - ax) + b[k] * ax) * (1 - ay) + (c[k] * (1 - ax) + d[k] * ax) * ay;
    }
};

// separable gaussian over a field, clamped at the borders
void Blur(Field& f, float sigma) {
    int r = int(std::ceil(sigma * 3));
    std::vector<float> k(size_t(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; i++) sum += k[size_t(i + r)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
    for (float& x : k) x /= sum;
    Field t = f;
    for (int pass = 0; pass < 2; pass++) {
        const Field& src = pass ? t : f;
        Field& dst = pass ? f : t;
        for (uint32_t y = 0; y < f.h; y++)
            for (uint32_t x = 0; x < f.w; x++) {
                float* o = &dst.v[(size_t(y) * f.w + x) * size_t(f.n)];
                for (int c = 0; c < f.n; c++) o[c] = 0;
                for (int i = -r; i <= r; i++) {
                    const float* s = pass ? src.At(int(x), int(y) + i) : src.At(int(x) + i, int(y));
                    for (int c = 0; c < f.n; c++) o[c] += s[c] * k[size_t(i + r)];
                }
            }
    }
}

// the edge's direction at a point and how clear it is (0 none, 1 one direction only)
void Tangent(const Field& tensor, float x, float y, float& tx, float& ty, float& coherence) {
    float j[3];
    tensor.Sample(x, y, j);
    float a = j[0], b = j[1], c = j[2];
    float d = std::sqrt((a - c) * (a - c) + 4 * b * b);
    float l1 = (a + c + d) * 0.5f, l2 = (a + c - d) * 0.5f;
    // the gradient lies along the larger eigenvector, the edge across it
    float gx = b, gy = l1 - a;
    if (std::fabs(gx) + std::fabs(gy) < 1e-12f) {
        gx = a >= c ? 1.f : 0.f;
        gy = a >= c ? 0.f : 1.f;
    }
    float len = std::sqrt(gx * gx + gy * gy);
    tx = -gy / len;
    ty = gx / len;
    float s = l1 + l2;
    // a contrast floor: below a few levels of difference the direction is noise (the DXT blocks' own)
    constexpr float kFloor = 4e-5f;
    coherence = s > 0 ? ((l1 - l2) / s) * (s / (s + kFloor)) : 0;
}

// a DXT colour block's 16 texels as BGRA; four colours always for DXT3 and DXT5
void ColorBlock(const uint8_t* b, bool four, uint8_t (*o)[4]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t p[4][4];
    auto unpack = [](uint16_t c, uint8_t* t) {
        t[2] = uint8_t((c >> 11) * 255 / 31);
        t[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        t[0] = uint8_t((c & 31) * 255 / 31);
        t[3] = 255;
    };
    unpack(c0, p[0]);
    unpack(c1, p[1]);
    for (int k = 0; k < 3; k++) {
        if (four || c0 > c1) {
            p[2][k] = uint8_t((2 * p[0][k] + p[1][k] + 1) / 3);
            p[3][k] = uint8_t((p[0][k] + 2 * p[1][k] + 1) / 3);
        } else {
            p[2][k] = uint8_t((p[0][k] + p[1][k]) / 2);
            p[3][k] = 0;
        }
    }
    p[2][3] = 255;
    p[3][3] = (four || c0 > c1) ? 255 : 0;
    uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(o[i], p[(idx >> (i * 2)) & 3], 4);
}

}  // namespace

void DecodeBlocks(Host host, const uint8_t* blocks, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    const uint32_t bytes = host == Host::BC1 ? 8 : 16;
    const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    out.assign(size_t(w) * h * 4, 0);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = blocks + (size_t(by) * bw + bx) * bytes;
            uint8_t t[16][4];
            ColorBlock(host == Host::BC1 ? b : b + 8, host != Host::BC1, t);
            if (host == Host::BC2) {
                for (int i = 0; i < 16; i++) t[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            } else if (host == Host::BC3) {
                uint8_t a[8] = {b[0], b[1]};
                for (int k = 1; k < 7; k++)
                    a[k + 1] = b[0] > b[1] ? uint8_t(((7 - k) * b[0] + k * b[1] + 3) / 7)
                                           : k < 5 ? uint8_t(((5 - k) * b[0] + k * b[1] + 2) / 5) : uint8_t(k == 5 ? 0 : 255);
                uint64_t idx = 0;
                for (int k = 0; k < 6; k++) idx |= uint64_t(b[2 + k]) << (8 * k);
                for (int i = 0; i < 16; i++) t[i][3] = a[(idx >> (i * 3)) & 7];
            }
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < w && y < h) std::memcpy(&out[(size_t(y) * w + x) * 4], t[i], 4);
            }
        }
}

static void SmoothUncached(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, uint32_t scale, float along,
                           std::vector<uint8_t>& out) {
    // the gaussian sigma along the edge in texels, and the texels the direction is gathered over, in proportion (3 to
    // 5): a step of an edge reads as its slope. The time goes as along * scale squared
    const float kAlong = std::max(along, 0.5f);
    const float kTensorSigma = kAlong * 0.6f;
    constexpr float kStep = 1.0f;         // texels between samples along it
    const int steps = int(std::ceil(kAlong * 2.5f / kStep));

    Field img{w, h, std::vector<float>(size_t(w) * h * 4), 4};
    for (size_t i = 0; i < img.v.size(); i++) img.v[i] = px[i] / 255.f;
    // the structure tensor of the luminance
    Field tensor{w, h, std::vector<float>(size_t(w) * h * 3), 3};
    auto lum = [&](int x, int y) {
        const float* p = img.At(x, y);
        return p[0] * 0.114f + p[1] * 0.587f + p[2] * 0.299f;  // BGRA
    };
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            int X = int(x), Y = int(y);
            // Scharr: rotation invariant enough for the direction to come out even
            float gx = (3 * (lum(X + 1, Y - 1) - lum(X - 1, Y - 1)) + 10 * (lum(X + 1, Y) - lum(X - 1, Y)) +
                        3 * (lum(X + 1, Y + 1) - lum(X - 1, Y + 1))) / 32.f;
            float gy = (3 * (lum(X - 1, Y + 1) - lum(X - 1, Y - 1)) + 10 * (lum(X, Y + 1) - lum(X, Y - 1)) +
                        3 * (lum(X + 1, Y + 1) - lum(X + 1, Y - 1))) / 32.f;
            float* t = &tensor.v[(size_t(y) * w + x) * 3];
            t[0] = gx * gx;
            t[1] = gx * gy;
            t[2] = gy * gy;
        }
    Blur(tensor, kTensorSigma);

    const uint32_t ow = w * scale, oh = h * scale;
    out.resize(size_t(ow) * oh * 4);
    auto rows = [&](uint32_t y0, uint32_t y1) {
        for (uint32_t oy = y0; oy < y1; oy++)
            for (uint32_t ox = 0; ox < ow; ox++) {
                // the output texel's centre in lightmap texels
                float x = (float(ox) + 0.5f) / float(scale) - 0.5f, y = (float(oy) + 0.5f) / float(scale) - 0.5f;
                float base[4], acc[4] = {}, wsum = 0, s[4];
                img.Sample(x, y, base);
                float tx, ty, coh;
                Tangent(tensor, x, y, tx, ty, coh);
                for (int c = 0; c < 4; c++) acc[c] = base[c];
                wsum = 1;
                // along the edge both ways, the direction followed as it bends
                for (int side = -1; side <= 1; side += 2) {
                    float px_ = x, py_ = y, dx = tx * float(side), dy = ty * float(side);
                    for (int k = 1; k <= steps; k++) {
                        px_ += dx * kStep;
                        py_ += dy * kStep;
                        if (px_ < -0.5f || py_ < -0.5f || px_ > float(w) - 0.5f || py_ > float(h) - 0.5f) break;
                        float d = float(k) * kStep;
                        float wk = std::exp(-0.5f * d * d / (kAlong * kAlong));
                        img.Sample(px_, py_, s);
                        for (int c = 0; c < 4; c++) acc[c] += s[c] * wk;
                        wsum += wk;
                        float nx, ny, nc;
                        Tangent(tensor, px_, py_, nx, ny, nc);
                        if (nx * dx + ny * dy < 0) {
                            nx = -nx;
                            ny = -ny;
                        }
                        dx = nx;
                        dy = ny;
                    }
                }
                uint8_t* o = &out[(size_t(oy) * ow + ox) * 4];
                for (int c = 0; c < 4; c++) {
                    float v = base[c] + (acc[c] / wsum - base[c]) * coh;
                    o[c] = uint8_t(std::clamp(v * 255.f + 0.5f, 0.f, 255.f));
                }
            }
    };
    uint32_t n = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    if (size_t(ow) * oh < 65536) n = 1;
    std::vector<std::thread> pool;
    for (uint32_t i = 1; i < n; i++) pool.emplace_back(rows, oh * i / n, oh * (i + 1) / n);
    rows(0, oh / n);
    for (auto& t : pool) t.join();
}

// Smoothed lightmaps kept on disk, app::CacheDir()/lightmaps/<key>.lm: a level's take seconds of the worker's time at
// the larger sizes, and come out the same every run. The key is the source texels' hash with the size and smoothing
// they were made at; kCacheRevision moves when SmoothUncached's output changes. Each file is a header and the texels as
// four planes (B, G, R, A) of differences from a median predictor (LOCO-I's: the left, upper and upper left
// neighbours), deflated: the larger picture is smooth, so most differences are near zero and A, all 255, is nothing.
// The folder is held to kCacheMax bytes, the least recently used files going first. XBE_LIGHTMAP_CACHE=0 turns it off.
#if !XBE_HEADLESS
namespace {

constexpr uint32_t kCacheRevision = 1;
constexpr char kCacheMagic[4] = {'L', 'M', 'C', '1'};
constexpr uint64_t kCacheMax = 1ull << 30, kCacheTrimTo = 768ull << 20, kCacheTrimEvery = 64ull << 20;

struct CacheHeader {
    char magic[4];
    uint32_t revision;
    uint32_t w, h;  // the smoothed picture's
    uint64_t key;
    uint32_t packed;  // the deflated bytes after the header
    uint32_t pad = 0;
};

std::mutex g_cache_lock;
uint64_t g_cache_written = kCacheTrimEvery;  // the first store trims

bool CacheEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_LIGHTMAP_CACHE");
        return !(e && e[0] == '0');
    }();
    return on;
}

std::filesystem::path CacheFolder() { return app::CacheDir() / "lightmaps"; }

uint8_t Median(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t lo = std::min(a, b), hi = std::max(a, b);
    if (c >= hi) return lo;
    if (c <= lo) return hi;
    return uint8_t(a + b - c);
}

// BGRA texels to planes of predictor differences, and back
void Encode(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, std::vector<uint8_t>& planes) {
    const size_t n = size_t(w) * h;
    planes.resize(n * 4);
    for (int c = 0; c < 4; c++) {
        uint8_t* o = &planes[n * size_t(c)];
        auto at = [&](uint32_t x, uint32_t y) { return px[(size_t(y) * w + x) * 4 + size_t(c)]; };
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint8_t p = y == 0 ? (x ? at(x - 1, 0) : 0)
                            : x == 0 ? at(0, y - 1)
                                     : Median(at(x - 1, y), at(x, y - 1), at(x - 1, y - 1));
                o[size_t(y) * w + x] = uint8_t(at(x, y) - p);
            }
    }
}

void Decode(const std::vector<uint8_t>& planes, uint32_t w, uint32_t h, std::vector<uint8_t>& px) {
    const size_t n = size_t(w) * h;
    px.resize(n * 4);
    for (int c = 0; c < 4; c++) {
        const uint8_t* d = &planes[n * size_t(c)];
        auto at = [&](uint32_t x, uint32_t y) -> uint8_t& { return px[(size_t(y) * w + x) * 4 + size_t(c)]; };
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint8_t p = y == 0 ? (x ? at(x - 1, 0) : 0)
                            : x == 0 ? at(0, y - 1)
                                     : Median(at(x - 1, y), at(x, y - 1), at(x - 1, y - 1));
                at(x, y) = uint8_t(d[size_t(y) * w + x] + p);
            }
    }
}

std::filesystem::path CachePath(uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.lm", static_cast<unsigned long long>(key));
    return CacheFolder() / name;
}

bool CacheLoad(uint64_t key, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    namespace fs = std::filesystem;
    const fs::path path = CachePath(key);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    CacheHeader hd{};
    if (!f.read(reinterpret_cast<char*>(&hd), sizeof hd) || std::memcmp(hd.magic, kCacheMagic, 4) != 0 ||
        hd.revision != kCacheRevision || hd.key != key || hd.w != w || hd.h != h || hd.packed > (256u << 20))
        return false;
    std::vector<uint8_t> packed(hd.packed);
    if (!f.read(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size()))) return false;
    f.close();
    std::vector<uint8_t> planes(size_t(w) * h * 4);
    mz_ulong len = mz_ulong(planes.size());
    if (mz_uncompress(planes.data(), &len, packed.data(), mz_ulong(packed.size())) != MZ_OK || len != planes.size())
        return false;
    Decode(planes, w, h, out);
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // used: last to go
    return true;
}

// the folder's files over kCacheMax, the least recently used out until kCacheTrimTo
void CacheTrim() {
    namespace fs = std::filesystem;
    struct Entry {
        fs::file_time_type time;
        uint64_t size;
        fs::path path;
    };
    std::vector<Entry> files;
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(CacheFolder(), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".lm") continue;
        Entry x{e.last_write_time(ec), e.file_size(ec), e.path()};
        total += x.size;
        files.push_back(std::move(x));
    }
    if (total <= kCacheMax) return;
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const Entry& x : files) {
        if (total <= kCacheTrimTo) break;
        if (fs::remove(x.path, ec)) total -= x.size;
    }
}

void CacheStore(uint64_t key, uint32_t w, uint32_t h, const std::vector<uint8_t>& px) {
    namespace fs = std::filesystem;
    std::vector<uint8_t> planes;
    Encode(px, w, h, planes);
    mz_ulong len = mz_compressBound(mz_ulong(planes.size()));
    std::vector<uint8_t> packed(len);
    if (mz_compress2(packed.data(), &len, planes.data(), mz_ulong(planes.size()), MZ_DEFAULT_LEVEL) != MZ_OK) return;
    CacheHeader hd{};
    std::memcpy(hd.magic, kCacheMagic, 4);
    hd.revision = kCacheRevision;
    hd.w = w;
    hd.h = h;
    hd.key = key;
    hd.packed = uint32_t(len);
    std::error_code ec;
    fs::create_directories(CacheFolder(), ec);
    // written beside and renamed over: a reader, or another copy of the game, never sees half of one
    const fs::path path = CachePath(key), tmp = fs::path(path).concat(".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(&hd), sizeof hd);
        f.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(len));
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return;
    }
    std::lock_guard<std::mutex> g(g_cache_lock);
    g_cache_written += sizeof hd + len;
    if (g_cache_written >= kCacheTrimEvery) {
        g_cache_written = 0;
        CacheTrim();
    }
}

}  // namespace
#endif

void SmoothLightmap(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, uint32_t scale, float along,
                    std::vector<uint8_t>& out) {
#if !XBE_HEADLESS
    if (CacheEnabled() && px.size() >= size_t(w) * h * 4) {
        uint32_t params[4] = {w, h, scale, 0};
        std::memcpy(&params[3], &along, 4);
        const uint64_t key = Hash(params, sizeof params, Hash(px.data(), size_t(w) * h * 4) ^ kCacheRevision);
        const uint32_t ow = w * scale, oh = h * scale;
        if (CacheLoad(key, ow, oh, out)) return;
        SmoothUncached(px, w, h, scale, along, out);
        CacheStore(key, ow, oh, out);
        return;
    }
#endif
    SmoothUncached(px, w, h, scale, along, out);
}

}  // namespace gpu
