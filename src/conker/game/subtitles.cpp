// Speech bubbles as subtitles (common/subtitles.h).
//
// A bubble is an object of effect layer 0x7C: 0xE0850 makes one for a descriptor (the cutscene speech command
// 0x569A0, the hint bubble 0x2CAF90, the world bubble 0xE0500), 0xE0CD0 runs it each tick and 0xE1880 draws it, its
// shape and then its text (0xE2940, its only caller). With the setting on, the hook on 0xE1880 draws nothing and
// keeps what the bubble shows for the overlay; the bubble itself runs on, so its timing is the game's.
//
// The object: +0x2D drawn yet, +0x30 its descriptor, +0x48 the page it shows (in the descriptor's text, which 0x1F3110
// has turned from {TOKEN}s into codes, see below). The descriptor: +0 flags (2 a thought, 8 "electronic"), +0x2009 the
// pieces of the page still to come, +0x2038/+0x203C/+0x2040 the cutscene slot, actor and command (all 0: no script),
// +0x2044 the page's time in 60ths of a second.
//
// A line's pages end at 0xBD ({PARAGRAPH}) and its pieces at 0xFE ({PMARKER}). In a cutscene the script times each
// piece: a speech command opens the bubble, each "advance" (0xE3690, flag 0x100) takes a piece off +0x2009 (0xE0CD0)
// and at 0 the bubble turns the page; the bubble scrolls over the page meanwhile. So the piece shown is the page's
// count less what is left. A bubble with no script (a hint) gets a time for the page (length x 6 ticks) and no
// advances: its piece goes by the share of the page's time gone, each piece's share its length's.
//
// Codes (0x1F3110 with the names at 0x6B9D98 and the codes at 0x6B9E28): 0xFFA1 NORMAL, 0xFFA2-A5 RED GREEN BLUE
// YELLOW, 0xFFA6 THROB, 0xFFA7 JIGGLE, buttons 0xFFA8 A, 0xFFA9 B, 0xFFBC X, 0xFFBD Y, 0xFFBE JOYSTICK, 0xFFAE Z,
// 0xFFAF L, 0xFFB0 R, arrows 0xFFAA-AD (left right up down), the censor's 0xFFB2 SKULL, 0xFFB3 SWIRL, 0xFFB4 STAR,
// 0xFFB5 LIGHTNING. The other codes are the text's own characters (Unicode). A line's first 0xBE/0xBF picks a
// thought/electronic bubble and the page starts after it.
#include "common/subtitles.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

bool subtitles::g_on = false;

