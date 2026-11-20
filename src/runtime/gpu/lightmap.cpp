// Baked lightmaps made larger with their edges smoothed along their own direction. The level's lightmaps (the texture
// a level draw multiplies its surface by, doubled) were rendered aliased: a shadow's edge across a surface is a row of
// steps a texel or two high, which linear filtering only softens. Each texel of the larger picture averages the
// lightmap along the edge through it, following the edge's direction (the structure tensor of the lightmap's
// luminance, gathered over a few texels so that the steps of one edge read as one slope) for a few texels each way, as
// much as the direction is clear; across the edge it is the lightmap filtered as before. Flat light and corners,
// where no one direction holds, stay as they were. The Vulkan renderer's lightmap smoother draws the larger picture
// (vulkan.cpp, kLightmapSmoothHlsl) from what PrepareLightmap makes here.

#include "gpu/texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

void PrepareLightmap(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, float along, LightmapField& f) {
    // the gaussian sigma along the edge in texels, and the texels the direction is gathered over, in proportion (3 to
    // 5): a step of an edge reads as its slope. The time goes as along * scale squared
    f.along = std::max(along, 0.5f);
    f.step = 1.0f;  // texels between samples along it
    f.steps = int(std::ceil(f.along * 2.5f / f.step));
    const float kTensorSigma = f.along * 0.6f;

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
    f.img = std::move(img.v);
    f.tensor.assign(size_t(w) * h * 4, 0.f);
    for (size_t i = 0; i < size_t(w) * h; i++) std::memcpy(&f.tensor[i * 4], &tensor.v[i * 3], 12);
}

}  // namespace gpu
