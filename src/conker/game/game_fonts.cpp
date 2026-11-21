// Custom fonts: the game's title and text fonts drawn from TrueType or OpenType files of the player's (FreeType).
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
// With a font set for it, the first time the game picks the font each glyph is made again from the player's font:
// scaled so its H is as tall as the game's, its box from the shape's with a unit to spare round it, its advance the
// font's, no kerning; the space's width the font's. Their shapes go into a picture of the atlas (gpu::FontAtlas)
// larger than the title's, each in a box of its own, and the texture coordinates point there; the renderer samples
// that picture in the atlas's place once the glyphs have drawn from it (GlyphQuad names the texture). A character
// the font has no shape for keeps the game's glyph, its texels copied into the picture. The glyphs as the file had
// them are kept, and put back with the setting cleared.
//
// A file's styles are its faces (a collection's) and a variable font's named instances; a style with a weight axis
// (wght) takes any weight along it, the style's own until one is set.
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
#include <string>
#include <vector>

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#endif

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

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
constexpr Known kKnown[game_fonts::kKinds] = {{512, 203, 47, 28.0f}, {256, 240, 37, 21.8f}};
// the picture's texels for a unit of the font's, about (the title's labels in the pub are drawn several times the
// atlas's size), and the most texels it may be on a side
constexpr float kTexelsPerUnit = 8.0f;
constexpr uint32_t kMaxSide = 4096;
constexpr long kMaxFaces = 64, kMaxInstances = 256;
constexpr FT_ULong kWght = FT_MAKE_TAG('w', 'g', 'h', 't');

// FreeType's library, made once; it and every face are used under g_ft_lock (the menu opens files, the game thread
// draws glyphs)
std::mutex g_ft_lock;
FT_Library g_ft = nullptr;
FT_Library Ft() {
    if (!g_ft && FT_Init_FreeType(&g_ft)) g_ft = nullptr;
    return g_ft;
}
struct FaceRef {
    FT_Face f = nullptr;
    ~FaceRef() {
        if (f) FT_Done_Face(f);
    }
};
struct MmRef {
    FT_MM_Var* mm = nullptr;
    ~MmRef() {
        if (mm) FT_Done_MM_Var(g_ft, mm);
    }
};
// the weight axis's index among a variable face's axes, -1 without one
int WeightAxis(const FT_MM_Var* mm) {
    for (FT_UInt a = 0; mm && a < mm->num_axis; a++)
        if (mm->axis[a].tag == kWght) return int(a);
    return -1;
}

// a style the file holds: its FreeType face index (a collection's face, a named instance in the high 16 bits)
struct Style {
    FT_Long index = 0;
    std::string name;
    int weight = 0;               // its own, on the axis (0 without one)
    int min = 0, max = 0;         // the weight axis's range, min == max without one
};

// a font file the settings name, read whole
struct Face {
    std::string path, family, error;
    std::vector<uint8_t> data;
    std::vector<Style> styles;
    bool ok = false;
};

// what a kind is set to; a style or weight change makes a new one
struct Setting {
    std::shared_ptr<const Face> face;
    size_t style = 0;  // in face->styles
    int weight = 0;    // on the style's axis, 0 the style's own
};

std::mutex g_lock;
Setting g_set[game_fonts::kKinds];
std::atomic<uint64_t> g_gen[game_fonts::kKinds];  // changes with each setting
std::atomic<uint64_t> g_atlas_id{0};

namespace fs = std::filesystem;
fs::path ToPath(const std::string& utf8) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.c_str()))); }
std::string ToUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::vector<uint8_t> ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> data;
    if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return data;
}

// ConfigDir/fonts, where a picked font is copied so the setting outlives the file being moved or deleted
fs::path FontsDir() { return app::ConfigDir() / "fonts"; }

bool InFontsDir(const fs::path& p) {
    std::error_code ec;
    return fs::equivalent(p.parent_path(), FontsDir(), ec);
}

bool NewFace(const Face& face, FT_Long index, FaceRef& out) {
    return !FT_New_Memory_Face(Ft(), face.data.data(), FT_Long(face.data.size()), index, &out.f);
}