namespace {

using kernel::R16;
using kernel::R32;

uint8_t R8(uint32_t va) { return kernel::G<uint8_t>(va); }

constexpr uint32_t kHidden = 0x847150;  // the bubbles are drawn not at all (0xE1880's first test)
constexpr uint32_t kPaused = 0x7FA275;  // the bubbles' clock stands (0xE2940)
constexpr uint32_t kThought = 2, kElectronic = 8;
constexpr uint16_t kParagraph = 0xBD, kMarker = 0xFE;
constexpr size_t kMaxChars = 2048;
constexpr double kGone = 0.25;  // seconds undrawn before a bubble's subtitle goes, while the game runs

constexpr size_t kLong = 90;  // a piece longer than this goes a sentence at a time (tools/subtitle_times.py's kLong)

// the lines with no markers, timed a sentence at a time from their voice (tools/subtitle_times.py)
struct Sentence {
    uint16_t at;  // the code it starts at
    uint32_t ms;  // from the voice's start
};
constexpr size_t kMaxSentences = 24;
struct Timed {
    uint32_t hash;  // FNV-1a over the line's codes, low byte first
    uint16_t length;
    uint32_t end;  // the voice's last word's end, ms
    Sentence s[kMaxSentences];
};
constexpr Timed kTimed[] = {
#include "subtitle_times.inc"
};

// the voice line playing: 0x1B210 runs as it is ready and starts. Its time goes on with the frames the bubbles are
// drawn on and the game's clock running (game thread)
bool g_voice = false;
double g_voice_time = 0.0, g_voice_seen = 0.0;

struct Bubble {
    uint32_t obj = 0, page = 0, flags = 0;
    std::u16string piece;
    double first = 0.0, last = 0.0;
    double elapsed = 0.0;  // the seconds drawn on this page, the game's clock running
};

std::mutex g_lock;
std::vector<Bubble> g_bubbles;

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint32_t Fnv(const std::u16string& s) {
    uint32_t h = 0x811C9DC5u;
    for (char16_t c : s) {
        h = (h ^ (uint32_t(c) & 0xFF)) * 0x01000193u;
        h = (h ^ (uint32_t(c) >> 8)) * 0x01000193u;
    }
    return h;
}

const Timed* FindTimed(const std::u16string& s) {
    const uint32_t h = Fnv(s);
    for (const Timed& t : kTimed)
        if (t.hash == h && t.length == s.size()) return &t;
    return nullptr;
}

// where a long piece's sentences start: after . ! ? (and ... 。！？) and spaces, or a line break; a short one stays
// with the one before
std::vector<size_t> Sentences(const std::u16string& s) {
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < s.size(); i++) {
        const char16_t c = s[i];
        size_t next = 0;
        if (c == u'\n' || c == u'\u3002' || c == u'\uFF01' || c == u'\uFF1F') {
            next = i + 1;
        } else if (c == u'.' || c == u'!' || c == u'?' || c == u'\u2026') {
            size_t j = i + 1;
            while (j < s.size() && (s[j] == u'.' || s[j] == u'!' || s[j] == u'?' || s[j] == u'"' || s[j] == u'\'')) j++;
            if (j < s.size() && (s[j] == u' ' || s[j] == 0xA0)) {
                while (j < s.size() && (s[j] == u' ' || s[j] == 0xA0)) j++;
                next = j;
            }
            i = j - 1;
        }
        while (next < s.size() && s[next] == u'\n') next++;
        if (next && next < s.size() && next - starts.back() >= 28) starts.push_back(next);
    }
    return starts;
}

// the piece's sentence for a time: by the voice's timings when the line has them, else by each sentence's share of
// the characters over the page's time
std::u16string SentenceOf(const std::u16string& piece, double elapsed, double page_time) {
    size_t from = 0, to = piece.size();
    const Timed* t = FindTimed(piece);
    const double voice = g_voice_time * 1000.0;
    if (t && g_voice && voice < double(t->end) + 4000.0) {
        size_t n = 1;
        while (n < kMaxSentences && t->s[n].at) n++;
        size_t k = 0;
        while (k + 1 < n && double(t->s[k + 1].ms) <= voice) k++;
        from = t->s[k].at;
        to = k + 1 < n ? t->s[k + 1].at : piece.size();
    } else {
        const std::vector<size_t> starts = Sentences(piece);
        const double at = page_time > 0.0 ? std::min(elapsed / page_time, 1.0) * double(piece.size()) : 0.0;
        size_t k = 0;
        while (k + 1 < starts.size() && double(starts[k + 1]) <= at) k++;
        from = starts[k];
        to = k + 1 < starts.size() ? starts[k + 1] : piece.size();
    }
    return piece.substr(std::min(from, piece.size()), to > from ? to - from : 0);
}

