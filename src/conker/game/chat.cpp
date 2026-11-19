// System link text chat (chat.h).
//
// While this copy is in a system link session (net state 1..5, syslink::InSession: joining, the waiting room, a match),
// the Chat key (keyboard::kChat, T by default, any player's) opens a line to type in at the bottom left: to the team,
// or with Shift held to everyone. The window's text input goes on and the keyboard drives no pad (keyboard::Pad, as
// for the on-screen keyboard). Enter sends what was typed, Escape drops it (and is not the game's Start until let go),
// Backspace takes back a character; F1 opening the menu, or the session ending, drops it too.
// A team line goes to everyone and only the sender's teammates show it. Out of a match (no teams yet), or with no
// teammate in it (a free-for-all, where each player is a team of its own), a team line is everyone's.
//
// A line goes to the session's other copies as the runtime's session packet (xnet::SendSession: to the endpoints the
// game talks to, with the session's XNKID, past the game's sockets and networklib, so nothing the game reads changes
// and no protocol version moves): version 2, flags (bit 0: to the team), the sender's team (int32, -1 out of a
// match), its name's length and name, then the text, all UTF-8, at most kMaxName and kMaxText bytes. A receiver shows it only while it is in a
// session itself. Each line said shows for kShowSeconds and fades; while typing, the last kLines show in full.
//
// The name is the profile name of this copy's first player, as the name tags have it (0x348480 -> 0x261A90 ->
// 0x299D60): the local players' indices are the signed bytes at 0x862CCB (two, -1 none; the HitMsg sender 0x281B80
// tests the attacker against them), and each machine of the table at 0x864EC8 (0xA50 bytes, the count the byte at
// 0x864EC1) has two slots: bit n of the byte +0x758 the slot used, the dword +0xA3C + 4n its player's index, and when
// the byte +0x7BC + n is set, its name at +0x794 + 17n (16 characters). Without one: "Player".
// The team, in a match ([0x7819D5]), is the dword the player's record's +0x238 points to (friendly_fire.cpp): the
// record of the table at [0x84A5F8] (0x6D0 bytes, 0x80 of them) of kind 1 (+0x64) whose word +0x60 is the index. A
// name is coloured as the receiver's name tags would colour that player: its team green (blue), another red (orange),
// out of a match light blue (display::g_team_colors).
//
// XBE_CHAT_TRACE=1 logs each line sent and received, and this copy's name and team as they change.
#include "common/chat.h"
#include "core/diag.h"

#if __has_include(<imgui.h>) && !defined(XBE_HEADLESS)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <imgui.h>

#include "app/app.h"
#include "common/display.h"
#include "common/keyboard.h"
#include "common/link.h"
#include "common/menus.h"
#include "common/ui.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace xnet {
int SendSession(const uint8_t* data, int len);                        // runtime xdk/xnet.cpp
void SetSessionReceiver(void (*receive)(const uint8_t* data, int len));  // runtime xdk/xnet.cpp
}  // namespace xnet

