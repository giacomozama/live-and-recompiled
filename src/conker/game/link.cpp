// System link: only copies of this build at the same tick rate play together.
//
// The game's own guard is one protocol version, 0x1D4, which a search (SysFind, written 0x288720, read 0x288760 into
// [msg+0x40]) and a join request (JoinG, written 0x27BC10, read into the word [msg+0x1F8]) carry in 16 bits. A host
// ignores a search of another version, and answers a JoinG of another (0x27C204) with JoinD reason 2, which the
// joining copy shows as VERSION_OUT_OK_SYNC (0x27CB90). Every copy of the console game says 0x1D4, so the console's,
// an emulator's, another port's and every build of this one would all pass.
//
// The advertisement: a host answers a search with SysFindR (built at 0x2887B0, written 0x288A10): the search's nonce,
// the session's XNKID, XNKEY and XNADDR, the level, a field of 29 bits the game leaves 0 ([msg+0x38], "unused29Bits"
// in its debug print), the player counts and the host's name. The joining copy keeps each answer in the session list
// at 0x863D40 (12 entries of 0x68 bytes, 0x288C20: the XNKID at +0x2C) and joins the one at index [0x862C58] (0x258BF7,
// system link).
//
// Here the host puts a token in those 29 bits:
//   bits 0-7   its tick rate in Hz (display::GameRate: 30 or 60 now)
//   bit 8      the session's rules: friendly fire off (syslink::g_no_friendly_fire, friendly_fire.cpp)
//   bits 9-28  20 bits of a hash of "Live & Recompiled" and the build's version (XBE_VERSION, `git describe`), never 0
// The layout belongs to the build: a build that changes it has another version, so its hash already differs from
// every build that reads the bits otherwise.
// The joining copy notes each session's token by its XNKID, and on joining compares the tick rate and the hash with
// its own (kMatch); if they differ (or the token is 0: an older build, the console game, an emulator, another port)
// its JoinG carries version 0, which every host refuses with JoinD reason 2. The game's notice of that says "Wrong
// version" (VERSION_OUT_OK_SYNC, looked up at 0x27CBF9); when the tick rate was why, it says "Wrong frame rate"
// instead (menus::GameText, in the game's language; Japanese keeps the game's text).
// The rules are the host's to set and not compared: in a match each attacker's machine decides its own hits, so the
// joining copy is to take the host's (SessionNoFriendlyFire) rather than be refused. The JoinG version itself is a
// 16 bit fold of the token's matched bits (Version, never 0 or 0x1D4), and the host takes only its own: a copy that
// never looked at the token (an older build) is refused too. The search keeps 0x1D4, so a session of another version
// still shows in the list.
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "common/display.h"
#include "common/link.h"
#include "common/menus.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#if __has_include("version.h")
#include "version.h"
#else
#define XBE_VERSION "dev"  // the headless harness writes no version header
#endif

namespace {

constexpr uint32_t kSessions = 0x863D40;       // the session list, kSessionSize bytes an entry
constexpr uint32_t kSessionSize = 0x68;
constexpr uint32_t kSessionCount = 12;
constexpr uint32_t kSessionKeyId = 0x2C;       // an entry's XNKID
constexpr uint32_t kSessionName = 0x0C;        // an entry's host name
constexpr uint32_t kJoinIndex = 0x862C58;      // the entry being joined, a signed byte
constexpr uint32_t kFindReplyToken = 0x38;     // SysFindR's 29 bits
constexpr uint32_t kFindReplyKeyId = 0x68;     // SysFindR's XNKID
constexpr uint32_t kJoinVersion = 0x1F8;       // JoinG's version, a word

// the token's fields
constexpr uint32_t kTickMask = 0xFF;            // tick rate, Hz
constexpr uint32_t kNoFriendlyFire = 1u << 8;  // rule: friendly fire off
constexpr uint32_t kIsHost = 0x862C5C;           // byte: this copy hosts
constexpr uint32_t kNetState = 0x863D08;         // 0 none, 1 joining, 2 waiting room, 3 playing, 4 leaving, 5 creating
constexpr uint32_t kHashShift = 9, kHashBits = 20;
constexpr uint32_t kHashMask = ((1u << kHashBits) - 1) << kHashShift;
constexpr uint32_t kMatch = kHashMask | kTickMask;  // what a joining copy must share with the host
static_assert(kHashShift + kHashBits == 29, "the token fills SysFindR's 29 bits");

// 20 bits of FNV-1a over the build's name and version, never 0
uint32_t BuildHash() {
    uint32_t h = 2166136261u;
    for (const char* s : {"Live & Recompiled ", XBE_VERSION})
        for (; *s; s++) h = (h ^ uint8_t(*s)) * 16777619u;
    h = (h ^ (h >> kHashBits)) & ((1u << kHashBits) - 1);
    return h ? h : 1;
}

// this copy's token, as advertised: the build, the tick rate it runs at now and the rules it hosts with
uint32_t Token() {
    return BuildHash() << kHashShift | (syslink::g_no_friendly_fire ? kNoFriendlyFire : 0) | (display::GameRate() & kTickMask);
}

// the JoinG version from a token's matched bits: 16 bits, never 0 (a refused join's) nor the console's 0x1D4
uint16_t Version(uint32_t token) {
    token &= kMatch;
    uint16_t v = uint16_t(token ^ (token >> 16));
    return v == 0 || v == 0x1D4 ? uint16_t(v ^ 0x8000) : v;
}

// each session's token by its XNKID, as the last answer from it said (game thread)
std::unordered_map<uint64_t, uint32_t> g_tokens;
// why the join under way was refused: its JoinG says so, and the game's notice of it says which (game thread)
enum class Refused { kNo, kVersion, kTickRate } g_refused = Refused::kNo;
// the JoinD being shown is the game's VERSION_OUT_OK_SYNC (game thread)
bool g_version_notice = false;
constexpr uint32_t kVersionNoticeKey = 0x603F38;  // L"VERSION_OUT_OK_SYNC", the text key of JoinD reason 2
// the rules of the session last joined, as its host advertised them (game thread)
uint32_t g_session_rules = 0;

}  // namespace