void Capture(uint32_t obj) {
    if (!obj || R8(kHidden) || !R8(obj + 0x2D)) return;
    const uint32_t desc = R32(obj + 0x30), page = R32(obj + 0x48);
    if (!desc || !page) return;

    std::vector<std::u16string> pieces(1);
    for (uint32_t a = page, n = 0; n < kMaxChars; a += 2, n++) {
        const uint16_t c = R16(a);
        if (!c || c == kParagraph) break;
        if (c == kMarker) pieces.emplace_back();
        else pieces.back().push_back(char16_t(c));
    }
    const uint32_t flags = R32(desc);
    const bool scripted = R32(desc + 0x2038) || R32(desc + 0x203C) || R32(desc + 0x2040);
    const double now = Now();

    std::lock_guard lock(g_lock);
    auto it = std::find_if(g_bubbles.begin(), g_bubbles.end(), [&](const Bubble& b) { return b.obj == obj; });
    if (it == g_bubbles.end() || now - it->last > kGone) {
        if (it == g_bubbles.end()) it = g_bubbles.emplace(g_bubbles.end());
        *it = Bubble{};
        it->obj = obj;
        it->first = it->last = now;
    }
    Bubble& b = *it;
    const bool running = !R8(kPaused);
    if (b.page != page) b.page = page, b.elapsed = 0.0;
    else if (running) b.elapsed += std::min(now - b.last, 0.1);
    b.last = now;
    if (g_voice && running) g_voice_time += std::min(now - g_voice_seen, 0.1);
    g_voice_seen = now;
    b.flags = flags;

    size_t index = 0;
    if (scripted) {
        const size_t left = R8(desc + 0x2009);
        index = pieces.size() > left ? pieces.size() - left : 0;
    } else {
        const double time = double(int32_t(R32(desc + 0x2044))) / 60.0;
        size_t total = 0;
        for (const auto& p : pieces) total += p.size() + 1;
        const double at = time > 0.0 ? std::min(b.elapsed / time, 1.0) * double(total) : 0.0;
        for (size_t sum = 0; index + 1 < pieces.size(); index++) {
            sum += pieces[index].size() + 1;
            if (double(sum) > at) break;
        }
    }
    b.piece = pieces[std::min(index, pieces.size() - 1)];
    // a line with no markers, one long piece: a sentence at a time
    if (pieces.size() == 1 && b.piece.size() > kLong)
        b.piece = SentenceOf(b.piece, b.elapsed, double(int32_t(R32(desc + 0x2044))) / 60.0);
}

}  // namespace

// [[hook]] 0xE1880 draws a bubble (cdecl, [esp+4] the object). With the setting on it draws nothing: what the bubble
// shows goes to the overlay
X86_EXTERN_FUNC(__imp__sub_000E1880);
X86_FUNC(sub_000E1880) {
    if (!subtitles::g_on) return __imp__sub_000E1880(ctx, base);
    Capture(R32(ctx.esp + 4));
    kernel::Return(ctx, ctx.eax, 0);
}

// [[hook]] 0x1B210: the voice line 0x1AF70 started is ready and plays (XACT's notification). Its clock starts
X86_EXTERN_FUNC(__imp__sub_0001B210);
X86_FUNC(sub_0001B210) {
    __imp__sub_0001B210(ctx, base);
    std::lock_guard lock(g_lock);
    g_voice = true;
    g_voice_time = 0.0;
    g_voice_seen = Now();
}

#if __has_include(<imgui.h>) && !defined(XBE_HEADLESS)

#include <cmath>

#include <imgui.h>

namespace {

enum class Kind { Text, Icon, Button };

struct Atom {
    Kind kind = Kind::Text;
    std::string text;  // UTF-8, Kind::Text
    uint16_t code = 0;
    ImU32 colour = 0;  // RGB, alpha 0 (the subtitle's alpha goes in at drawing)
    bool throb = false;
    float w = 0.0f;
};
using Word = std::vector<Atom>;

constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 0);
constexpr ImU32 kColours[] = {kWhite, IM_COL32(255, 80, 70, 0), IM_COL32(90, 220, 90, 0), IM_COL32(110, 160, 255, 0),
                              IM_COL32(255, 215, 70, 0)};
constexpr ImU32 kElectronicColour = IM_COL32(170, 235, 255, 0);

ImU32 WithAlpha(ImU32 rgb, float alpha) {
    return (rgb & 0x00FFFFFFu) | (ImU32(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT);
}

void AppendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) {
        s += char(c);
    } else if (c < 0x800) {
        s += char(0xC0 | c >> 6), s += char(0x80 | (c & 0x3F));
    } else {
        s += char(0xE0 | c >> 12), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
    }
}