std::shared_ptr<const Face> OpenFace(const std::string& path) {
    auto face = std::make_shared<Face>();
    face->path = path;
    face->data = ReadAll(ToPath(path));
    if (face->data.empty()) {
        face->error = "read";
        return face;
    }
    std::lock_guard ft(g_ft_lock);
    FaceRef probe;
    if (!Ft() || !NewFace(*face, -1, probe)) {
        face->error = "font";
        return face;
    }
    const long faces = std::min<long>(probe.f->num_faces, kMaxFaces);
    for (long j = 0; j < faces; j++) {
        FaceRef f;
        if (!NewFace(*face, j, f) || !FT_IS_SCALABLE(f.f)) continue;
        const std::string family = f.f->family_name ? f.f->family_name : "";
        if (face->family.empty()) face->family = family;
        // a collection's other families keep their name before the style's
        const std::string prefix = family != face->family ? family + " " : "";
        MmRef mm;
        if (FT_HAS_MULTIPLE_MASTERS(f.f) && FT_Get_MM_Var(f.f, &mm.mm)) mm.mm = nullptr;
        const int axis = WeightAxis(mm.mm);
        Style base;
        base.index = j;
        base.name = prefix + (f.f->style_name ? f.f->style_name : "Regular");
        if (axis >= 0) {
            base.weight = int(mm.mm->axis[axis].def >> 16);
            base.min = int(mm.mm->axis[axis].minimum >> 16), base.max = int(mm.mm->axis[axis].maximum >> 16);
        }
        face->styles.push_back(base);
        // a variable font's named instances, past the default one's name
        const long instances = std::min<long>(f.f->style_flags >> 16, kMaxInstances);
        for (long i = 1; mm.mm && i <= instances && FT_UInt(i) <= mm.mm->num_namedstyles; i++) {
            FaceRef n;
            if (!NewFace(*face, i << 16 | j, n)) continue;
            Style s = base;
            s.index = i << 16 | j;
            s.name = prefix + (n.f->style_name ? n.f->style_name : std::to_string(i));
            if (axis >= 0) s.weight = int(mm.mm->namedstyle[i - 1].coords[axis] >> 16);
            if (s.name != base.name) face->styles.push_back(s);
        }
    }
    if (face->styles.empty()) {
        face->error = "font";
        return face;
    }
    if (face->family.empty()) face->family = path.substr(path.find_last_of("/\\") + 1);  // npos + 1: all of it
    face->ok = true;
    return face;
}

// a font the game picked, as the file had it and as made from the player's
struct Font {
    uint32_t f = 0, glyphs = 0, count = 0;
    int kind = -1;
    std::vector<uint8_t> original, mine;  // the glyphs' bytes
    uint32_t space = 0, my_space = 0;     // the space's width, [P+4]
    uint64_t gen = ~0ull;                 // g_gen mine was made for
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
    for (int k = 0; k < game_fonts::kKinds; k++)
        if (R16(p + 8) == kKnown[k].w && R16(p + 0xA) == kKnown[k].h && R32(p) == kKnown[k].line) return k;
    return -1;
}

struct Rec {
    int8_t x, y, w, h;
    int16_t u0, u1, v0, v1, advance, kern;
    uint8_t base, pad;
};
static_assert(sizeof(Rec) == kRecord);

constexpr FT_Int32 kLoad = FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;

