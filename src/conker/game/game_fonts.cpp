// High-quality fonts: the game's title and text fonts drawn from TrueType files (stb_truetype,
// the copy ImGui carries). The title font is Reloaded
// Serif Italic (src/conker/ReloadedSerifItalic.ttf, the game's own title font retraced), carried in the game; the text
// font is Trebuchet MS Bold, the system's on Windows (Fonts\trebucbd.ttf), elsewhere fonts/Trebucbd.ttf beside the
// game, as Setup installs it.
//
// A font file (aid/font/<name>/default.bin) holds a font set, which 0x3D49A0 picks (its block the second argument,
// [esp+8]: 0xCA dwords copied to 0x8072F4, the fonts' table from +4, 0x8072F8 on, and the character map last, +0x324
// to 0x807618). A font (F) holds its picture's facts at [F] (P: line height +0, the space's width +4, the atlas's
// width +8 and height +0xA as words) and its glyphs at [F+8], 18 bytes each: x and y offsets of the quad +0 +1, its
// width and height +2 +3 (signed bytes, in the font's units), texture coordinates u0 u1 v0 v1 +4 to +0xA (words,
// 1/16384 of the atlas's texels less one: u0 * (w - 1), u1 * (w - 0.7), v0 * (h - 1.3), v1 * (h - 0.7)), the
// advance +0xC, a kerning list's index +0xE (-1 none) and the baseline below the quad's top +0x10. Each glyph's y
// offset and baseline add to the font's ascent (37 the title's, 30 the text's). The character map is 2048 slots of a
// character and its glyph (words), double hashed (0x3D4A50). The text drawing (0x3D30C0) places a quad from x
// offset to x + width * scale - 1 and from baseline above the pen's y to height - baseline below it, the atlas's
// texels between the coordinates stretched over it.
//
// With its setting on, the first time the game picks the font each glyph is made again from the TrueType font:
// scaled so its H is as tall as the game's, its box from the shape's with a unit to spare round it, its advance the
// font's, no kerning; the space's width the font's. Their shapes go into a picture of the atlas (gpu::FontAtlas)
// larger than the title's, each in a box of its own, and the texture coordinates point there; the renderer samples
// that picture in the atlas's place once the glyphs have drawn from it (GlyphQuad names the texture). A character
// the font has no shape for keeps the game's glyph, its texels copied into the picture. The glyphs as the file had
// them are kept, and put back with the setting off.
#include "common/game_fonts.h"
#include "core/diag.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

bool game_fonts::g_title, game_fonts::g_text;