bool IsIcon(uint16_t c) { return c >= 0xFFB2 && c <= 0xFFB5; }
bool IsButton(uint16_t c) {
    return c == 0xFFA8 || c == 0xFFA9 || c == 0xFFBC || c == 0xFFBD || c == 0xFFBE || (c >= 0xFFAA && c <= 0xFFB0);
}
bool IsTrigger(uint16_t c) { return c == 0xFFAF || c == 0xFFB0; }

// the piece as words (wrapped only between them), each atom in its colour
std::vector<Word> Words(const std::u16string& piece, ImU32 base) {
    std::vector<Word> words(1);
    ImU32 colour = base;
    bool throb = false;
    auto atom = [&](Kind kind) -> Atom& {
        Word& w = words.back();
        if (kind != Kind::Text || w.empty() || w.back().kind != Kind::Text || w.back().colour != colour ||
            w.back().throb != throb) {
            w.push_back({kind, {}, 0, colour, throb, 0.0f});
        }
        return w.back();
    };
    for (size_t i = 0; i < piece.size(); i++) {
        const uint16_t c = piece[i];
        if (i == 0 && (c == 0xBE || c == 0xBF)) continue;  // a thought's or an electronic bubble's mark
        if (c == 0xFFA1) {
            colour = base, throb = false;
        } else if (c >= 0xFFA2 && c <= 0xFFA5) {
            colour = kColours[c - 0xFFA1];
        } else if (c == 0xFFA6 || c == 0xFFA7) {
            throb = true;
        } else if (IsIcon(c) || IsButton(c)) {
            atom(Kind::Icon).code = c;
            if (IsButton(c)) words.back().back().kind = Kind::Button;
        } else if (c == ' ' || c == '\n' || c == 0xA0) {
            if (!words.back().empty()) words.emplace_back();
        } else if (c < 0xFFA0 && c != 0x3E8) {
            AppendUtf8(atom(Kind::Text).text, c);
        }
    }
    if (words.back().empty()) words.pop_back();
    return words;
}

void DrawIcon(ImDrawList* dl, uint16_t code, ImVec2 lo, float s, ImU32 col, float alpha) {
    auto P = [&](float x, float y) { return ImVec2(lo.x + x * s, lo.y + y * s); };
    const ImU32 dark = IM_COL32(0, 0, 0, int(alpha * 255.0f));
    const ImVec2 c = P(0.5f, 0.5f);
    switch (code) {
        case 0xFFB2: {  // skull
            dl->AddCircleFilled(P(0.5f, 0.42f), s * 0.34f, col);
            dl->AddRectFilled(P(0.3f, 0.55f), P(0.7f, 0.88f), col, s * 0.08f);
            dl->AddCircleFilled(P(0.37f, 0.45f), s * 0.09f, dark);
            dl->AddCircleFilled(P(0.63f, 0.45f), s * 0.09f, dark);
            dl->AddTriangleFilled(P(0.5f, 0.56f), P(0.45f, 0.66f), P(0.55f, 0.66f), dark);
            for (float x : {0.42f, 0.5f, 0.58f}) dl->AddLine(P(x, 0.74f), P(x, 0.88f), dark, std::max(1.0f, s * 0.04f));
            break;
        }
        case 0xFFB3: {  // swirl
            ImVec2 pts[48];
            for (int i = 0; i < 48; i++) {
                const float t = float(i) / 47.0f * 3.0f * 3.14159265f;
                const float r = s * (0.05f + 0.37f * t / (3.0f * 3.14159265f));
                pts[i] = ImVec2(c.x + r * std::cos(t), c.y + r * std::sin(t));
            }
            dl->AddPolyline(pts, 48, col, ImDrawFlags_None, std::max(1.5f, s * 0.11f));
            break;
        }
        case 0xFFB4: {  // star
            ImVec2 pts[10];
            for (int i = 0; i < 10; i++) {
                const float a = -3.14159265f * 0.5f + float(i) * 3.14159265f / 5.0f;
                const float r = s * (i & 1 ? 0.2f : 0.47f);
                pts[i] = ImVec2(c.x + r * std::cos(a), c.y + 0.03f * s + r * std::sin(a));
            }
            for (int i = 0; i < 10; i++) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
            break;
        }
        case 0xFFB5: {  // lightning
            const ImVec2 pts[] = {P(0.62f, 0.02f), P(0.18f, 0.56f), P(0.46f, 0.56f), P(0.32f, 0.98f),
                                  P(0.84f, 0.4f),  P(0.55f, 0.4f),  P(0.74f, 0.02f)};
            dl->AddConcavePolyFilled(pts, 7, col);
            break;
        }
        default:
            break;
    }
}