namespace {

constexpr uint32_t kLocalPlayers = 0x862CCB;  // two signed bytes: this copy's players' indices, -1 none
constexpr uint32_t kMachines = 0x864EC8, kMachineSize = 0xA50, kMachineCount = 0x864EC1;
constexpr uint32_t kSlotMask = 0x758, kSlotPlayer = 0xA3C, kSlotNamed = 0x7BC, kSlotName = 0x794, kSlotNameSize = 17;
constexpr uint32_t kRecords = 0x84A5F8, kRecordSize = 0x6D0, kRecordCount = 0x80;
constexpr uint32_t kKind = 0x64, kKindPlayer = 1, kIndex = 0x60, kTeam = 0x238;
constexpr uint32_t kMatchFlag = 0x7819D5;

constexpr uint8_t kVersion = 2;
constexpr uint8_t kToTeam = 1;  // flags
constexpr size_t kMaxName = 16, kMaxText = 360;  // bytes
constexpr size_t kMaxTyped = 120;                // characters
constexpr size_t kLog = 32, kLines = 8;
constexpr double kShowSeconds = 12.0, kFadeSeconds = 1.5;
constexpr int kEnter = 0x0D, kEscape = 0x1B, kBackspace = 0x08, kShift = 0x10, kLShift = 0xA0, kRShift = 0xA1;

enum Colour : uint8_t { kNeutral, kMate, kFoe };

struct Line {
    std::string name, text;
    Colour colour;
    bool team;  // to the team
    double at;  // Now() when it came
};

std::mutex g_lock;            // all below
std::deque<Line> g_log;
bool g_typing = false;
bool g_to_team = false;  // the line typed: to the team
int g_open_vk = 0;       // the key that opened it
std::string g_typed;
double g_opened_at = 0;
uint16_t g_high_surrogate = 0;  // a character past U+FFFF sent as two UTF-16 units (SDL sends code points)
std::string g_name = "Player";  // this copy's, as Tick found it

std::atomic<bool> g_in_session{false};
std::atomic<bool> g_typing_flag{false};
std::atomic<int32_t> g_team{-1};
std::atomic<bool> g_has_mates{false};  // another player of the match is on this copy's team

// a team line now: in a match, with a teammate in it
bool TeamLine(bool asked) { return asked && g_team.load() >= 0 && g_has_mates.load(); }

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool Trace() {
    static const bool on = [] {
        const char* v = diag::Env("XBE_CHAT_TRACE");
        return v && *v && *v != '0';
    }();
    return on;
}

void Append(std::string& s, uint32_t c) {
    if (c < 0x80) {
        s += char(c);
    } else if (c < 0x800) {
        s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
    } else {
        s += char(0xF0 | (c >> 18)), s += char(0x80 | ((c >> 12) & 0x3F)), s += char(0x80 | ((c >> 6) & 0x3F));
        s += char(0x80 | (c & 0x3F));
    }
}

// valid UTF-8 from what came, control characters dropped, cut to at most max bytes on a character's boundary
std::string Clean(const uint8_t* p, size_t n, size_t max) {
    std::string out;
    for (size_t i = 0; i < n;) {
        const uint8_t b = p[i];
        const int more = b >= 0xF0 && b < 0xF8 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : b < 0x80 ? 0 : -1;
        if (more < 0 || i + size_t(more) >= n) {  // not a lead byte, or cut short
            i++;
            continue;
        }
        uint32_t c = more == 3 ? b & 0x07 : more == 2 ? b & 0x0F : more == 1 ? b & 0x1F : b;
        bool ok = true;
        for (int k = 1; k <= more; k++) {
            if ((p[i + k] & 0xC0) != 0x80) ok = false;
            c = c << 6 | (p[i + k] & 0x3F);
        }
        i += size_t(more) + 1;
        if (!ok || c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF)
            continue;
        std::string one;
        Append(one, c);
        if (out.size() + one.size() > max) break;
        out += one;
    }
    return out;
}

size_t Characters(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

void Add(Line line) {  // with g_lock held
    g_log.push_back(std::move(line));
    while (g_log.size() > kLog) g_log.pop_front();
}

void Close() {  // with g_lock held
    if (!g_typing) return;
    g_typing = false;
    g_typing_flag = false;
    g_typed.clear();
    g_high_surrogate = 0;
    app::TextInput(false);
}

Colour ColourOf(int32_t theirs) {
    const int32_t ours = g_team.load();
    if (theirs < 0 || ours < 0) return kNeutral;
    return theirs == ours ? kMate : kFoe;
}

// on the link's thread
void Receive(const uint8_t* data, int len) {
    constexpr int kHead = 7;  // version, flags, team, name length
    if (!g_in_session.load() || len < kHead || data[0] != kVersion) return;
    const bool to_team = (data[1] & kToTeam) != 0;
    int32_t team;
    std::memcpy(&team, data + 2, 4);
    const size_t name_len = data[6];
    if (kHead + name_len > size_t(len)) return;
    std::string name = Clean(data + kHead, name_len, kMaxName);
    std::string text = Clean(data + kHead + name_len, size_t(len) - kHead - name_len, kMaxText);
    if (text.empty()) return;
    if (name.empty()) name = "Player";
    const Colour colour = ColourOf(team);
    const bool shown = !to_team || colour == kMate;
    if (Trace())
        LOGR("chat: from %s (team %d%s)%s: %s", name.c_str(), team, to_team ? ", to its team" : "",
             shown ? "" : " not shown, another team's", text.c_str());
    if (!shown) return;
    std::lock_guard lock(g_lock);
    Add({std::move(name), std::move(text), colour, to_team, Now()});
}

const bool g_receiver_set = [] {
    xnet::SetSessionReceiver(Receive);
    return true;
}();

// with g_lock held: what was typed, to the session and the log
void Send() {
    std::string text = Clean(reinterpret_cast<const uint8_t*>(g_typed.data()), g_typed.size(), kMaxText);
    if (text.find_first_not_of(' ') == std::string::npos) return;
    const int32_t team = g_team.load();
    const bool to_team = TeamLine(g_to_team);
    std::string packet;
    packet += char(kVersion);
    packet += char(to_team ? kToTeam : 0);
    packet.append(reinterpret_cast<const char*>(&team), 4);
    packet += char(g_name.size());
    packet += g_name;
    packet += text;
    const int to = xnet::SendSession(reinterpret_cast<const uint8_t*>(packet.data()), int(packet.size()));
    if (Trace())
        LOGR("chat: sent to %d (as %s, team %d%s): %s", to, g_name.c_str(), team, to_team ? ", to the team" : "",
             text.c_str());
    Add({g_name, std::move(text), team >= 0 ? kMate : kNeutral, to_team, Now()});
}

ImU32 NameColour(Colour c, float alpha) {
    // as player_tags.cpp's TeamColour: the game's green / red, or blue / orange; the game's neutral light blue
    static constexpr uint8_t kColours[2][3][3] = {{{0x96, 0xC8, 0xFF}, {0x96, 0xFF, 0x96}, {0xFF, 0x96, 0x96}},
                                                  {{0x96, 0xC8, 0xFF}, {0x78, 0xBC, 0xFF}, {0xFF, 0xB8, 0x40}}};
    const uint8_t* rgb = kColours[std::min<uint32_t>(display::g_team_colors, display::kTeamBlueOrange)][c];
    return IM_COL32(rgb[0], rgb[1], rgb[2], int(alpha * 255.0f));
}

}  // namespace

namespace chat {

void Tick() {
    const bool in = syslink::InSession();
    if (in != g_in_session.exchange(in)) {
        std::lock_guard lock(g_lock);
        if (!in) Close();
        g_log.clear();  // a new session's chat starts empty
    }
    if (!in) return;

    std::string name;
    const int8_t index = kernel::G<int8_t>(kLocalPlayers);
    const int machines = std::clamp<int>(kernel::G<int8_t>(kMachineCount), 0, 16);
    for (int m = 0; index >= 0 && m < machines && name.empty(); m++) {
        const uint32_t machine = kMachines + uint32_t(m) * kMachineSize;
        const uint8_t used = kernel::G<uint8_t>(machine + kSlotMask);
        for (uint32_t slot = 0; slot < 2; slot++)
            if ((used & (1u << slot)) && kernel::R32(machine + kSlotPlayer + slot * 4) == uint32_t(int32_t(index)) &&
                kernel::G<uint8_t>(machine + kSlotNamed + slot)) {
                const std::string raw = kernel::ReadCString(machine + kSlotName + slot * kSlotNameSize, kSlotNameSize - 1);
                name = Clean(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), kMaxName);
                while (!name.empty() && name.back() == ' ') name.pop_back();
                break;
            }
    }
    if (name.empty()) name = "Player";