namespace {

using kernel::R16;
using kernel::R32;

constexpr uint32_t kFontTable = 0x8072F8, kCurrent = 0x807674;
constexpr uint32_t kRecord = 18, kMapSlots = 2048, kMaxGlyphs = 1024;
// how the glyphs' texture coordinates scale (0x649138) and the fraction the text drawing adds and takes (0x648D3C)
constexpr float kUv = 16384.0f, kUvBias = 0.3f;

// the game's fonts by their atlas and line height, and their capitals' height in units (the H's ink, measured)
struct Known {
    uint16_t w, h;
    uint32_t line;
    float cap;
};
enum Kind { kTitle, kText, kKinds };
constexpr Known kKnown[kKinds] = {{512, 203, 47, 28.0f}, {256, 240, 37, 21.8f}};
// the picture's texels for a unit of the font's, about (the title's labels in the pub are drawn several times the
// atlas's size), and the most texels it may be on a side
constexpr float kTexelsPerUnit = 8.0f;
constexpr uint32_t kMaxSide = 4096;

const uint8_t kTitleFont[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "../ReloadedSerifItalic.ttf"
#pragma clang diagnostic pop
};

namespace fs = std::filesystem;

fs::path TextFontPath() {
#ifdef _WIN32
    const wchar_t* windir = _wgetenv(L"WINDIR");
    return fs::path(windir && *windir ? windir : L"C:\\Windows") / L"Fonts" / L"trebucbd.ttf";
#else
    return app::ExeDir() / "fonts" / "Trebucbd.ttf";
#endif
}

// a kind's font file, empty if it can't be read. The text font's is read the first time. Game thread
std::span<const uint8_t> FontFile(int kind) {
    if (kind == kTitle) return kTitleFont;
    static const std::vector<uint8_t> text = [] {
        std::ifstream in(TextFontPath(), std::ios::binary);
        std::vector<uint8_t> data;
        if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return data;
    }();
    return text;
}

std::atomic<uint64_t> g_atlas_id{0};

// a font the game picked, as the file had it and as made from the player's
struct Font {
    uint32_t f = 0, glyphs = 0, count = 0;
    int kind = -1;
    std::vector<uint8_t> original, mine;  // the glyphs' bytes
    uint32_t space = 0, my_space = 0;     // the space's width, [P+4]
    bool on = false;                      // the setting mine was made for
    std::shared_ptr<const gpu::FontAtlas> atlas;
    uint32_t texture = 0;  // the atlas's level 0 the glyphs were seen drawing from, given the atlas
};
std::vector<Font> g_fonts;  // game thread
bool g_string = false;      // a string's first glyph is yet to be sent, game thread

void Unregister(Font& font) {
    if (font.texture) gpu::SetFontAtlas(font.texture, nullptr);
    font.texture = 0;
}

bool Readable(uint32_t va, uint32_t size) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1); }

int Identify(uint32_t f) {
    if (!Readable(f, 12)) return -1;
    const uint32_t p = R32(f);
    if (!Readable(p, 12)) return -1;
    for (int k = 0; k < kKinds; k++)
        if (R16(p + 8) == kKnown[k].w && R16(p + 0xA) == kKnown[k].h && R32(p) == kKnown[k].line) return k;
    return -1;
}

struct Rec {
    int8_t x, y, w, h;
    int16_t u0, u1, v0, v1, advance, kern;
    uint8_t base, pad;
};
static_assert(sizeof(Rec) == kRecord);