bool syslink::g_no_friendly_fire = false;

// [[hook]] after 0x2888A8: the host's SysFindR (edi), its 29 bits set to 0, carries the token
void ConkerLinkAdvertise(X86Context& ctx, uint8_t* base) {
    (void)base;
    kernel::W32(ctx.edi + kFindReplyToken, Token());
}

// [[hook]] at 0x288D45: a SysFindR (ebx) kept in the session list at edi: its token noted by the session's XNKID
void ConkerLinkSession(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (g_tokens.size() > 256) g_tokens.clear();  // sessions come and go; the list holds 12
    g_tokens[kernel::R64(ctx.ebx + kFindReplyKeyId)] = kernel::R32(ctx.ebx + kFindReplyToken);
}

// [[hook]] at 0x258BF7: joining the system link session at [0x862C58]: refused unless its token is this copy's
void ConkerLinkJoin(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    const int8_t index = kernel::G<int8_t>(kJoinIndex);
    g_refused = Refused::kNo;
    if (index < 0 || uint32_t(index) >= kSessionCount) return;
    const uint32_t entry = kSessions + uint32_t(index) * kSessionSize;
    const auto it = g_tokens.find(kernel::R64(entry + kSessionKeyId));
    const uint32_t theirs = it == g_tokens.end() ? 0 : it->second, ours = Token();
    g_session_rules = theirs & kNoFriendlyFire;
    if ((theirs & kMatch) == (ours & kMatch)) return;
    const bool version = (theirs & kHashMask) != (ours & kHashMask);
    g_refused = version ? Refused::kVersion : Refused::kTickRate;
    const std::string name = kernel::ReadCString(entry + kSessionName, 32);
    if (version)
        LOGR("link: not joining '%s': it does not run Live & Recompiled " XBE_VERSION
             " (another version, the console game, an emulator or another port)", name.c_str());
    else
        LOGR("link: not joining '%s': its game runs at %u FPS, this copy's at %u (Frame rate)", name.c_str(),
             theirs & kTickMask, ours & kTickMask);
}

// [[hook]] at 0x27BC31: JoinG's version about to be written (eax, 16 bits): this copy's, or 0 for a refused join,
// which any host turns down
void ConkerLinkJoinVersion(X86Context& ctx, uint8_t* base) {
    (void)base;
    ctx.eax = g_refused != Refused::kNo ? 0 : Version(Token());
}

// [[hook]] at 0x27C219: the host's check of a JoinG (ebx) against 0x1D4 ([esp+0xA]): against this copy's version
void ConkerLinkJoinCheck(X86Context& ctx, uint8_t* base) {
    (void)base;
    const uint16_t theirs = kernel::R16(ctx.ebx + kJoinVersion), ours = Version(Token());
    kernel::G<uint8_t>(ctx.esp + 0xA) = theirs == ours;
    if (theirs != ours) LOGR("link: a join of version %04X refused (this copy's %04X)", theirs, ours);
}

// the host's own rule, or as the host advertised it to this copy when it joined; out of a session (split screen,
// Dumbots) this copy's own. Each attacker's machine decides its own hits, so every copy in a session plays the host's
bool syslink::SessionNoFriendlyFire() {
    if (!InSession()) return g_no_friendly_fire;
    return kernel::G<uint8_t>(kIsHost) ? g_no_friendly_fire : g_session_rules != 0;
}

bool syslink::InSession() {
    const uint32_t net = kernel::G<uint32_t>(kNetState);
    return net >= 1 && net <= 5;
}

// [[hook]] at 0x27CBF7 in JoinD's handling (0x27CB90): ecx the key of the notice's text about to be looked up
void ConkerLinkDeniedKey(X86Context& ctx, uint8_t* base) {
    (void)base;
    g_version_notice = ctx.ecx == kVersionNoticeKey;
}

// [[hook]] at 0x27CBFE: the notice's text found (eax, UTF-16): the game's "Wrong version" says which, when the join
// was refused here for the tick rate
void ConkerLinkDeniedText(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!g_version_notice || g_refused != Refused::kTickRate || !ctx.eax) return;
    if (const uint32_t text = menus::GameText("Wrong frame rate", "Falsche Bildrate", "Mauvaise fréquence d'images",
                                              "Tasa de fotogramas incorrecta", "Frequenza fotogrammi errata"))
        ctx.eax = text;
}
