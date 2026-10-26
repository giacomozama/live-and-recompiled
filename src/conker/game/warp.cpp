// Debug warps: any checkpoint, level or cutscene, from the F1 menu's Debug tab or the environment.
//
// Levels. [0x7FA1F8] is the running level and [0x7FA1F4] the next, indices into the name table 0x69CF90 (75 entries;
// the package is aid\zpackage\Level\<name>). 0x2A94A0 takes a level change: arg0 the kind, esi the level. Kinds 4 and 6
// store arg1 as the spawn point (byte [0x771B9C]) and set the load state [0x7819D6] to 2, a new game there
// (0x2A95B0 resets the game, 0x2A96E0 loads); kinds 1 and 2 (state 3) keep the game and only load. It drops the
// request while [0x76EC82] or [0x771130] is set, and the level loop leaves at the end of the frame for the load.
//
// Checkpoints. 0x59E9A0 holds the game's 238 checkpoints ("contexts"), 20 bytes each: +0 name (UTF-16, may be null),
// +4 flags, +8 level (word), +0xA spawn (low byte), +0xC the checkpoints it follows (word list, 0xFFFF ends it). The
// frontend's chapter start 0x1FB760 clears the progress bits ([0x84B290], 0x35DF40), sets those of every checkpoint
// before the chosen one (0x35DE10), and asks for a new game at its level and spawn. It takes the checkpoint from the
// chapter menu, unless the byte [0x849813] is set: then from the word [0x849810]. Nothing in the image writes those
// two, a start override the developers left in, so a warp to a checkpoint is that override and a call.
//
// Cutscenes. 0x4EC10 (cdecl: level, cutscene, slot, two more the callers pass as 0) plays cutscene_<prefix>_<n>, the
// prefix from 0x68C358 by level. In another level it asks for a kind 1 change with the cutscene in [0x76F0B6], and
// the load plays it (0x2A9E18). A cutscene the level's packages lack sends the game to its missing-data screen
// (0x1F04F0), which never returns, so the hook below first asks 0x55790 (ecx level, eax cutscene, esi a name buffer:
// 5 or 4 the resource type that has it, 0 none). It looks only at loaded resources, so it answers for the running
// level alone: the hook asks once the cutscene's level is the running one. From the frontend that kind 1 load stops
// on the missing-data screen as a level load does (docs/conker.md), so there the level is started from its first
// checkpoint instead and the cutscene waits until it runs.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#include "xdk/hle.h"
#include "cpu/x86_ops.h"
#include "core/log.h"

#include "common/warp.h"

namespace warp {

namespace {

constexpr uint32_t kLevelNames = 0x69CF90;
constexpr uint32_t kCutscenePrefixes = 0x68C358;
constexpr uint32_t kCheckpointTable = 0x59E9A0;
constexpr int kCheckpointCount = 238;  // the progress walk 0x35DDD0 covers 0xEE
constexpr int kAnimTest = 8;           // named, but not on the disc
constexpr int kNightLevels = 73;       // 73, 74: Windmill and BankEntrance at night, which the game picks itself
// the frontend's levels: Frontend, FrontendTable, Hell, Mp_Frontend, BootCutscene
constexpr int kFrontendLevels[] = {29, 33, 34, 70, 71};

constexpr uint32_t kLevel = 0x7FA1F8;
constexpr uint32_t kLoadState = 0x7819D6;
constexpr uint32_t kBusy1 = 0x76EC82, kBusy2 = 0x771130;
constexpr uint32_t kPlayer = 0x84A5F8;  // 0x4EC10 reads it first thing
constexpr uint32_t kStartOverride = 0x849813, kStartCheckpoint = 0x849810;

constexpr uint32_t kStartAtCheckpoint = 0x1FB760;
constexpr uint32_t kRequestLevel = 0x2A94A0;
constexpr uint32_t kPlayCutscene = 0x4EC10;
constexpr uint32_t kFindCutscene = 0x55790;

constexpr int kMaxCutscene = 100;  // the highest number looked for when a level loads

enum class Kind { Checkpoint, Level, Cutscene };
struct Request {
    Kind kind;
    int a, b;
    bool loaded = false;  // a cutscene behind a checkpoint start: waits for its level to be the running one
};

std::mutex g_lock;
std::deque<Request> g_requests;
std::string g_status;
std::vector<int> g_cutscenes;
int g_cutscenes_level = -1;
int g_level = -1;

// the cutscene asked for, checked by the hook on 0x4EC10 when it starts in its own level; main thread
int g_want_level = -1, g_want_index = -1;

void SetStatus(std::string s) {
    LOGR("warp: %s", s.c_str());
    std::lock_guard<std::mutex> l(g_lock);
    g_status = std::move(s);
}

std::string LevelLabel(int level) {
    const char* n = LevelName(level);
    return std::to_string(level) + " " + (n ? n : "?");
}

bool InFrontend(int level) { return std::find(std::begin(kFrontendLevels), std::end(kFrontendLevels), level) != std::end(kFrontendLevels); }

// the first checkpoint in the level, -1 none
int FirstCheckpoint(int level) {
    const std::vector<Checkpoint>& list = Checkpoints();
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].level == level) return int(i);
    return -1;
}