// the glyphs made again from the font file into mine, and the picture they draw from; false where nothing could be
// made
bool Make(Font& font, uint32_t map) {
    const std::span<const uint8_t> file = FontFile(font.kind);
    if (file.empty()) return false;
    const int offset = stbtt_GetFontOffsetForIndex(file.data(), 0);
    stbtt_fontinfo f;
    if (offset < 0 || !stbtt_InitFont(&f, file.data(), offset)) return false;
    // a glyph's box (y up, empty without an outline) and advance, in the font's units
    struct Box {
        int x0, y0, x1, y1;
    };
    auto measure = [&](int gi, Box& box, float& advance) {
        box = {};
        if (!stbtt_IsGlyphEmpty(&f, gi)) stbtt_GetGlyphBox(&f, gi, &box.x0, &box.y0, &box.x1, &box.y1);
        int adv = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&f, gi, &adv, &bearing);
        advance = float(adv);
    };

    const Known& known = kKnown[font.kind];
    const uint32_t aw = known.w, ah = known.h;
    std::vector<Rec> rec(font.count);
    std::memcpy(rec.data(), font.original.data(), font.original.size());
    // each glyph's character, the first the map has for it
    std::vector<uint32_t> chars(font.count, 0);
    for (uint32_t s = 0; s < kMapSlots; s++) {
        const uint16_t c = R16(map + s * 4), g = R16(map + s * 4 + 2);
        if (c != 0xFFFF && g < font.count && (!chars[g] || c < chars[g])) chars[g] = c;
    }
    // the ascent: the H's y offset and baseline
    int ascent = rec[0].y + rec[0].base;
    for (uint32_t g = 0; g < font.count; g++)
        if (chars[g] == 'H') ascent = rec[g].y + rec[g].base;
    // the font's capitals: its H
    Box box;
    float adv;
    const int h = stbtt_FindGlyphIndex(&f, 'H');
    if (!h) return false;
    measure(h, box, adv);
    if (box.y1 <= 0) return false;
    const float sf = known.cap / float(box.y1);  // units a font unit

    // each glyph's quad and its box in the picture: the font's shape, or the game's glyph copied
    struct Item {
        uint32_t g;
        int index;                 // the font's glyph, 0 the game's copied
        bool empty;                // a shape with no outline (a space)
        float bw, bh;              // the box, in the atlas's texels at 1 a unit (scaled by p below)
        float sx0, sy0, sx1, sy1;  // a copy's texels in the title's atlas
        float bx, by;
    };
    std::vector<Item> items;
    for (uint32_t g = 0; g < font.count; g++) {
        Item it{g, chars[g] ? stbtt_FindGlyphIndex(&f, int(chars[g])) : 0, false, 0, 0, 0, 0, 0, 0, 0, 0};
        Rec& r = rec[g];
        if (it.index) {
            measure(it.index, box, adv);
            float left = 0, right = 0, top = float(ascent), bottom = float(ascent);
            it.empty = box.x1 <= box.x0 || box.y1 <= box.y0;
            if (!it.empty) {
                left = float(box.x0) * sf, right = float(box.x1) * sf;
                top = float(ascent) - float(box.y1) * sf, bottom = float(ascent) - float(box.y0) * sf;
            }
            const int qx = int(std::floor(left)) - 1, qy = int(std::floor(top)) - 1;
            // width * scale - 1 across, height down: a unit spare round the shape
            const int qw = int(std::ceil(right)) + 2 - qx, qh = int(std::ceil(bottom)) + 1 - qy;
            r.x = int8_t(std::clamp(qx, -128, 127));
            r.y = int8_t(std::clamp(qy, -128, 127));
            r.w = int8_t(std::clamp(qw, 2, 127));
            r.h = int8_t(std::clamp(qh, 1, 127));
            r.base = uint8_t(std::clamp(ascent - r.y, 0, 255));
            r.advance = int16_t(std::clamp(int(std::lround(adv * sf)), 0, 32767));
            r.kern = -1;
            it.bw = float(r.w - 1), it.bh = float(r.h);
        } else {
            it.sx0 = float(r.u0) / kUv * float(aw - 1), it.sx1 = float(r.u1) / kUv * (float(aw - 1) + kUvBias);
            it.sy0 = float(r.v0) / kUv * (float(ah - 1) - kUvBias), it.sy1 = float(r.v1) / kUv * (float(ah - 1) + kUvBias);
            it.bw = std::max(it.sx1 - it.sx0, 0.0f), it.bh = std::max(it.sy1 - it.sy0, 0.0f);
        }
        items.push_back(it);
    }

    // the boxes in rows, tallest first, as large as the atlas lets them (p atlas texels a unit)
    std::vector<uint32_t> order(items.size());
    for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return items[a].bh > items[b].bh; });
    constexpr float kGap = 2.0f;  // atlas texels between boxes: the smallest levels blend that far
    float p = 1.0f;
    for (;; p *= 0.92f) {
        if (p < 0.2f) return false;
        float x = kGap, y = kGap, row = 0;
        bool fits = true;
        for (uint32_t i : order) {
            Item& it = items[i];
            const float w = std::ceil(it.bw * p), h = std::ceil(it.bh * p);
            if (x + w + kGap > float(aw)) x = kGap, y += row + kGap, row = 0;
            if (y + h + kGap > float(ah) || w + 2 * kGap > float(aw)) {
                fits = false;
                break;
            }
            it.bx = x, it.by = y;
            x += w + kGap;
            row = std::max(row, h);
        }
        if (fits) break;
    }
    const uint32_t scale = std::clamp(uint32_t(std::ceil(kTexelsPerUnit / p)), 1u, kMaxSide / std::max(aw, ah));
    auto atlas = std::make_shared<gpu::FontAtlas>();
    atlas->w = aw, atlas->h = ah, atlas->scale = scale;
    atlas->id = ++g_atlas_id;
    const uint32_t pw = aw * scale, ph = ah * scale;
    atlas->coverage.assign(size_t(pw) * ph, 0);
    const float k = p * float(scale);  // picture texels a unit
    const float ps = sf * k;           // picture texels a font unit
    std::vector<uint8_t> bitmap;
    for (Item& it : items) {
        Rec& r = rec[it.g];
        const float bw = it.bw * p, bh = it.bh * p;
        r.u0 = int16_t(std::lround(it.bx * kUv / float(aw - 1)));
        r.u1 = int16_t(std::lround((it.bx + bw) * kUv / (float(aw - 1) + kUvBias)));
        r.v0 = int16_t(std::lround(it.by * kUv / (float(ah - 1) - kUvBias)));
        r.v1 = int16_t(std::lround((it.by + bh) * kUv / (float(ah - 1) + kUvBias)));
        const int bx0 = int(it.bx * float(scale)), by0 = int(it.by * float(scale));
        const int bx1 = std::min(int(std::ceil((it.bx + bw) * float(scale))), int(pw));
        const int by1 = std::min(int(std::ceil((it.by + bh) * float(scale))), int(ph));
        if (!it.index) {
            atlas->copies.push_back({it.sx0, it.sy0, it.sx1, it.sy1, uint32_t(bx0), uint32_t(by0), uint32_t(bx1),
                                     uint32_t(by1)});
            continue;
        }
        if (it.empty) continue;
        // the pen at x 0 on the baseline, where the quad's corner (x, y) is the box's; the outline drawn moved by the
        // fraction of a texel the pen is past the grid
        const float ox = it.bx * float(scale) - float(r.x) * k, oy = it.by * float(scale) + float(ascent - r.y) * k;
        const float fx = ox - std::floor(ox), fy = oy - std::floor(oy);
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&f, it.index, ps, ps, fx, fy, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;
        bitmap.assign(size_t(gw) * size_t(gh), 0);
        stbtt_MakeGlyphBitmapSubpixel(&f, bitmap.data(), gw, gh, gw, ps, ps, fx, fy, it.index);
        const int left = int(std::floor(ox)) + x0, top = int(std::floor(oy)) + y0;
        for (int y = 0; y < gh; y++) {
            const int py = top + y;
            if (py < by0 || py >= by1) continue;
            const uint8_t* row = bitmap.data() + size_t(y) * size_t(gw);
            for (int x = 0; x < gw; x++) {
                const int px = left + x;
                if (px < bx0 || px >= bx1) continue;
                uint8_t& c = atlas->coverage[size_t(py) * pw + size_t(px)];
                c = std::max(c, row[x]);
            }
        }
    }
    font.mine.resize(font.original.size());
    std::memcpy(font.mine.data(), rec.data(), font.mine.size());
    // the space: its advance
    font.my_space = font.space;
    if (const int sp = stbtt_FindGlyphIndex(&f, ' ')) {
        measure(sp, box, adv);
        font.my_space = uint32_t(std::max(1L, std::lround(adv * sf)));
    }
    font.atlas = std::move(atlas);
    LOGI("fonts: %s font, %u glyphs (%zu the game's), atlas x%u (%.2f texels a unit)",
         font.kind == kTitle ? "title" : "text", font.count, font.atlas->copies.size(), scale, double(p));
    return true;
}