    // the team, and whether another player (or bot) of the match shares it
    int32_t team = -1;
    bool mates = false;
    if (index >= 0 && kernel::G<uint8_t>(kMatchFlag))
        if (const uint32_t records = kernel::R32(kRecords)) {
            auto team_of = [](uint32_t r) {
                const uint32_t t = kernel::R32(r + kTeam);
                return t ? int32_t(kernel::R32(t)) : -1;
            };
            for (uint32_t i = 0; i < kRecordCount; i++) {
                const uint32_t r = records + i * kRecordSize;
                if (kernel::R32(r + kKind) == kKindPlayer && kernel::R16(r + kIndex) == uint16_t(index)) {
                    team = team_of(r);
                    break;
                }
            }
            for (uint32_t i = 0; team >= 0 && i < kRecordCount && !mates; i++) {
                const uint32_t r = records + i * kRecordSize;
                mates = kernel::R32(r + kKind) == kKindPlayer && kernel::R16(r + kIndex) != uint16_t(index) &&
                        team_of(r) == team;
            }
        }
    const bool mates_was = g_has_mates.exchange(mates);
    if ((team != g_team.exchange(team) || mates != mates_was) && Trace())
        LOGR("chat: this copy's team %d%s", team, mates ? ", with teammates" : "");

    std::lock_guard lock(g_lock);
    if (name != g_name) {
        if (Trace()) LOGR("chat: this copy's name '%s' (player %d)", name.c_str(), index);
        g_name = std::move(name);
    }
}

bool Typing() { return g_typing_flag.load(); }

bool Message(uint32_t msg, uintptr_t wp, intptr_t lp) {
    const bool repeat = (lp & (1 << 30)) != 0;
    std::lock_guard lock(g_lock);
    if (!g_typing) {
        if (msg != app::kMsgKeyDown || repeat || !g_in_session.load() || ui::Open() || menus::Typing() ||
            app::Background())
            return false;
        bool chat_key = false;
        for (int port = 0; port < input::kPorts; port++)
            for (int vk : keyboard::g_keys[port][keyboard::kChat]) chat_key |= vk && vk == int(wp);
        if (!chat_key) return false;
        g_typing = true;
        g_typing_flag = true;
        g_to_team = !(app::KeyDown(kShift) || app::KeyDown(kLShift) || app::KeyDown(kRShift));
        g_open_vk = int(wp);
        g_typed.clear();
        g_opened_at = Now();
        app::TextInput(true);
        return true;
    }
    if (msg == app::kMsgChar) {
        uint32_t c = uint32_t(wp);
        // Windows: the key that opened the line (a letter's virtual key is its capital) comes as a character too,
        // straight after
        if (c < 0x80 && std::toupper(int(c)) == g_open_vk && g_typed.empty() && Now() - g_opened_at < 0.15) return true;
        if (c >= 0xD800 && c < 0xDC00) {
            g_high_surrogate = uint16_t(c);
            return true;
        }
        if (c >= 0xDC00 && c < 0xE000) {
            if (!g_high_surrogate) return true;
            c = 0x10000 + ((uint32_t(g_high_surrogate) - 0xD800) << 10) + (c - 0xDC00);
        }
        g_high_surrogate = 0;
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || Characters(g_typed) >= kMaxTyped) return true;
        std::string next = g_typed;
        Append(next, c);
        if (next.size() <= kMaxText) g_typed = std::move(next);
        return true;
    }
    if (msg != app::kMsgKeyDown) return false;
    switch (wp) {
        case kEnter:
            if (repeat) return true;
            Send();
            Close();
            return true;
        case kEscape:
            if (repeat) return true;
            Close();
            ui::SwallowEsc();  // not the game's Start until let go
            return true;
        case kBackspace:
            while (!g_typed.empty() && (uint8_t(g_typed.back()) & 0xC0) == 0x80) g_typed.pop_back();
            if (!g_typed.empty()) g_typed.pop_back();
            return true;
        default:
            return false;  // F1 and the like still work; the keys drive no pad while typing (keyboard::Pad)
    }
}