bool Idle() {
    return !kernel::G<uint8_t>(kLoadState) && !kernel::G<uint8_t>(kBusy1) && !kernel::G<uint8_t>(kBusy2);
}

// 0x55790: does the running level have this cutscene
bool HasCutscene(X86Context& ctx, int level, int index) {
    if (level < 0 || level >= kLevels || !kernel::R32(kCutscenePrefixes + 4 * uint32_t(level))) return false;
    const uint32_t esp = ctx.esp, esi = ctx.esi, ecx = ctx.ecx, eax = ctx.eax;
    ctx.esp = (esp - 0x200) & ~15u;  // the name is built in room under the frame
    ctx.esi = ctx.esp;
    ctx.ecx = uint32_t(level);
    ctx.eax = uint32_t(index);
    uint32_t type = kernel::CallGuest(ctx, kFindCutscene, {});
    ctx.esp = esp, ctx.esi = esi, ctx.ecx = ecx, ctx.eax = eax;
    return type != 0;
}

int ParseLevel(const std::string& s) {
    if (!s.empty() && std::all_of(s.begin(), s.end(), ::isdigit)) return std::atoi(s.c_str());
    for (int i = 0; i < kLevels; i++) {
        const char* n = LevelName(i);
        if (n && strcasecmp(n, s.c_str()) == 0) return i;
    }
    return -1;
}

// XBE_WARP=<checkpoint> | <level>[:<spawn>], XBE_CUTSCENE=<level>:<cutscene>; levels by name or number
void ReadEnv() {
    if (const char* e = std::getenv("XBE_WARP")) {
        std::string s = e;
        size_t colon = s.find(':');
        bool number = !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
        int level = number ? -1 : ParseLevel(s.substr(0, colon));
        if (number)
            GoCheckpoint(std::atoi(e));
        else if (level >= 0)
            GoLevel(level, colon == std::string::npos ? 0 : std::atoi(s.c_str() + colon + 1));
        else
            LOGR("warp: XBE_WARP=%s names no checkpoint or level", e);
    }
    if (const char* e = std::getenv("XBE_CUTSCENE")) {
        std::string s = e;
        size_t colon = s.find(':');
        int level = ParseLevel(s.substr(0, colon));
        if (level >= 0 && colon != std::string::npos)
            PlayCutscene(level, std::atoi(s.c_str() + colon + 1));
        else
            LOGR("warp: XBE_CUTSCENE=%s is not <level>:<cutscene>", e);
    }
}

void Run(X86Context& ctx, const Request& r) {
    switch (r.kind) {
        case Kind::Checkpoint: {
            const Checkpoint& c = Checkpoints()[size_t(r.a)];
            kernel::G<uint8_t>(kStartOverride) = 1;
            kernel::W16(kStartCheckpoint, uint16_t(r.a));
            kernel::CallGuest(ctx, kStartAtCheckpoint, {});
            kernel::G<uint8_t>(kStartOverride) = 0;  // the chapter menu reads its own choice again
            SetStatus("checkpoint " + std::to_string(r.a) + (c.name.empty() ? "" : " \"" + c.name + "\"") + ": " +
                      LevelLabel(c.level) + ", spawn " + std::to_string(c.spawn));
            break;
        }
        case Kind::Level: {
            const uint32_t esi = ctx.esi;
            ctx.esi = uint32_t(r.a);
            kernel::CallGuest(ctx, kRequestLevel, {4, uint32_t(r.b), 0, 1});  // 0 and 1 as the load loop seeds them
            ctx.esi = esi;
            SetStatus("level " + LevelLabel(r.a) + ", spawn " + std::to_string(r.b));
            break;
        }
        case Kind::Cutscene:
            if (!kernel::R32(kPlayer)) {
                SetStatus("cutscene: no player yet, start a game first");
                break;
            }
            if (r.a != g_level && InFrontend(g_level)) {
                const int c = FirstCheckpoint(r.a);
                if (c < 0) {
                    SetStatus("cutscene: " + LevelLabel(r.a) + " has no checkpoint to start it from the frontend, start "
                              "a game first");
                    break;
                }
                Run(ctx, {Kind::Checkpoint, c, 0});
                {
                    // ahead of anything asked since, so the level started for it is the one it plays in
                    std::lock_guard<std::mutex> l(g_lock);
                    g_requests.push_front({Kind::Cutscene, r.a, r.b, true});
                }
                SetStatus("cutscene " + std::to_string(r.b) + " of " + LevelLabel(r.a) + ": starting the level from "
                          "checkpoint " + std::to_string(c));
                break;
            }
            g_want_level = r.a;
            g_want_index = r.b;
            SetStatus("cutscene " + std::to_string(r.b) + " of " + LevelLabel(r.a) +
                      (r.a == g_level ? "" : " (loading the level)"));
            kernel::CallGuest(ctx, kPlayCutscene, {uint32_t(r.a), uint32_t(r.b), 0, 0, 0});
            break;
    }
}

