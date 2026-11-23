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
// 0xFFB5 LIGHTNING. The other codes are the text's own characters (Unicode). The cutscene's speech command takes a
// line's first 0xBE/0xBF for a thought's/electronic bubble's mark and starts the text after it (no line has one; in
// Spanish 0xBF is ¿, see Capture).
//
// A hint (0x2CAF90: the manual's pages, the reminders) waits on the player: its descriptor has flag 0x10 and its object
// 0 at +0x34 (0xE0850 sets 1, the hint clears it), and in state 0x20 0xE0CD0 turns its page only on a button. The
// camera leaves it room for a big bubble, so it isn't a subtitle: its page shows whole, as a panel where the game puts
// the bubble's body (its own draw run with nothing drawn, bubbles.cpp's BubblePlacing), with the key that turns it
// under it. There are five (the table at 0x6F22BC: manual_instructions, manual_catapult, remember_frying_pan,
// remember_helicopter_jump, remember_vitamins). The pad's +0x88 (A) always turns the page, +0x8C (B) closes the
// hint unless flag 0x40; with flag 0x80 (remember_vitamins) B on the last page answers no.
#include "common/subtitles.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "common/prompts.h"
#include "core/input.h"
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

// the speech lines' fixes (tools/text_fixes.py): the bubbles draw in capitals, so a lowercase "i", a name or a
// sentence's first letter never showed, and a few slips with them. Patterns in a page, not lines, so that a mod's
// text (the decensor's words are not as long as the censor's icons) keeps every fix whose surroundings it kept: the
// text a fix changes with whole words of its page about it, tied to the page's start (anchor 1) or end (2). By the
// game's language ([0x7FA1DC], menus.cpp's GameLanguage: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6
// Italian)
constexpr uint32_t kLanguage = 0x7FA1DC;
constexpr uint32_t kFrench = 4, kSpanish = 5;
struct TextFix {
    uint8_t language, anchor;
    std::u16string_view left, old, right, repl;
};
constexpr TextFix kFixes[] = {
#include "text_fixes.inc"
};

// every pattern of the language found in the page as read, the changes made in order of place, one overlapping an
// earlier one dropped (tools/text_fixes.py's apply)
std::u16string FixPage(const std::u16string& page, uint32_t language) {
    struct Edit {
        size_t at, length;
        std::u16string_view repl;
    };
    std::vector<Edit> edits;
    std::u16string needle;
    for (const TextFix& f : kFixes) {
        if (f.language != language) continue;
        needle.assign(f.left).append(f.old).append(f.right);
        for (size_t pos = page.find(needle); pos != page.npos; pos = page.find(needle, pos + 1)) {
            if ((f.anchor & 1) && pos != 0) break;
            if ((f.anchor & 2) && pos + needle.size() != page.size()) continue;
            edits.push_back({pos + f.left.size(), f.old.size(), f.repl});
        }
    }
    if (edits.empty()) return page;
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.at < b.at; });
    std::u16string out;
    size_t cur = 0;
    for (const Edit& e : edits) {
        if (e.at < cur) continue;
        out.append(page, cur, e.at - cur).append(e.repl);
        cur = e.at + e.length;
    }
    return out.append(page, cur);
}

// the voice line playing: 0x1B210 runs as it is ready and starts. Its time goes on with every frame the overlay
// draws while the game's clock runs, bubble or none: a cutscene's lines can share one clip (TrainingLevel's cut 11,
// seven lines in cut_hungover_33), the gaps between their bubbles part of its time
bool g_voice = false;
double g_voice_time = 0.0, g_voice_seen = 0.0;