void Write(const Font& font, const std::vector<uint8_t>& bytes, uint32_t space) {
    std::memcpy(kernel::HostPtr(font.glyphs), bytes.data(), bytes.size());
    kernel::W32(R32(font.f) + 4, space);
}

// a font the game is picking: its glyphs as the setting has them
void Pick(uint32_t f, uint32_t map) {
    const int kind = Identify(f);
    if (kind < 0) return;
    const uint32_t glyphs = R32(f + 8);
    if (!Readable(map, kMapSlots * 4)) return;
    uint32_t count = 0;
    for (uint32_t s = 0; s < kMapSlots; s++)
        if (R16(map + s * 4) != 0xFFFF) count = std::max<uint32_t>(count, R16(map + s * 4 + 2) + 1u);
    if (!count || count > kMaxGlyphs || !Readable(glyphs, count * kRecord)) return;
    const bool on = kind == kTitle ? game_fonts::g_title : game_fonts::g_text;
    const uint8_t* now = static_cast<const uint8_t*>(kernel::HostPtr(glyphs));
    const size_t bytes = size_t(count) * kRecord;
    const uint32_t space = R32(R32(f) + 4);
    auto it = std::find_if(g_fonts.begin(), g_fonts.end(), [&](const Font& x) { return x.f == f; });
    if (it == g_fonts.end()) it = g_fonts.insert(g_fonts.end(), Font{});
    Font& font = *it;
    const bool same = font.glyphs == glyphs && font.count == count && font.kind == kind;
    const bool mine = same && font.mine.size() == bytes && !std::memcmp(now, font.mine.data(), bytes) &&
                      space == font.my_space;
    if (mine && font.on == on) return;
    const bool original =
        same && font.original.size() == bytes && !std::memcmp(now, font.original.data(), bytes) && space == font.space;
    if (!mine && !original) {
        // loaded again, or another font in its place
        Unregister(font);
        font = Font{};
        font.f = f, font.glyphs = glyphs, font.count = count, font.kind = kind;
        font.original.assign(now, now + bytes);
        font.space = space;
    }
    font.on = on;
    if (on && Make(font, map)) {
        Write(font, font.mine, font.my_space);
        if (font.texture) gpu::SetFontAtlas(font.texture, font.atlas);
        return;
    }
    // the game's own
    Write(font, font.original, font.space);
    font.mine = font.original;
    font.my_space = font.space;
    font.atlas.reset();
    Unregister(font);
}

}  // namespace