void Post(Request r) {
    std::lock_guard<std::mutex> l(g_lock);
    g_requests.push_back(r);
}

}  // namespace

const std::vector<Checkpoint>& Checkpoints() {
    static const std::vector<Checkpoint> list = [] {
        std::vector<Checkpoint> v;
        for (int i = 0; i < kCheckpointCount; i++) {
            uint32_t e = kCheckpointTable + 20 * uint32_t(i);
            Checkpoint c{kernel::R16(e + 8), kernel::G<uint8_t>(e + 0xA), {}};
            if (uint32_t p = kernel::R32(e))
                for (uint16_t ch; (ch = kernel::R16(p)) != 0; p += 2) c.name += ch < 0x80 ? char(ch) : '?';
            v.push_back(std::move(c));
        }
        return v;
    }();
    return list;
}

const char* LevelName(int level) {
    if (level < 0 || level >= kNightLevels || level == kAnimTest) return nullptr;
    uint32_t p = kernel::R32(kLevelNames + 4 * uint32_t(level));
    return p ? reinterpret_cast<const char*>(kernel::HostPtr(p)) : nullptr;
}

int CurrentLevel() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_level;
}

bool GoCheckpoint(int index) {
    if (index < 0 || index >= kCheckpointCount) return SetStatus("no checkpoint " + std::to_string(index)), false;
    Post({Kind::Checkpoint, index, 0});
    return true;
}

bool GoLevel(int level, int spawn) {
    if (!LevelName(level)) return SetStatus("no level " + std::to_string(level)), false;
    Post({Kind::Level, level, std::clamp(spawn, 0, 255)});
    return true;
}

bool PlayCutscene(int level, int index) {
    if (!LevelName(level)) return SetStatus("no level " + std::to_string(level)), false;
    Post({Kind::Cutscene, level, std::clamp(index, 0, 255)});
    return true;
}

std::vector<int> Cutscenes(int* level) {
    std::lock_guard<std::mutex> l(g_lock);
    if (level) *level = g_cutscenes_level;
    return g_cutscenes;
}

std::string Status() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_status;
}

// main thread, from the frame timer (display.cpp): the level loop is running, so a level is loaded
void Poll(X86Context& ctx) {
    static bool env = (ReadEnv(), true);
    (void)env;
    const int level = int(kernel::R32(kLevel));
    if (level != g_cutscenes_level) {
        // the running level's cutscenes, while its packages are loaded
        std::vector<int> found;
        for (int i = 0; i < kMaxCutscene; i++)
            if (HasCutscene(ctx, level, i)) found.push_back(i);
        LOGR("warp: in level %s, %zu cutscenes", LevelLabel(level).c_str(), found.size());
        std::lock_guard<std::mutex> l(g_lock);
        g_level = level;
        g_cutscenes_level = level;
        g_cutscenes = std::move(found);
    }
    if (!Idle()) return;
    Request r;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_requests.empty()) return;
        // a checkpoint start load runs the frontend's level loop a while first: its cutscene waits for its level, and
        // is dropped when another level came up instead
        const Request& f = g_requests.front();
        if (f.loaded && f.a != level) {
            if (InFrontend(level)) return;
            g_status = "cutscene " + std::to_string(f.b) + ": " + LevelLabel(level) + " came up, not " + LevelLabel(f.a);
            g_requests.pop_front();
            return;
        }
        r = g_requests.front();
        g_requests.pop_front();
    }
    Run(ctx, r);
}

}  // namespace warp

// 0x4EC10: starts a cutscene (see the top). The one the Debug tab asked for is looked up first, once it starts in its
// own level; one the level lacks is not started.
X86_EXTERN_FUNC(__imp__sub_0004EC10);
X86_FUNC(sub_0004EC10) {
    const int level = int(kernel::R32(ctx.esp + 4)), index = int(kernel::R32(ctx.esp + 8));
    if (level == warp::g_want_level && index == warp::g_want_index && level == int(kernel::R32(warp::kLevel))) {
        warp::g_want_level = warp::g_want_index = -1;
        if (!warp::HasCutscene(ctx, level, index)) {
            warp::SetStatus("level " + warp::LevelLabel(level) + " has no cutscene " + std::to_string(index));
            kernel::Return(ctx, 0, 0);  // cdecl: the caller pops the arguments
            return;
        }
    }
    __imp__sub_0004EC10(ctx, base);
}