struct Bubble {
    uint32_t obj = 0, page = 0, flags = 0;
    // a key-paced bubble shown whole: its body's box at full scale on the 640 by 480 frame, the page shown of how many
    bool panel = false;
    float x = 0, y = 0, w = 0, h = 0;
    int page_no = 0, pages = 1;
    bool kb = false;     // the pieces as they read with keys (KbRewrite)
    std::u16string key;  // what the panel's key line shows: the key's code, or a question's answers with theirs
    std::u16string raw;                // the page as read
    std::vector<std::u16string> pieces;  // fixed, split and tidied
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

// the game's seconds in a frame's wall seconds (at most 0.1): fast forward (display.cpp, audio::g_speed) runs them
// that many times as fast, as it does the voices the subtitles follow
double GameSeconds(double wall) {
    return std::min(wall, 0.1) * double(std::max(audio::g_speed.load(std::memory_order_relaxed), 1u));
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

// (not 0xFE: the marker)
bool Lower(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7 && c != 0xFE); }
bool Upper(char16_t c) { return (c >= u'A' && c <= u'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7); }
bool Digit(char16_t c) { return c >= u'0' && c <= u'9'; }
bool Stop(char16_t c) { return c == u'.' || c == u'!' || c == u'?' || c == 0x2026; }

// the text as the subtitles put it (tools/subtitle_times.py's tidy does the same, the timings keyed by the tidied
// line): a run of stops is the ellipsis character; two spaces are one ("Yes.  But"), three or more a line break (a
// bubble's padding); no space before a comma or a full stop ("Hello ,", "wieder ."; not French's before ! ? : ;); a
// space after a stop between a small letter and a capital ("a bit.To fool", "Huh!He's"; an ellipsis only before
// ¿ ¡, Italian's "Aspetta…degli" is how it is written; "k.o." stays); "ok" is "OK"; in French a no-break space
// before ! ? : ; (not a time's colon); no spaces or line breaks at the ends
std::u16string Tidy(const std::u16string& text, uint32_t language) {
    std::u16string s;
    s.reserve(text.size());
    for (char16_t c : text) {
        if (c == u'.' && !s.empty() && (s.back() == u'.' || s.back() == 0x2026)) s.back() = 0x2026;
        else s.push_back(c);
    }
    std::u16string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        const char16_t c = s[i];
        if (c == u' ') {
            size_t j = i;
            while (j < s.size() && s[j] == u' ') j++;
            if (j - i >= 3) {  // padding that put what follows on a line of its own ("hai capito?     A Sì B No")
                out.push_back(u'\n');
                i = j - 1;
                continue;
            }
            i = j - 1;
            const char16_t next = j < s.size() ? s[j] : 0;
            if (next == u',' || next == u'.') continue;
        }
        out.push_back(c);
        if (Stop(c) && i + 1 < s.size() && !Stop(s[i + 1])) {
            size_t a = i;  // the stop run's start
            while (a > 0 && Stop(s[a - 1])) a--;
            const bool ellipsis = s.find(char16_t(0x2026), a) <= i;
            const char16_t next = s[i + 1];
            const bool after_letter = a > 0 && (Lower(s[a - 1]) || Upper(s[a - 1]));
            if (after_letter && ((Lower(s[a - 1]) && !ellipsis && Upper(next)) || next == 0xBF || next == 0xA1))
                out.push_back(u' ');
        }
    }
    auto word = [](char16_t c) { return Lower(c) || Upper(c) || Digit(c); };
    for (size_t i = 0; i + 1 < out.size(); i++)
        if ((out[i] == u'o' || out[i] == u'O') && (out[i + 1] == u'k' || out[i + 1] == u'K') &&
            (i == 0 || !word(out[i - 1])) && (i + 2 == out.size() || !word(out[i + 2])))
            out[i] = u'O', out[i + 1] = u'K';
    if (language == kFrench) {
        auto mark = [](char16_t c) { return c == u'!' || c == u'?' || c == u':' || c == u';'; };
        std::u16string fr;
        fr.reserve(out.size() + 8);
        for (size_t i = 0; i < out.size(); i++) {
            const char16_t c = out[i];
            if (mark(c) && !fr.empty() &&
                !(c == u':' && Digit(fr.back()) && i + 1 < out.size() && Digit(out[i + 1]))) {
                if (fr.back() == u' ') fr.back() = 0xA0;
                else if (!mark(fr.back()) && fr.back() != 0xA0 && fr.back() != u'\n') fr.push_back(0xA0);
            }
            fr.push_back(c);
        }
        out.swap(fr);
    }
    const size_t from = out.find_first_not_of(u" \n"), to = out.find_last_not_of(u" \n");
    return from == out.npos ? std::u16string() : out.substr(from, to + 1 - from);
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

// player 1 last used the keyboard or the mouse, the keyboard prompts on: the buttons show as keys (any thread)
bool KeyboardInUse() { return prompts::g_on && input::LastUsed(0) == input::Source::Keyboard; }

// the sticks as the keyboard rewrite tells them apart: the one that moves and the one that turns the camera
constexpr char16_t kLeftStick = 0xFFC0, kRightStick = 0xFFC1;

// The text as it reads with keys for the buttons (KbRewrite), every line that needs it in the six languages: a
// trigger is pressed, not pulled, and no longer called a trigger; a stick's left or right goes, the key or the mouse
// in its place says which (the stick's code becomes kLeftStick or kRightStick). In a pattern \x01 is a gap (spaces,
// line breaks, piece markers); in a replacement each \x01 is the pattern's next gap as it was, and the markers of
// the gaps it leaves out go first, so a line's pieces stay as many. T stands for {L} or {R}, J for a stick
struct Rewrite {
    std::u16string_view from, to;
};
constexpr Rewrite kRewrites[] = {
    // the trigger's name ("pull the {L} trigger", "la gâchette {L}", "den Schalter {L}", ...)
    {u"the\x01T\x01trigger", u"T"},
    {u"la\x01gâchette\x01T", u"T"},
    {u"den\x01Schalter\x01T", u"T"},
    {u"il\x01grilletto\x01T", u"T"},
    {u"el\x01disparador\x01T", u"T"},
    // pulled
    {u"pull\x01T", u"press\x01T"},
    {u"Pull\x01T", u"Press\x01T"},
    {u"zieh\x01einfach\x01T", u"drück\x01einfach\x01T"},
    {u"T\x01ziehen", u"T\x01drücken"},
    {u"tira de\x01T", u"presiona\x01T"},
    {u"T\x01を引いて", u"T\x01を押して"},
    {u"T\x01を引くんだ", u"T\x01を押すんだ"},
    // the sticks' sides
    {u"left\x01J", u"\uFFC0"},      {u"right\x01J", u"\uFFC1"},
    {u"linken\x01J", u"\uFFC0"},    {u"rechten\x01J", u"\uFFC1"},
    {u"J\x01gauche", u"\uFFC0"},    {u"J\x01droit", u"\uFFC1"},
    {u"J\x01sinistra", u"\uFFC0"},  {u"J\x01destra", u"\uFFC1"},
    {u"J\x01izquierdo", u"\uFFC0"}, {u"J\x01derecho", u"\uFFC1"}, {u"derecho\x01J", u"\uFFC1"},
    {u"左の\x01J", u"\uFFC0"},      {u"右の\x01J", u"\uFFC1"},
};

bool Gap(char16_t c) { return c == u' ' || c == u'\n' || c == 0xA0 || c == 0x3000 || c == kMarker; }
// (not 0xFE, the piece marker, nor × ÷)
bool Letter(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
           (c >= 0xC0 && c < 0x250 && c != kMarker && c != 0xD7 && c != 0xF7);
}
// a pattern's letter (T and J stand for codes), which the text's word has to start or end with
bool WordEdge(char16_t c) { return c != u'T' && c != u'J' && Letter(c); }

std::u16string KbRewrite(std::u16string text) {
    for (const Rewrite& r : kRewrites) {
        std::u16string out;
        size_t i = 0;
        while (i < text.size()) {
            // the pattern at i: its gaps and what T and J stood for
            std::vector<std::u16string> gaps;
            char16_t t = 0, j = 0;
            size_t k = i;
            bool ok = !WordEdge(r.from[0]) || i == 0 || !Letter(text[i - 1]);
            for (size_t p = 0; ok && p < r.from.size(); p++) {
                const char16_t c = r.from[p];
                if (c == u'\x01') {
                    size_t e = k;
                    while (e < text.size() && Gap(text[e])) e++;
                    ok = e > k;
                    gaps.push_back(text.substr(k, e - k));
                    k = e;
                } else if (k >= text.size()) {
                    ok = false;
                } else if (c == u'T') {
                    ok = text[k] == 0xFFAF || text[k] == 0xFFB0;
                    t = text[k++];
                } else if (c == u'J') {
                    ok = text[k] == 0xFFB1 || text[k] == 0xFFBE;
                    j = text[k++];
                } else {
                    ok = text[k++] == c;
                }
            }
            if (ok && WordEdge(r.from.back()) && k < text.size() && Letter(text[k])) ok = false;
            if (!ok) {
                out.push_back(text[i++]);
                continue;
            }
            (void)j;
            size_t used = std::count(r.to.begin(), r.to.end(), u'\x01');
            for (size_t g = used; g < gaps.size(); g++)
                for (char16_t c : gaps[g])
                    if (c == kMarker) out.push_back(c);
            size_t g = 0;
            for (char16_t c : r.to) {
                if (c == u'\x01') out += gaps[std::min(g++, gaps.size() - 1)];
                else out.push_back(c == u'T' ? t : c);
            }
            i = k;
        }
        text.swap(out);
    }
    return text;
}

struct Place {
    float x, y, w, h;
};

bool FaceButton(char16_t c) { return c == 0xFFA8 || c == 0xFFA9 || c == 0xFFBC || c == 0xFFBD; }

// a question's answer: a word after its button ("{A} Yes", "{A} : Sí", "{A}\u3000ハイ"), short and on its own
bool Answer(std::u16string& w) {
    const std::u16string_view trim = u" :\n\u3000\u00A0";
    const size_t from = w.find_first_not_of(trim), to = w.find_last_not_of(trim);
    if (from == w.npos) return false;
    w = w.substr(from, to + 1 - from);
    if (w.size() > 16) return false;
    for (char16_t c : w)
        if (c == u' ' || c == u'\n' || c >= 0xFFA0) return false;
    return true;
}

// The key line of a hint's page (the page as the panel shows it, which loses the line). A page that ends in a
// question's answers ("{A} Yes  {B} No", as three of the five do in every language) has them: the answers come
// off the page. Any other, the key the text names for it (manual_instructions' "To skip it, just press {B}": the
// last A or B on the page) where the game takes it, else A, which always does
std::u16string KeyLine(std::u16string& page, uint32_t flags) {
    const size_t k2 = page.find_last_of(u"\uFFA8\uFFA9\uFFBC\uFFBD");
    if (k2 != page.npos && k2 > 0) {
        const size_t k1 = page.find_last_of(u"\uFFA8\uFFA9\uFFBC\uFFBD", k2 - 1);
        std::u16string w2 = page.substr(k2 + 1);
        std::u16string w1 = k1 == page.npos ? std::u16string() : page.substr(k1 + 1, k2 - k1 - 1);
        if (k1 != page.npos && Answer(w1) && Answer(w2)) {
            std::u16string line;
            line.push_back(page[k1]), line.append(u" ").append(w1).append(u" / ");
            line.push_back(page[k2]), line.append(u" ").append(w2);
            page.resize(k1);
            const size_t end = page.find_last_not_of(u" \n\u3000\u00A0");
            page.resize(end == page.npos ? 0 : end + 1);
            return line;
        }
    }
    const size_t named = page.find_last_of(u"\uFFA8\uFFA9");
    const bool b = named != page.npos && page[named] == 0xFFA9 && !(flags & 0x40);
    return std::u16string(1, b ? char16_t(0xFFA9) : char16_t(0xFFA8));
}

// a bubble that waits on the player's button to turn its page (a hint)
bool KeyPaced(uint32_t obj) {
    const uint32_t desc = obj ? R32(obj + 0x30) : 0;
    if (!desc || !(R32(desc) & 0x10) || R8(obj + 0x34)) return false;
    return !R32(desc + 0x2038) && !R32(desc + 0x203C) && !R32(desc + 0x2040);
}

void Capture(uint32_t obj, const Place* place = nullptr) {
    if (!obj || R8(kHidden) || !R8(obj + 0x2D)) return;
    const uint32_t desc = R32(obj + 0x30), page = R32(obj + 0x48);
    if (!desc || !page) return;

    std::u16string raw;
    for (uint32_t a = page, n = 0; n < kMaxChars; a += 2, n++) {
        const uint16_t c = R16(a);
        if (!c || c == kParagraph) break;
        raw.push_back(char16_t(c));
    }
    // the cutscene's speech command (0x569A0) takes a line's first 0xBE/0xBF for a thought's or an electronic bubble's
    // mark and starts the text past it (in its copy, desc+0x205C). No line marks itself so, and in Spanish 0xBF is ¿:
    // the four lines that start with a question lost it, in electronic bubbles. It is put back, the bubble plain
    uint32_t flags = R32(desc);
    if (R32(kLanguage) == kSpanish && page == R32(desc + 4) && page == desc + 0x205E && R16(desc + 0x205C) == 0xBF) {
        raw.insert(raw.begin(), char16_t(0xBF));
        flags &= ~kElectronic;
    }
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
    else if (running) b.elapsed += GameSeconds(now - b.last);
    b.last = now;
    b.flags = flags;
    const bool kb = KeyboardInUse();
    if (b.pieces.empty() || b.raw != raw || b.kb != kb) {  // the page's text, fixed and in pieces, again on a change
        b.raw = raw;
        b.kb = kb;
        const uint32_t language = R32(kLanguage);
        std::u16string text = raw;
        text = FixPage(text, language);
        if (kb) text = KbRewrite(text);
        b.pieces.assign(1, {});
        for (char16_t c : text) {
            if (c == kMarker) b.pieces.emplace_back();
            else b.pieces.back().push_back(c);
        }
        for (auto& p : b.pieces) p = Tidy(p, language);
    }
    const std::vector<std::u16string>& pieces = b.pieces;

    b.panel = place != nullptr;
    if (place) {  // the whole page
        b.x = place->x, b.y = place->y, b.w = place->w, b.h = place->h;
        b.page_no = int(R8(obj + 0x46)), b.pages = std::max(int(R8(desc + 0x2008)), 1);
        b.piece.clear();
        for (const auto& p : pieces) {
            if (p.empty()) continue;
            if (!b.piece.empty()) b.piece.push_back(u' ');
            b.piece += p;
        }
        b.key = KeyLine(b.piece, flags);
        return;
    }

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
// shows goes to the overlay; off, the bubble is drawn again (bubbles.cpp)
X86_EXTERN_FUNC(__imp__sub_000E1880);
X86_FUNC(sub_000E1880) {
    const uint32_t obj = R32(ctx.esp + 4);
    if (subtitles::g_on && KeyPaced(obj)) {
        subtitles::BubblePlacing(obj);
        __imp__sub_000E1880(ctx, base);
        Place place;
        const bool placed = subtitles::BubblePlaced(place.x, place.y, place.w, place.h);
        subtitles::BubblePlacing(0);
        if (placed) Capture(obj, &place);
        return;
    }
    if (!subtitles::g_on) {
        subtitles::BubbleDrawing(R32(ctx.esp + 4));
        subtitles::BubbleMeasure(ctx);
        __imp__sub_000E1880(ctx, base);
        subtitles::BubbleDrawing(0);
        return;
    }
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

#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string_view>

#include <imgui.h>

#include "common/keyboard.h"
#include "common/prompts.h"
#include "common/ui.h"
#include "core/input.h"

namespace {

// player 1 last used the keyboard or the mouse (and the keyboard prompts are on): buttons drawn as their keys. Set
// as each Draw starts
bool g_kb = false;

enum class Kind { Text, Icon, Button, Break };  // Break: a line break, a word of its own

struct Atom {
    Kind kind = Kind::Text;
    std::string text;  // UTF-8, Kind::Text
    uint16_t code = 0;
    ImU32 colour = 0;  // RGB, alpha 0 (the subtitle's alpha goes in at drawing)
    bool throb = false;
    float w = 0.0f;
    std::string key;  // Kind::Button, with g_kb: its key's name ("E", "Mouse 2", "WASD", "Mouse" the look stick)
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
// the censor's marks as letters: drawn about a capital's height, each a letter's room apart
constexpr float kIconSize = 0.7f, kIconAdvance = 0.74f;
// and shaking, each its own way: offset by up to this much of the text's size, turned by up to this many radians
constexpr float kShake = 0.035f, kShakeTurn = 0.12f;
// JOYSTICK, TXT_JOYSTICK, and KbRewrite's sticks that move and turn the camera
bool IsStick(uint16_t c) { return c == 0xFFBE || c == 0xFFB1 || c == kLeftStick || c == kRightStick; }
bool IsButton(uint16_t c) {
    return c == 0xFFA8 || c == 0xFFA9 || c == 0xFFBC || c == 0xFFBD || IsStick(c) || (c >= 0xFFAA && c <= 0xFFB0);
}

// a button's key for player 1, as their controls drive it: of the keys of the actions on the button (L and R the
// triggers) a mouse button first (B the right one, RT the left, in the default controls), else the first; the
// movement keys for the stick that moves (WASD), "Mouse" for the one that turns the camera (a stick the text gives
// no side). Empty: the console's button stays (Z, the arrows, an unbound button)
std::string KeyLabel(uint16_t code) {
    const bool left = code == kLeftStick;
    using namespace keyboard;
    const int scheme = Scheme(0);
    auto first = [](int action) -> std::string {
        for (int vk : g_keys[0][action])
            if (const char* name = vk ? KeyName(vk) : nullptr) return name;
        return {};
    };
    if (IsStick(code)) {
        if (left == Southpaw(scheme)) return "Mouse";
        std::string all, each[4];
        bool short_names = true;
        for (int i = 0; i < 4; i++) {
            each[i] = first(std::array{kMoveUp, kMoveLeft, kMoveDown, kMoveRight}[size_t(i)]);
            short_names = short_names && each[i].size() == 1;
        }
        if (short_names) return each[0] + each[1] + each[2] + each[3];
        return each[0];
    }
    int button = -1;
    switch (code) {
        case 0xFFA8: button = kButtonA; break;
        case 0xFFA9: button = kButtonB; break;
        case 0xFFBC: button = kButtonX; break;
        case 0xFFBD: button = kButtonY; break;
        case 0xFFAF: button = kButtonLT; break;
        case 0xFFB0: button = kButtonRT; break;
        default: return {};
    }
    std::string key;
    for (int a = 0; a < kActions; a++) {
        if (Drives(scheme, a) != button) continue;
        for (int vk : g_keys[0][a]) {
            const char* name = vk ? KeyName(vk) : nullptr;
            if (!name) continue;
            if (std::strncmp(name, "Mouse", 5) == 0) return name;
            if (key.empty()) key = name;
        }
    }
    return key;
}
bool IsMouse(const std::string& key) { return key == "Mouse" || key == "Mouse 1" || key == "Mouse 2" || key == "Mouse 3"; }
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
    // a censor's colour and throb end with its marks: 142 runs have no {NORMAL} after them ("kick the
    // {BLUE}{STAR}{SKULL}{LIGHTNING} out of him"), and the rest of the line took the colour. A colour with no marks
    // after it ("those {BLUE} buttons") goes on to {NORMAL} as before
    bool censor = false;
    for (size_t i = 0; i < piece.size(); i++) {
        const uint16_t c = piece[i];
        if (censor && !IsIcon(c) && !(c >= 0xFFA1 && c <= 0xFFA7)) colour = base, throb = false, censor = false;
        if (c == 0xFFA1) {
            colour = base, throb = false;
        } else if (c >= 0xFFA2 && c <= 0xFFA5) {
            colour = kColours[c - 0xFFA1];
        } else if (c == 0xFFA6 || c == 0xFFA7) {
            throb = true;
        } else if (IsIcon(c) || IsButton(c)) {
            Atom& a = atom(Kind::Icon);
            a.code = c;
            if (IsButton(c)) {
                a.kind = Kind::Button;
                if (g_kb) a.key = KeyLabel(c);
            }
            censor = IsIcon(c);
        } else if (c == ' ' || c == '\n') {  // not 0xA0: French's before ! ? : ; keeps the mark on the word's line
            if (!words.back().empty()) words.emplace_back();
            if (c == '\n') {
                words.back().push_back({Kind::Break, {}, 0, colour, false, 0.0f});
                words.emplace_back();
            }
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
        case 0xFFB1:
        case kLeftStick:
        case kRightStick:
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

// a key as the keyboard prompts draw them (light): a key cap with its name, a mouse button a mouse with the button lit
void DrawKey(ImDrawList* dl, ImFont* font, const std::string& key, ImVec2 lo, float s, float w, float alpha) {
    const int a = int(alpha * 255.0f);
    const ImU32 cap = IM_COL32(236, 236, 236, a), edge = IM_COL32(150, 150, 150, a), ink = IM_COL32(25, 25, 25, a);
    if (IsMouse(key)) {
        const float mw = s * 0.62f, mh = s * 0.92f;
        const ImVec2 m0(lo.x + (w - mw) * 0.5f, lo.y + (s - mh) * 0.5f), m1(m0.x + mw, m0.y + mh);
        const float split = m0.y + mh * 0.42f, mid = (m0.x + m1.x) * 0.5f, r = mw * 0.48f;
        dl->AddRectFilled(m0, m1, cap, r);
        // the button meant lit as the prompt sprites have it: the body's shape cut to its corner
        const ImU32 lit = IM_COL32(230, 80, 60, a);
        if (key == "Mouse 1" || key == "Mouse 2") {
            const bool l = key == "Mouse 1";
            dl->PushClipRect(ImVec2(l ? m0.x : mid, m0.y), ImVec2(l ? mid : m1.x, split), true);
            dl->AddRectFilled(m0, m1, lit, r);
            dl->PopClipRect();
        }
        const float t = std::max(1.0f, s * 0.05f);
        dl->AddRect(m0, m1, edge, r, 0, t);
        dl->AddLine(ImVec2(m0.x, split), ImVec2(m1.x, split), edge, t);
        dl->AddLine(ImVec2(mid, m0.y), ImVec2(mid, split), edge, t);
        dl->AddRectFilled(ImVec2(mid - mw * 0.09f, m0.y + mh * 0.12f), ImVec2(mid + mw * 0.09f, m0.y + mh * 0.32f),
                          key == "Mouse 3" ? lit : edge, mw * 0.09f);
        return;
    }
    std::string name = key;
    if (name.rfind("Mouse ", 0) == 0) name = "M" + name.substr(6);  // the side buttons
    const ImVec2 k0(lo.x, lo.y + s * 0.04f), k1(lo.x + w, lo.y + s * 0.96f);
    dl->AddRectFilled(ImVec2(k0.x, k0.y + s * 0.06f), ImVec2(k1.x, k1.y), edge, s * 0.2f);  // its side, below
    dl->AddRectFilled(k0, ImVec2(k1.x, k1.y - s * 0.06f), cap, s * 0.2f);
    const float fs = s * 0.58f;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, name.c_str());
    dl->AddText(font, fs, ImVec2(lo.x + (w - ts.x) * 0.5f, k0.y + (k1.y - s * 0.06f - k0.y - ts.y) * 0.5f), ink,
                name.c_str());
}

// a key's width at a text size
float KeyWidth(ImFont* font, const std::string& key, float size) {
    if (IsMouse(key)) return size * 0.75f;
    std::string name = key;
    if (name.rfind("Mouse ", 0) == 0) name = "M" + name.substr(6);
    return std::max(size * 0.95f, font->CalcTextSizeA(size * 0.95f * 0.58f, FLT_MAX, 0.0f, name.c_str()).x + size * 0.5f);
}

struct Line {
    std::vector<const Atom*> atoms;  // with nullptr for a space between words
    float w = 0.0f;
};

// each atom's width at a size
void Measure(std::vector<Word>& words, ImFont* font, float size) {
    for (Word& w : words)
        for (Atom& a : w) {
            if (a.kind == Kind::Text) a.w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, a.text.c_str()).x;
            else if (a.kind == Kind::Icon) a.w = size * kIconAdvance;
            else if (a.kind == Kind::Button && !a.key.empty()) a.w = KeyWidth(font, a.key, size) + size * 0.06f;
            else if (a.kind == Kind::Button) a.w = size * (IsTrigger(a.code) ? 1.45f : 1.0f) + size * 0.06f;
            else a.w = 0.0f;
        }
}

// the lines, filled word by word up to wrap; a line break starts a new one when breaks, else it is a space
std::vector<Line> Fill(const std::vector<Word>& words, float wrap, float space, bool breaks) {
    std::vector<Line> lines(1);
    for (const Word& w : words) {
        if (w.size() == 1 && w[0].kind == Kind::Break) {
            if (breaks) lines.emplace_back();
            continue;
        }
        float ww = 0.0f;
        for (const Atom& a : w) ww += a.w;
        if (!lines.back().atoms.empty() && lines.back().w + space + ww > wrap) lines.emplace_back();
        Line& cur = lines.back();
        if (!cur.atoms.empty()) cur.atoms.push_back(nullptr), cur.w += space;
        for (const Atom& a : w) cur.atoms.push_back(&a);
        cur.w += ww;
    }
    while (lines.size() > 1 && lines.back().atoms.empty()) lines.pop_back();
    return lines;
}

// a line from (x, y), its top
void DrawLine(ImDrawList* dl, ImFont* font, const Line& l, float x, float y, float size, float space, float alpha,
              bool thought, double now) {
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
        } else if (a->kind != Kind::Break) {
            const float scale = a->throb ? 1.0f + 0.12f * std::sin(float(now) * 9.0f + float(n)) : 1.0f;
            if (a->kind == Kind::Icon) {  // a letter's size, about a capital's middle, shaking
                const float s = size * kIconSize * scale, t = float(now), k = float(n);
                const ImVec2 c(x + a->w * 0.5f + size * kShake * std::sin(t * 37.0f + k * 1.7f),
                               y + size * 0.53f + size * kShake * std::sin(t * 29.0f + k * 2.3f + 1.0f));
                DrawIcon(dl, a->code, ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), s, col, alpha);
                const float turn = kShakeTurn * std::sin(t * 23.0f + k * 1.3f);
                const float cs = std::cos(turn), sn = std::sin(turn);
                for (int v = first_vtx; v < dl->VtxBuffer.Size; v++) {
                    ImVec2& q = dl->VtxBuffer[v].pos;
                    const float qx = q.x - c.x, qy = q.y - c.y;
                    q = ImVec2(c.x + qx * cs - qy * sn, c.y + qx * sn + qy * cs);
                }
            } else if (!a->key.empty()) {
                const float s = size * 0.95f * scale, box = a->w - size * 0.06f;
                DrawKey(dl, font, a->key, ImVec2(x + size * 0.03f, y + (size - s) * 0.5f), s, box, alpha);
            } else {
                const float s = size * 0.95f * scale;
                const float box = a->w - size * 0.06f;
                const ImVec2 at(x + size * 0.03f + (box - s * (box / size)) * 0.5f, y + (size - s) * 0.5f);
                DrawButton(dl, font, a->code, at, s, s * (box / size), alpha);
            }
        }
        // a thought in italics: its words (and the censor's marks) leaned about the line's middle; a button stays
        // upright
        if (thought && a->kind != Kind::Button)
            for (int v = first_vtx; v < dl->VtxBuffer.Size; v++)
                dl->VtxBuffer[v].pos.x += (mid - dl->VtxBuffer[v].pos.y) * 0.2f;
        x += a->w;
        n++;
    }
}

// UTF-8 as UTF-16 (the overlay's text has nothing past the BMP)
std::u16string Utf16(const char* s) {
    std::u16string out;
    for (const auto* p = reinterpret_cast<const unsigned char*>(s); *p;) {
        uint32_t c = *p++;
        int more = c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
        for (; more > 0 && (*p & 0xC0) == 0x80; more--) c = c << 6 | (*p++ & 0x3F);
        out.push_back(char16_t(c));
    }
    return out;
}

// A key-paced bubble's page whole, in a panel where the game puts the bubble's body: about its middle, as wide as
// it at least (the room the camera leaves it), wider while the page would be taller than the frame allows, kept on
// the frame. The 640 by 480 frame is shown letterboxed to FrameAspect
void DrawPanel(const Bubble& b, ImDrawList* dl, ImFont* font, double now) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    float aspect = app::FrameAspect();
    if (!(aspect > 0.1f) || !std::isfinite(aspect)) aspect = 4.0f / 3.0f;
    const float fw = std::min(display.x, display.y * aspect), fh = fw / aspect;
    const float ox = (display.x - fw) * 0.5f, oy = (display.y - fh) * 0.5f;
    const float kx = fw / 640.0f, ky = fh / 480.0f;

    const float alpha = float(std::clamp((now - b.first) / 0.15, 0.0, 1.0));
    const bool electronic = (b.flags & kElectronic) && !(b.flags & kThought);
    std::vector<Word> words = Words(b.piece, electronic ? kElectronicColour : kWhite);
    if (words.empty()) return;
    const float size = std::max(15.0f, std::round(fh / 26.0f));
    Measure(words, font, size);
    const float space = font->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
    const float pad = std::round(size * 0.75f), line_h = std::round(size * 1.24f);
    const float margin = std::round(size * 0.5f);
    const bool paged = b.pages > 1;
    // the key line under the words: "(Press [B])" in the overlay's language, the key where its %s is
    const float key_size = std::round(size * 0.82f), key_line_h = std::round(key_size * 1.3f);
    const float key_space = font->CalcTextSizeA(key_size, FLT_MAX, 0.0f, " ").x;
    std::vector<Word> key_words;
    if (!b.key.empty()) {
        std::u16string line = Utf16(ui::Text("press_key"));
        const size_t at = line.find(u"%s");
        if (at != line.npos) line.replace(at, 2, b.key);
        else line = b.key;
        key_words = Words(line, kWhite);
        Measure(key_words, font, key_size);
    }
    const std::vector<Line> key_line = Fill(key_words, FLT_MAX, key_space, false);
    const float key_w = key_words.empty() ? 0.0f : key_line[0].w;
    const float footer = key_words.empty() && !paged ? 0.0f : std::round(pad * 0.5f) + key_line_h;

    const float most_w = fw - 2 * (margin + pad), most_h = fh - 2 * (margin + pad) - footer;
    float wrap = std::clamp(b.w * kx - 2 * pad, std::min(size * 18.0f, most_w), most_w);
    std::vector<Line> lines = Fill(words, wrap, space, true);
    while (float(lines.size()) * line_h > most_h && wrap < most_w) {
        wrap = std::min(wrap * 1.15f, most_w);
        lines = Fill(words, wrap, space, true);
    }
    float block_w = key_w + (paged ? key_size * 8.0f : 0.0f);
    for (const Line& l : lines) block_w = std::max(block_w, l.w);
    const float w = block_w + 2 * pad, h = float(lines.size()) * line_h + 2 * pad + footer;
    const float cx = ox + b.x * kx, cy = oy + b.y * ky;
    auto keep = [](float v, float lo, float hi) { return hi < lo ? (lo + hi) * 0.5f : std::clamp(v, lo, hi); };
    const float x0 = std::round(keep(cx - w * 0.5f, ox + margin, ox + fw - margin - w));
    const float y0 = std::round(keep(cy - h * 0.5f, oy + margin, oy + fh - margin - h));
    const ImVec2 lo(x0, y0), hi(x0 + std::round(w), y0 + std::round(h));
    dl->AddRectFilled(lo, hi, IM_COL32(10, 12, 18, int(alpha * 205.0f)), size * 0.45f);

    for (size_t li = 0; li < lines.size(); li++) {
        const float y = y0 + pad + line_h * float(li) + (line_h - size) * 0.5f;
        DrawLine(dl, font, lines[li], x0 + pad, y, size, space, alpha, false, now);
    }
    const float key_y = hi.y - pad - key_line_h + (key_line_h - key_size) * 0.5f;
    if (!key_words.empty())  // in the middle, a little dimmer than the words
        DrawLine(dl, font, key_line[0], std::round((lo.x + hi.x - key_w) * 0.5f), key_y, key_size, key_space,
                 alpha * 0.85f, false, now);
    if (paged) {  // the page of how many, on the right
        char t[16];
        std::snprintf(t, sizeof t, "%d / %d", std::clamp(b.page_no + 1, 1, b.pages), b.pages);
        const float fs = std::round(size * 0.72f);
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t);
        dl->AddText(font, fs, ImVec2(hi.x - pad - ts.x, key_y + (key_size - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, int(alpha * 140.0f)), t);
    }
}

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
        if (g_voice && !kernel::Held() && !R8(kPaused)) g_voice_time += GameSeconds(now - g_voice_seen);
        g_voice_seen = now;
        if (!kernel::Held())
            std::erase_if(g_bubbles, [&](const Bubble& b) { return now - b.last > kGone; });
        shown = g_bubbles;
    }
    if (shown.empty()) return;
    std::sort(shown.begin(), shown.end(), [](const Bubble& a, const Bubble& b) { return a.first > b.first; });
    g_kb = prompts::g_on && input::LastUsed(0) == input::Source::Keyboard;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImFont* font = ImGui::GetFont();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float size = std::max(16.0f, std::round(display.y / 24.0f));
    const float wrap = std::round(std::min(display.x * 0.72f, size * 34.0f));
    const float space = font->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x;
    const float pad_x = std::round(size * 0.5f), pad_y = std::round(size * 0.22f), line_h = std::round(size * 1.18f);
    const double now = Now();
    float bottom = std::round(display.y * 0.93f);

    for (const Bubble& b : shown)
        if (b.panel) DrawPanel(b, dl, font, now);

    // the newest at the bottom, the older above it
    for (const Bubble& b : shown) {
        if (b.panel) continue;
        const float alpha = float(std::clamp((now - b.first) / 0.15, 0.0, 1.0));
        const bool thought = b.flags & kThought, electronic = (b.flags & kElectronic) && !thought;
        std::vector<Word> words = Words(b.piece, electronic ? kElectronicColour : kWhite);
        if (words.empty()) continue;
        Measure(words, font, size);
        const std::vector<Line> lines = Fill(words, wrap, space, false);

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
            DrawLine(dl, font, l, std::round((display.x - l.w) * 0.5f), y, size, space, alpha, thought, now);
        }
        bottom = lo.y - std::round(size * 0.35f) - pad_y;
    }
}

#else  // no ImGui here (the headless harness)

void subtitles::Draw() {}

#endif