void Draw(const char* team_label, const char* all_label) {
    std::lock_guard lock(g_lock);
    if (g_typing && (ui::Open() || !g_in_session.load())) Close();
    if (!g_in_session.load() || (g_log.empty() && !g_typing)) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImFont* font = ImGui::GetFont();
    const float size = std::max(14.0f, std::round(display.y / 40.0f));
    const float pad = std::round(size * 0.35f);
    const float left = std::round(std::max(display.x * 0.03f, size));
    const float wrap = std::round(std::min(display.x * 0.42f, size * 36.0f));
    float bottom = std::round(display.y * 0.74f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float shadow = std::max(1.0f, std::round(size / 14.0f));
    const double now = Now();

    if (g_typing) {
        // the line being typed, on a dark bar, a caret blinking after it
        const std::string prompt = std::string(TeamLine(g_to_team) ? team_label : all_label) + ": ";
        std::string shown = prompt + g_typed;
        if (std::fmod(now, 1.0) < 0.55) shown += '_';
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, wrap, shown.c_str());
        const float h = std::max(ts.y, size);
        const ImVec2 lo(left - pad, bottom - h - pad), hi(left + wrap + pad, bottom + pad);
        dl->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 160), size * 0.25f);
        dl->AddText(font, size, ImVec2(left, bottom - h), IM_COL32(255, 255, 255, 255), shown.c_str(), nullptr, wrap);
        bottom = lo.y - pad;
    }

    // the lines said, the newest at the bottom
    size_t drawn = 0;
    for (auto it = g_log.rbegin(); it != g_log.rend() && drawn < kLines; ++it) {
        const double age = now - it->at;
        float alpha = 1.0f;
        if (!g_typing) {
            if (age >= kShowSeconds) break;
            if (age > kShowSeconds - kFadeSeconds) alpha = float((kShowSeconds - age) / kFadeSeconds);
        }
        // the name in its colour, the text after it on the first row (as far as it fits), the rest wrapped below
        const std::string head = (it->team ? "(" + std::string(team_label) + ") " : std::string()) + it->name + ": ";
        const float head_w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, head.c_str()).x;
        const char* text = it->text.c_str();
        const char* end = text + it->text.size();
        const char* cut = head_w < wrap ? font->CalcWordWrapPosition(size, text, end, wrap - head_w) : text;
        const char* rest = cut;
        while (rest < end && *rest == ' ') rest++;
        const float rest_h = rest < end ? font->CalcTextSizeA(size, FLT_MAX, wrap, rest, end).y : 0.0f;
        const ImVec2 at(left, bottom - size - rest_h);
        const ImU32 dark = IM_COL32(0, 0, 0, int(alpha * 200.0f)), white = IM_COL32(255, 255, 255, int(alpha * 255.0f));
        auto put = [&](ImVec2 p, ImU32 col, const char* b, const char* e, float w) {
            dl->AddText(font, size, ImVec2(p.x + shadow, p.y + shadow), dark, b, e, w);
            dl->AddText(font, size, p, col, b, e, w);
        };
        put(at, NameColour(it->colour, alpha), head.c_str(), nullptr, 0.0f);
        if (cut > text) put(ImVec2(at.x + head_w, at.y), white, text, cut, 0.0f);
        if (rest < end) put(ImVec2(at.x, at.y + size), white, rest, end, wrap);
        bottom = at.y - std::round(size * 0.15f);
        drawn++;
    }
}

}  // namespace chat

#else  // no ImGui here (the headless harness)

namespace chat {
void Tick() {}
bool Message(uint32_t, uintptr_t, intptr_t) { return false; }
void Draw(const char*, const char*) {}
bool Typing() { return false; }
}  // namespace chat

#endif