// a controller's button as the console draws them: a face button its coloured disc and letter, a trigger a pill
void DrawButton(ImDrawList* dl, ImFont* font, uint16_t code, ImVec2 lo, float s, float w, float alpha) {
    const ImU32 white = IM_COL32(255, 255, 255, int(alpha * 255.0f)), black = IM_COL32(0, 0, 0, int(alpha * 255.0f));
    const ImVec2 c(lo.x + w * 0.5f, lo.y + s * 0.5f);
    auto label = [&](const char* t, ImU32 col) {
        const float fs = s * 0.62f;
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t);
        dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
    };
    auto disc = [&](int r, int g, int b, const char* t, ImU32 text) {
        dl->AddCircleFilled(c, s * 0.44f, IM_COL32(r, g, b, int(alpha * 255.0f)));
        label(t, text);
    };
    switch (code) {
        case 0xFFA8: disc(90, 180, 60, "A", white); break;
        case 0xFFA9: disc(210, 60, 50, "B", white); break;
        case 0xFFBC: disc(60, 120, 220, "X", white); break;
        case 0xFFBD: disc(235, 195, 40, "Y", black); break;
        case 0xFFAE: disc(225, 225, 225, "Z", black); break;
        case 0xFFBE:  // the stick
            dl->AddCircleFilled(c, s * 0.44f, IM_COL32(90, 90, 90, int(alpha * 255.0f)));
            dl->AddCircleFilled(c, s * 0.26f, IM_COL32(40, 40, 40, int(alpha * 255.0f)));
            break;
        case 0xFFAF:
        case 0xFFB0:
            dl->AddRectFilled(ImVec2(lo.x, lo.y + s * 0.1f), ImVec2(lo.x + w, lo.y + s * 0.9f),
                              IM_COL32(80, 80, 80, int(alpha * 255.0f)), s * 0.4f);
            label(code == 0xFFAF ? "LT" : "RT", white);
            break;
        default: {  // the arrows: left, right, up, down
            const float r = s * 0.36f;
            static const float kAngle[] = {3.14159265f, 0.0f, -3.14159265f * 0.5f, 3.14159265f * 0.5f};
            const float a = kAngle[std::min(code - 0xFFAA, 3)];
            auto at = [&](float da, float rr) { return ImVec2(c.x + rr * std::cos(a + da), c.y + rr * std::sin(a + da)); };
            dl->AddTriangleFilled(at(0.0f, r), at(2.3f, r), at(-2.3f, r), white);
            break;
        }
    }
}

struct Line {
    std::vector<const Atom*> atoms;  // with nullptr for a space between words
    float w = 0.0f;
};

}  // namespace