// the glyphs made again from the setting's face into mine, and the picture they draw from; false where nothing could
// be made. Under g_ft_lock
bool Make(Font& font, uint32_t map, const Setting& set) {
    const Face& face = *set.face;
    const Style& style = face.styles[set.style];
    FaceRef ref;
    if (!Ft() || !NewFace(face, style.index, ref) || !FT_IS_SCALABLE(ref.f)) return false;
    FT_Face f = ref.f;
    if (set.weight > 0 && style.max > style.min) {
        MmRef mm;
        if (!FT_Get_MM_Var(f, &mm.mm)) {
            const int axis = WeightAxis(mm.mm);
            std::vector<FT_Fixed> coords(mm.mm->num_axis);
            if (axis >= 0 && !FT_Get_Var_Design_Coordinates(f, mm.mm->num_axis, coords.data())) {
                coords[size_t(axis)] = FT_Fixed(std::clamp(set.weight, style.min, style.max)) << 16;
                FT_Set_Var_Design_Coordinates(f, mm.mm->num_axis, coords.data());
            }
        }
    }
    // measured a pixel a font unit (26.6 values, so / 64 is font units)
    const FT_UShort upem = f->units_per_EM;
    if (!upem || FT_Set_Char_Size(f, 0, FT_F26Dot6(upem) * 64, 72, 72)) return false;
    auto measure = [&](FT_UInt gi, FT_BBox& box, float& advance) {
        box = {};
        advance = 0;
        if (FT_Load_Glyph(f, gi, kLoad)) return false;
        advance = float(f->glyph->advance.x) / 64.0f;
        if (f->glyph->format == FT_GLYPH_FORMAT_OUTLINE && f->glyph->outline.n_points)
            FT_Outline_Get_CBox(&f->glyph->outline, &box);
        return true;
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
    // the font's capitals: its H, else what its OS/2 table says, else most of its ascent
    FT_BBox box;
    float adv, cap = 0;
    if (const FT_UInt h = FT_Get_Char_Index(f, 'H'); h && measure(h, box, adv)) cap = float(box.yMax) / 64.0f;
    if (cap <= 0)
        if (const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(f, FT_SFNT_OS2)); os2 && os2->version != 0xFFFF)
            cap = float(os2->sCapHeight);
    if (cap <= 0) cap = float(f->ascender) * 0.7f;
    if (cap <= 0) return false;
    const float sf = known.cap / cap;  // units a font unit

    // each glyph's quad and its box in the picture: the font's shape, or the game's glyph copied
    struct Item {
        uint32_t g;
        FT_UInt index;             // the font's glyph, 0 the game's copied
        bool empty;                // a shape with no outline (a space)
        float bw, bh;              // the box, in the atlas's texels at 1 a unit (scaled by p below)
        float sx0, sy0, sx1, sy1;  // a copy's texels in the title's atlas
        float bx, by;
    };
    std::vector<Item> items;
    for (uint32_t g = 0; g < font.count; g++) {
        Item it{g, chars[g] ? FT_Get_Char_Index(f, chars[g]) : 0, false, 0, 0, 0, 0, 0, 0, 0, 0};
        Rec& r = rec[g];
        if (it.index && measure(it.index, box, adv)) {
            float left = 0, right = 0, top = float(ascent), bottom = float(ascent);
            it.empty = box.xMax <= box.xMin || box.yMax <= box.yMin;
            if (!it.empty) {
                left = float(box.xMin) / 64.0f * sf, right = float(box.xMax) / 64.0f * sf;
                top = float(ascent) - float(box.yMax) / 64.0f * sf, bottom = float(ascent) - float(box.yMin) / 64.0f * sf;
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
            it.index = 0;
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
    if (FT_Set_Char_Size(f, 0, std::max<FT_F26Dot6>(1, FT_F26Dot6(std::lround(float(upem) * ps * 64.0f))), 72, 72))
        return false;
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
        if (it.empty || FT_Load_Glyph(f, it.index, kLoad) || f->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
        // the pen at x 0 on the baseline, where the quad's corner (x, y) is the box's; the outline moved by the
        // fraction of a texel the pen is past the grid (FreeType's y goes up)
        const float ox = it.bx * float(scale) - float(r.x) * k, oy = it.by * float(scale) + float(ascent - r.y) * k;
        const float fx = ox - std::floor(ox), fy = oy - std::floor(oy);
        FT_Outline_Translate(&f->glyph->outline, FT_Pos(std::lround(fx * 64.0f)), -FT_Pos(std::lround(fy * 64.0f)));
        if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL)) continue;
        const FT_Bitmap& bm = f->glyph->bitmap;
        if (bm.pixel_mode != FT_PIXEL_MODE_GRAY || !bm.buffer) continue;
        const int left = int(std::floor(ox)) + f->glyph->bitmap_left, top = int(std::floor(oy)) - f->glyph->bitmap_top;
        for (int y = 0; y < int(bm.rows); y++) {
            const int py = top + y;
            if (py < by0 || py >= by1) continue;
            const uint8_t* row = bm.buffer + ptrdiff_t(y) * bm.pitch;
            for (int x = 0; x < int(bm.width); x++) {
                const int px = left + x;
                if (px < bx0 || px >= bx1) continue;
                uint8_t& c = atlas->coverage[size_t(py) * pw + size_t(px)];
                c = std::max(c, row[x]);
            }
        }
    }
    font.mine.resize(font.original.size());
    std::memcpy(font.mine.data(), rec.data(), font.mine.size());
    // the space: its advance at the measuring size again
    font.my_space = font.space;
    if (const FT_UInt sp = FT_Get_Char_Index(f, ' '); sp && !FT_Set_Char_Size(f, 0, FT_F26Dot6(upem) * 64, 72, 72) &&
                                                      measure(sp, box, adv))
        font.my_space = uint32_t(std::max(1L, std::lround(adv * sf)));
    font.atlas = std::move(atlas);
    LOGI("fonts: %s from %s %s (weight %d), %u glyphs (%zu the game's), atlas x%u (%.2f texels a unit)",
         font.kind == game_fonts::kTitle ? "title" : "text", face.family.c_str(), style.name.c_str(),
         set.weight ? set.weight : style.weight, font.count, font.atlas->copies.size(), scale, double(p));
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
    const uint64_t gen = g_gen[kind].load(std::memory_order_acquire);
    const uint8_t* now = static_cast<const uint8_t*>(kernel::HostPtr(glyphs));
    const size_t bytes = size_t(count) * kRecord;
    const uint32_t space = R32(R32(f) + 4);
    auto it = std::find_if(g_fonts.begin(), g_fonts.end(), [&](const Font& x) { return x.f == f; });
    if (it == g_fonts.end()) it = g_fonts.insert(g_fonts.end(), Font{});
    Font& font = *it;
    const bool same = font.glyphs == glyphs && font.count == count && font.kind == kind;
    const bool mine = same && font.mine.size() == bytes && !std::memcmp(now, font.mine.data(), bytes) &&
                      space == font.my_space;
    if (mine && font.gen == gen) return;
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
    font.gen = gen;
    Setting set;
    {
        std::lock_guard hold(g_lock);
        set = g_set[kind];
    }
    bool made = false;
    if (set.face && set.face->ok && set.style < set.face->styles.size()) {
        std::lock_guard ft(g_ft_lock);
        made = Make(font, map, set);
    }
    if (made) {
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

// a copy in FontsDir no setting names any more, deleted
void Forget(const std::string& old) {
    if (old.empty() || !InFontsDir(ToPath(old))) return;
    for (int k = 0; k < game_fonts::kKinds; k++)
        if (game_fonts::Path(game_fonts::Kind(k)) == old) return;
    std::error_code ec;
    fs::remove(ToPath(old), ec);
}

void Changed(game_fonts::Kind k) { g_gen[k].fetch_add(1, std::memory_order_release); }

}  // namespace

std::string game_fonts::Path(Kind k) {
    std::lock_guard hold(g_lock);
    return g_set[k].face ? g_set[k].face->path : std::string();
}

void game_fonts::SetPath(Kind k, const std::string& path) {
    std::shared_ptr<const Face> face = path.empty() ? nullptr : OpenFace(path);
    if (face && !face->ok) LOGW("fonts: %s isn't a font that can be used (%s)", path.c_str(), face->error.c_str());
    {
        std::lock_guard hold(g_lock);
        g_set[k] = Setting{std::move(face), 0, 0};
    }
    Changed(k);
}

void game_fonts::Choose(Kind k, const std::string& picked) {
    const fs::path src = ToPath(picked);
    std::string use = picked;
    if (!InFontsDir(src)) {
        // the copy: the file's own name, numbered where another font already has it
        std::error_code ec;
        fs::create_directories(FontsDir(), ec);
        const std::vector<uint8_t> data = ReadAll(src);
        fs::path dst = FontsDir() / src.filename();
        for (int n = 2; fs::exists(dst, ec) && ReadAll(dst) != data && n < 100; n++)
            dst = FontsDir() / ToPath(ToUtf8(src.stem()) + " (" + std::to_string(n) + ")" + ToUtf8(src.extension()));
        if (!data.empty() && (fs::exists(dst, ec) || fs::copy_file(src, dst, ec))) use = ToUtf8(dst);
        else LOGW("fonts: couldn't copy %s into %s, using it where it is", picked.c_str(), ToUtf8(FontsDir()).c_str());
    }
    const std::string old = Path(k);
    SetPath(k, use);
    if (old != use) Forget(old);
}

void game_fonts::Clear(Kind k) {
    const std::string old = Path(k);
    SetPath(k, "");
    Forget(old);
}

std::string game_fonts::Name(Kind k) {
    std::lock_guard hold(g_lock);
    const Setting& s = g_set[k];
    if (!s.face || !s.face->ok) return {};
    const std::string& style = s.face->styles[s.style].name;
    return style.rfind(s.face->family, 0) == 0 ? style : s.face->family + " " + style;
}

std::string game_fonts::Error(Kind k) {
    std::lock_guard hold(g_lock);
    return g_set[k].face ? g_set[k].face->error : std::string();
}

std::vector<std::string> game_fonts::Styles(Kind k) {
    std::lock_guard hold(g_lock);
    std::vector<std::string> out;
    if (g_set[k].face)
        for (const Style& s : g_set[k].face->styles) out.push_back(s.name);
    return out;
}

int game_fonts::StyleIndex(Kind k) {
    std::lock_guard hold(g_lock);
    return int(g_set[k].style);
}

void game_fonts::SetStyle(Kind k, int style) {
    {
        std::lock_guard hold(g_lock);
        Setting& s = g_set[k];
        if (!s.face || style < 0 || size_t(style) >= s.face->styles.size() || size_t(style) == s.style) return;
        s.style = size_t(style);
        s.weight = 0;
    }
    Changed(k);
}

bool game_fonts::WeightRange(Kind k, int& min, int& max) {
    std::lock_guard hold(g_lock);
    const Setting& s = g_set[k];
    if (!s.face || !s.face->ok) return false;
    const Style& st = s.face->styles[s.style];
    min = st.min, max = st.max;
    return st.max > st.min;
}

int game_fonts::Weight(Kind k) {
    std::lock_guard hold(g_lock);
    const Setting& s = g_set[k];
    if (!s.face || !s.face->ok) return 0;
    return s.weight ? s.weight : s.face->styles[s.style].weight;
}

bool game_fonts::WeightSet(Kind k) {
    std::lock_guard hold(g_lock);
    return g_set[k].weight != 0;
}

void game_fonts::SetWeight(Kind k, int weight) {
    {
        std::lock_guard hold(g_lock);
        Setting& s = g_set[k];
        if (!s.face || !s.face->ok || s.weight == weight) return;
        s.weight = weight;
    }
    Changed(k);
}

#if __has_include(<toml++/toml.hpp>)
namespace {
constexpr const char* kKeys[game_fonts::kKinds] = {"title_font", "text_font"};
}

void game_fonts::Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    for (int i = 0; i < kKinds; i++) {
        const Kind k = Kind(i);
        const std::string key = kKeys[i];
        const auto path = t[key].value<std::string>();
        if (!path || path->empty()) continue;
        SetPath(k, *path);
        // the style by its face index, which stays as the file does
        if (const auto index = t[key + "_style"].value<int64_t>()) {
            std::lock_guard hold(g_lock);
            if (const auto& face = g_set[k].face)
                for (size_t s = 0; s < face->styles.size(); s++)
                    if (face->styles[s].index == FT_Long(*index)) g_set[k].style = s;
        }
        SetWeight(k, int(std::max<int64_t>(t[key + "_weight"].value_or(int64_t(0)), 0)));
        Changed(k);
    }
}

void game_fonts::Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    for (int i = 0; i < kKinds; i++) {
        const std::string key = kKeys[i];
        std::lock_guard hold(g_lock);
        const Setting& s = g_set[i];
        t.insert_or_assign(key, s.face ? s.face->path : std::string());
        t.insert_or_assign(key + "_style", int64_t(s.face && s.face->ok ? s.face->styles[s.style].index : 0));
        t.insert_or_assign(key + "_weight", int64_t(s.weight));
    }
}
#else
void game_fonts::Load(const void*) {}
void game_fonts::Save(void*) {}
#endif

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