bool game_fonts::TextInstalled() {
    static const bool installed = [] {
        std::error_code ec;
        return fs::is_regular_file(TextFontPath(), ec);
    }();
    return installed;
}

void game_fonts::StringStart() { g_string = !g_fonts.empty(); }

void game_fonts::GlyphQuad() {
    if (!g_string) return;
    g_string = false;
    const uint32_t current = R32(kCurrent);
    if (!current) return;
    const uint32_t f = R32(kFontTable + R32(current + 0x10) * 4);
    auto it = std::find_if(g_fonts.begin(), g_fonts.end(), [&](const Font& x) { return x.f == f; });
    if (it == g_fonts.end() || (!it->atlas && !it->texture)) return;
    uint32_t data = 0;
    {
        d3d::FlushPending();
        std::lock_guard lock(d3d::Lock());
        gpu::TextureDesc d;
        if (gpu::Describe(d3d::State().textures[0], d) && d.width == kKnown[it->kind].w &&
            d.height == kKnown[it->kind].h)
            data = d.data;
    }
    if (!it->atlas) {
        Unregister(*it);
        return;
    }
    if (!data || data == it->texture) return;
    Unregister(*it);
    it->texture = data;
    gpu::SetFontAtlas(data, it->atlas);
}

// 0x3D49A0 picks a font set, its block the second argument: each of its fonts made from the player's font first, so
// what the game reads from them as it picks (the W's advance) is the new glyphs'
X86_EXTERN_FUNC(__imp__sub_003D49A0);
X86_FUNC(sub_003D49A0) {
    const uint32_t block = R32(ctx.esp + 8);
    if (block && gpu::g_font_atlases.load(std::memory_order_relaxed)) {
        const uint32_t map = R32(block + 0x324);
        for (uint32_t i = 0; i < 4; i++)
            if (const uint32_t f = R32(block + 4 + i * 4)) Pick(f, map);
    }
    __imp__sub_003D49A0(ctx, base);
}