void subtitles::Draw() {
    std::vector<Bubble> shown;
    {
        std::lock_guard lock(g_lock);
        if (!g_on) {
            g_bubbles.clear();
            return;
        }
        const double now = Now();
        if (!kernel::Held())
            std::erase_if(g_bubbles, [&](const Bubble& b) { return now - b.last > kGone; });
        shown = g_bubbles;
    }
    if (shown.empty()) return;
    std::sort(shown.begin(), shown.end(), [](const Bubble& a, const Bubble& b) { return a.first > b.first; });

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImFont* font = ImGui::GetFont();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float size = std::max(16.0f, std::round(display.y / 24.0f));
    const float wrap = std::round(std::min(display.x * 0.72f, size * 34.0f));
    const float space = font->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
    const float pad_x = std::round(size * 0.5f), pad_y = std::round(size * 0.22f), line_h = std::round(size * 1.18f);
    const double now = Now();
    float bottom = std::round(display.y * 0.93f);

    // the newest at the bottom, the older above it
    for (const Bubble& b : shown) {
        const float alpha = float(std::clamp((now - b.first) / 0.15, 0.0, 1.0));
        const bool thought = b.flags & kThought, electronic = (b.flags & kElectronic) && !thought;
        std::vector<Word> words = Words(b.piece, electronic ? kElectronicColour : kWhite);
        if (words.empty()) continue;

        // measure, then fill the lines word by word
        std::vector<Line> lines(1);
        for (Word& w : words) {
            float ww = 0.0f;
            for (Atom& a : w) {
                if (a.kind == Kind::Text) a.w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, a.text.c_str()).x;
                else a.w = size * (a.kind == Kind::Button && IsTrigger(a.code) ? 1.45f : 1.0f) + size * 0.06f;
                ww += a.w;
            }
            Line& l = lines.back();
            if (!l.atoms.empty() && l.w + space + ww > wrap) lines.emplace_back();
            Line& cur = lines.back();
            if (!cur.atoms.empty()) cur.atoms.push_back(nullptr), cur.w += space;
            for (const Atom& a : w) cur.atoms.push_back(&a);
            cur.w += ww;
        }

        float block_w = 0.0f;
        for (const Line& l : lines) block_w = std::max(block_w, l.w);
        const float block_h = line_h * float(lines.size());
        const float top = bottom - block_h;
        const ImVec2 lo(std::round((display.x - block_w) * 0.5f) - pad_x, top - pad_y);
        const ImVec2 hi(std::round((display.x + block_w) * 0.5f) + pad_x, bottom + pad_y);
        dl->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, int(alpha * 150.0f)), size * 0.3f);

        for (size_t li = 0; li < lines.size(); li++) {
            const Line& l = lines[li];
            const float y = top + line_h * float(li) + (line_h - size) * 0.5f;
            float x = std::round((display.x - l.w) * 0.5f);
            const float mid = y + size * 0.5f;
            int n = 0;
            for (const Atom* a : l.atoms) {
                if (!a) {
                    x += space;
                    continue;
                }
                const ImU32 col = WithAlpha(a->colour, alpha);
                const int first_vtx = dl->VtxBuffer.Size;
                if (a->kind == Kind::Text) {
                    dl->AddText(font, size, ImVec2(x, y), col, a->text.c_str());
                } else {
                    const float scale = a->throb ? 1.0f + 0.12f * std::sin(float(now) * 9.0f + float(n)) : 1.0f;
                    const float s = size * 0.95f * scale;
                    const float box = a->w - size * 0.06f;
                    const ImVec2 at(x + size * 0.03f + (box - s * (box / size)) * 0.5f, y + (size - s) * 0.5f);
                    if (a->kind == Kind::Icon) DrawIcon(dl, a->code, at, s, col, alpha);
                    else DrawButton(dl, font, a->code, at, s, s * (box / size), alpha);
                }
                // a thought in italics: its words (and the censor's marks) leaned about the line's middle; a button
                // stays upright
                if (thought && a->kind != Kind::Button)
                    for (int v = first_vtx; v < dl->VtxBuffer.Size; v++)
                        dl->VtxBuffer[v].pos.x += (mid - dl->VtxBuffer[v].pos.y) * 0.2f;
                x += a->w;
                n++;
            }
        }
        bottom = lo.y - std::round(size * 0.35f) - pad_y;
    }
}

#else  // no ImGui here (the headless harness)

void subtitles::Draw() {}

#endif
