// Conker hooks. The engine was built to squeeze the original Xbox, so it calls the D3D8 library's internals
// directly (LTCG inlining) and submits raw NV2A pushbuffers. The runtime already decodes those pushbuffers
// (d3d::Walk) and implements the D3D8 API at a high level, so the only internals that still need replacing
// are the ones that drive the hardware directly (the per-frame buffer clear, which reads the miniport).
//
// A strong X86_FUNC replaces the recompiled function at that address.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <bit>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xdk/hle.h"
#include "kernel/memory.h"
#include "cpu/x86_ops.h"
#include "core/diag.h"
#include "core/input.h"
#include "core/loadtrace.h"
#include "xdk/d3d8.h"
#include "xdk/dsound.h"

#include "common/characters.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/grass.h"
#include "common/keyboard.h"
#include "common/loading.h"
#include "common/ui.h"

namespace {

// device globals Conker's D3D8LTCG uses (see src/conker/game.toml [data])
constexpr uint32_t kD3D_pDevice = 0x5499E8;
constexpr uint32_t kDevBackBuffer = 0x1A14;  // CDevice_BackBuffer
constexpr uint32_t kDevAutoDepth = 0x1A20;   // CDevice_AutoDepth

}  // namespace

// 0x4E2801: the XMV movie decoder's per-frame update. The transition manager treats a negative return as a
// failed movie: its open loop (0x252470) leaves and its playing state (0x2529xx) closes and releases the
// decoder. With XBE_SKIP_FMV set it always reports that, so the intro FMVs never decode in real time.
X86_EXTERN_FUNC(__imp__sub_004E2801);
X86_FUNC(sub_004E2801) {
    if (diag::SkipFmv()) {
        kernel::Return(ctx, 0xFFFFFFFFu, 4);  // stdcall: the callee pops the 4 arguments
        return;
    }
    __imp__sub_004E2801(ctx, base);
}

// 0x601D0: one step of the hard disk cache copier. The first launch copies wavebanks, packages and textures to Z:
// (0x35950 and its kind queue them at priority 3), and the intro won't take a skip (0x1F6C50) until that queue is
// empty. The file pump (0x2A37B0, once a frame) polls the async I/O (0x1F1360), and with none in flight (0x775F50)
// picks the next job (0x5FFE0) and runs this step, which moves 128 KB between double buffers: one step a frame,
// about 6 MB/s, so the copy took most of the intro FMVs. Keep stepping within a slice of the frame, completing this
// thread's reads at once (they land a moment later otherwise), until a step finds nothing to do or I/O is still
// out. XBE_CONSOLE_CACHE_COPY keeps the one step a frame.
X86_EXTERN_FUNC(__imp__sub_000601D0);
X86_FUNC(sub_000601D0) {
    static const bool console = std::getenv("XBE_CONSOLE_CACHE_COPY") != nullptr;
    static thread_local bool inner = false;
    if (console || inner) {
        __imp__sub_000601D0(ctx, base);
        return;
    }
    using kernel::R32;
    using clock = std::chrono::steady_clock;
    constexpr uint32_t kPollFiles = 0x1F1360, kFilesInFlight = 0x775F50, kQueue = 0x5F5F0, kNextJob = 0x5FFE0;
    const auto end = clock::now() + std::chrono::milliseconds(2);
    const uint32_t step = ctx.eax;  // the copier: its 0x40000 double buffer at +0, freed when idle
    inner = true;
    kernel::CallGuest(ctx, 0x601D0, {});
    const uint32_t queue = kernel::CallGuest(ctx, kQueue, {});
    while (clock::now() < end) {
        kernel::FlushPendingIo();
        kernel::CallGuest(ctx, kPollFiles, {});
        if (R32(kFilesInFlight) || base[queue + 0xC]) break;  // a read still out, or the queue paused
        const bool idle = !R32(step);
        const uint32_t esi = ctx.esi, edi = ctx.edi;
        ctx.edi = R32(queue);
        ctx.esi = R32(queue + 8);
        kernel::CallGuest(ctx, kNextJob, {});
        ctx.eax = step;
        kernel::CallGuest(ctx, 0x601D0, {});
        ctx.esi = esi, ctx.edi = edi;
        if (idle && !R32(step)) break;  // nothing under way and nothing picked up
    }
    inner = false;
    kernel::Return(ctx, ctx.eax, 0);
}

// 0x1F6C50: the intro FMVs' update (eax the intro, [intro] its state, 1 while a movie plays). It runs the file pump
// and takes a skip (A, B or Start: 0x4110 in the pressed bits at +8 of the pad record 0x4E020 returns) only once the
// cache copy queue is empty, so a press while the first launch's copy runs was lost and the movies looked
// unskippable. Remember that press and hand it back each frame until the intro takes it, then put the pad as it was
// so nothing else reads it a second time. A left click (any player's mouse, the settings menu shut) skips as A does
// (0x100): the mouse's buttons are bound to actions, not to A.
X86_EXTERN_FUNC(__imp__sub_001F6C50);
X86_FUNC(sub_001F6C50) {
    using kernel::R32;
    constexpr uint32_t kSkipButtons = 0x4110, kA = 0x100;
    constexpr int kMouse1 = 0x01;
    static uint32_t intro = 0, held = 0;
    static bool clicked = false;
    const uint32_t self = ctx.eax;
    if (self != intro) intro = self, held = 0;
    const uint32_t pad = kernel::CallGuest(ctx, 0x4E020, {1});
    const uint32_t pressed = R32(pad + 8);
    held |= pressed & kSkipButtons;
    bool click = false;
    for (int p = 0; p < input::kPorts && !ui::Open(); p++) click |= input::KeyDown(p, kMouse1);
    if (click && !clicked) held |= kA;  // pressed now, not held from before
    clicked = click;
    kernel::W32(pad + 8, pressed | held);
    __imp__sub_001F6C50(ctx, base);
    kernel::W32(pad + 8, pressed);
    if (R32(self) != 1) held = 0;  // skipped, or the movies ended
}

// [[hook]] before 0x56AD4 in the cutscene speech command (0x569A0, ebx the command: +2 its text, +6 its flags). A
// line flagged 0x100 shows no speech bubble in English, which the next instruction tests eax (XGetLanguage) for:
// Conker's monologues on the throne, in the intro and the ending, whose text the English game has all the same.
// cutscenes::g_english_bubbles: not English, so they get their bubbles as in the other languages.
bool cutscenes::g_english_bubbles = true;

void ConkerEnglishBubble(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (cutscenes::g_english_bubbles) ctx.eax = 0;
}

// 0x4FF00: may LT end the playing in-engine cutscene (edi its slot)? The title allows it only for cutscenes the
// player has seen (0x4EBE0, a bit per cutscene at 0x765608), never for the intro's first one (mode 0x18, cutscene
// 1), in mode 0x1D (the intro's later scenes) only cutscene 5, and not in the last half second.
// cutscenes::g_skippable (or XBE_SKIPPABLE_CUTSCENES) replaces LT: any button held for a second (all but Start
// and Back on a controller, all but Escape, F1 and the pause and quit keys on the keyboard) skips any cutscene, seen or
// not, still not in the last half second. The widget the ImGui overlay draws (cutscenes::Hold) fills as it is held.
// Mode 0x21 skips on a button of its own and keeps its test.
bool cutscenes::g_skippable = true;

namespace {

constexpr double kSkipHold = 1.0;
std::atomic<float> g_hold_progress{0.0f};
std::atomic<int64_t> g_hold_seen{INT64_MIN};  // steady-clock microseconds of the hook's last test

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// a button other than the pause and quit ones held on any port: a controller's buttons, triggers, d-pad and stick
// clicks, or any key or mouse button
bool SkipButtonHeld() {
    constexpr uint16_t kStartBack = 0x10 | 0x20;
    for (int p = 0; p < input::kPorts; p++) {
        const input::Pad pad = input::Raw(p);
        if (pad.connected) {
            if (pad.buttons & ~kStartBack) return true;
            for (uint8_t a : pad.analog)
                if (a > 64) return true;
        }
        // any key or mouse button (virtual-key codes 1 to 254) but Escape and F1, which open the menu, and the
        // ones bound to pause or quit
        for (int vk = 1; vk < 0xFF; vk++) {
            if (vk == 0x1B || vk == 0x70) continue;
            if (!input::KeyDown(p, vk)) continue;
            bool special = false;
            for (int a : {keyboard::kPause, keyboard::kQuit})
                for (int bound : keyboard::g_keys[p][a]) special |= bound == vk;
            if (!special) return true;
        }
    }
    return false;
}

// held for kSkipHold seconds? A button already down when the cutscene began (or the settings menu shut) does not
// count until it is let go
bool SkipHeld() {
    static bool armed = false, running = false;
    static int64_t since = 0;
    const int64_t now = NowUs();
    if (now - g_hold_seen.load() > 250000) armed = running = false;  // the last test was a different cutscene's
    g_hold_seen.store(now);
    const bool down = !ui::Open() && SkipButtonHeld();
    if (!down) {
        armed = !ui::Open();
        running = false;
        g_hold_progress.store(0.0f);
        return false;
    }
    if (!armed) return false;
    if (!running) running = true, since = now;
    const double held = double(now - since) / 1e6;
    g_hold_progress.store(float(std::min(held / kSkipHold, 1.0)));
    if (held < kSkipHold) return false;
    armed = running = false;  // the next cutscene wants a fresh press
    g_hold_progress.store(0.0f);
    return true;
}

}  // namespace

float cutscenes::Hold() {
    return NowUs() - g_hold_seen.load() <= 250000 ? g_hold_progress.load() : 0.0f;
}

X86_EXTERN_FUNC(__imp__sub_0004FF00);
X86_FUNC(sub_0004FF00) {
    static const bool env = std::getenv("XBE_SKIPPABLE_CUTSCENES") != nullptr;
    using kernel::R32;
    uint32_t mode = R32(0x7FA1F8);
    if ((env || cutscenes::g_skippable) && mode != 0x21) {
        // the "no skipping" flag; the last half second is not skippable
        float fps;
        std::memcpy(&fps, base + 0x7F9F5C, 4);
        int32_t now = int32_t(R32(0x76F0F8 + ctx.edi * 4)), end = int32_t(R32(0x76F0E8 + ctx.edi * 4));
        const bool allowed = !base[0x7655F7] && now + int32_t(fps) / 2 < end;
        kernel::Return(ctx, allowed && SkipHeld() ? 1 : 0, 0);
        return;
    }
    __imp__sub_0004FF00(ctx, base);
}

// 0x501F0 steps a cutscene slot's clock (esi the slot): [0x76F0F8] its frame, [0x76F0F0] the last, the commands
// between handed on (0x56050). A cutscene that starts part way in (its start frame [0x7655F4], Birdy's in the
// training level at 180) hands on what lies before its start on its first step: while [0x76F0D8] (set once its
// commands were first handed on) is clear and the last frame is the start, a clock that has left the start makes the
// last -1, so the first window (-1, now] holds them. Above 30 the first step is a frame of 1/60 s, and seconds * 60
// can land a hair under the next whole frame (3.0 + 0.01666 -> 180.9999 -> 180): the clock had not left the start,
// the window was empty, and Birdy's scene lost its voice stream (command 0xB at frame 175) and with it the lip sync.
// The last is made -1 then too.
X86_EXTERN_FUNC(__imp__sub_000501F0);
X86_FUNC(sub_000501F0) {
    using kernel::R32;
    const uint32_t slot = ctx.esi;
    __imp__sub_000501F0(ctx, base);
    if (slot > 1 || base[0x76F0E0 + slot] != 1 || base[0x76F0D8 + slot]) return;
    const uint32_t start = kernel::R16(0x7655F4);
    if (R32(0x76F0F0 + slot * 4) == start && R32(0x76F0F8 + slot * 4) == start)
        kernel::W32(0x76F0F0 + slot * 4, 0xFFFFFFFFu);
}

// 0x52B50 (the slot on the stack) hands the tracks to what they move; a level object (kind 3, its id the byte at
// [0x76EF2C + slot * 4] + entry * 8 + 2, matched against [obj+0xC6] by 0x6ADD0) gets its place cut to whole units,
// (short)(int) of x, y + [+0x38], z, and its matrix built (0x6AD30, the object in edx, the matrix at +0x20). The
// InsideSafe platform rose and sank in unit steps under its roll. Above 30 the part the cut dropped is kept per object
// and added to the matrix's translation whenever 0x6AD30 builds it at that place while the cutscene plays. (Its roll,
// angles the tracks read at whole frames of a jittery clock, looked smooth already: reading them at an even frame
// was tried and made no difference.)
namespace {
struct CutFraction {
    int16_t pos[3];
    float frac[3];
};
std::unordered_map<uint32_t, CutFraction> g_cut_fractions;
bool CutscenePlaying() { return kernel::g_base[0x76EC82] == 1 || kernel::g_base[0x76F0E0] == 1; }
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00052B50);
X86_FUNC(sub_00052B50) {
    using kernel::R32;
    const uint32_t slot = R32(ctx.esp + 4);
    __imp__sub_00052B50(ctx, base);
    if (display::GameRate() <= 30 || slot > 1) return;
    const uint32_t count = base[0x76EF28 + slot];
    const uint32_t entries = R32(0x76E884 + slot * 4), kinds = R32(0x76EF2C + slot * 4);
    const uint32_t objs = R32(0x8471B8), nobjs = std::min(R32(0x76FD10), 4096u);
    if (!entries || !kinds || !objs || count > 30) return;
    for (uint32_t i = 0; i < count; i++) {
        if (kernel::R16(kinds + i * 8) != 3) continue;
        const uint8_t id = base[kinds + i * 8 + 2];
        if (!id) continue;
        uint32_t o = 0;
        for (uint32_t k = 0; k < nobjs && !o; k++)
            if (base[objs + k * 0x150 + 0xC6] == id) o = objs + k * 0x150;
        if (!o) continue;
        float e[17];
        std::memcpy(e, base + entries + i * 0x44, sizeof e);
        const float exact[3] = {e[0], e[1] + e[14], e[2]};
        CutFraction f;
        bool placed = true;
        for (uint32_t k = 0; k < 3; k++) {
            f.pos[k] = int16_t(kernel::R16(o + 0x14 + 2 * k));
            f.frac[k] = exact[k] - float(f.pos[k]);
            if (!(std::fabs(f.frac[k]) < 1.0f)) placed = false;  // not this entry's place (its track not running)
        }
        if (placed) g_cut_fractions[o] = f;
    }
}

X86_EXTERN_FUNC(__imp__sub_0006AD30);
X86_FUNC(sub_0006AD30) {
    const uint32_t o = ctx.edx, m = kernel::R32(ctx.esp + 4);
    __imp__sub_0006AD30(ctx, base);
    if (g_cut_fractions.empty()) return;
    if (!CutscenePlaying()) {
        g_cut_fractions.clear();
        return;
    }
    const auto it = g_cut_fractions.find(o);
    if (it == g_cut_fractions.end()) return;
    for (uint32_t k = 0; k < 3; k++)
        if (int16_t(kernel::R16(o + 0x14 + 2 * k)) != it->second.pos[k]) return;  // moved since
    for (uint32_t k = 0; k < 3; k++) {
        float t;
        std::memcpy(&t, base + m + 0x30 + 4 * k, 4);
        t += it->second.frac[k];
        std::memcpy(base + m + 0x30 + 4 * k, &t, 4);
    }
}

// 0x1A9540: draws one grass patch (a node renderer, [patch+0x5C] its fade). A patch at full strength binds the wind
// program (handle at 0x6B89A4: blades bend with time and away from the player, by (1 - v)^2 up the blade); a fading
// one binds a plain transform (0x6B89DC) and fades it with the texture factor and a scaled alpha reference. The two
// declarations read the same stride 12 vertices, the texture coordinate as v7 or v2, and share their constants, so
// grass::g_wind (or XBE_GRASS_WIND=0 off) hands the fading patches the wind program too: only the handle the
// fading path reads changes, for this call, and the rest of its fade stays as it is.
bool grass::g_wind = true;

X86_EXTERN_FUNC(__imp__sub_001A9540);
X86_FUNC(sub_001A9540) {
    static const char* env = std::getenv("XBE_GRASS_WIND");
    constexpr uint32_t kWind = 0x6B89A4, kFade = 0x6B89DC;
    uint32_t wind = kernel::R32(kWind), fade = kernel::R32(kFade);
    if (!(env ? env[0] != '0' : grass::g_wind) || !wind) {
        __imp__sub_001A9540(ctx, base);
        return;
    }
    kernel::W32(kFade, wind);
    __imp__sub_001A9540(ctx, base);
    kernel::W32(kFade, fade);
}

// The patch's detail, in 0x1A9540: d its distance from the player ([esp+0x20] at 0x1A95DD), past 2500 it isn't drawn;
// to 800 it draws all [patch+0x60] blades at full strength; past that f = 1 - (d - 800) / 1700 is both its fade
// (alpha f * 255 at [patch+0x5C], with the plain program) and the share of its blades drawn ([patch+0x64], at least
// 1). grass::g_max_lod draws them all; without grass::g_fade a patch is solid (alpha 255) and drawn however far,
// its d held at the 2500 ([0x649234]) past which it would be culled; with both, every patch is drawn as a near one.
bool grass::g_max_lod = true;
bool grass::g_fade = false;

// [[hook]] at 0x1A95DD, before d is weighed
void ConkerGrassNear(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (grass::g_fade) return;
    if (grass::g_max_lod) {
        kernel::W32(ctx.esp + 0x20, 0);  // 0.0f
        return;
    }
    const uint32_t cull = kernel::R32(0x649234);
    if (std::bit_cast<float>(kernel::R32(ctx.esp + 0x20)) > std::bit_cast<float>(cull))
        kernel::W32(ctx.esp + 0x20, cull);
}

// [[hook]] at 0x1A968F, where the near and fading paths join with the count and alpha set
void ConkerGrassFull(X86Context& ctx, uint8_t* base) {
    (void)base;
    grass::Update();
    if (grass::g_max_lod) kernel::W32(ctx.ebx + 0x64, kernel::R32(ctx.ebx + 0x60));
    if (!grass::g_fade) kernel::G<uint8_t>(ctx.ebx + 0x5C) = 0xFF;
}

// 0x81FC0 (edi the object, its player's index, and where it writes the shells' length scale): how many fur shells an
// object draws ([0x750340], run by 0x3F0270). Most take R = 1000 (type 0x6D 3000) and at most 8; past 0.4 R the count
// falls linearly from that to 1, all but the skin gone by R plus a share for the model's own count. Closer, the full
// count, and the length scale grows from 0.5 at 100 to 1 at 0.4 R. A few types and levels return a fixed count first.
// fur::g_distant (or XBE_FUR_DISTANT=0 off) weighs a farther object as at 0.4 R: every shell, at full length.
bool fur::g_distant = true;

// A player's fur shells (0x80322 in 0x7FB00, Rodent and the others drawn as players) leave the alpha test to whatever
// drew before them: Rodent's material sets neither the compare nor the reference, so in the throne room's close-up of
// him a prop's "greater than 229" cut every strand down to its densest texels, spots on bare skin, while in the rain
// the "16 or more" left there drew the whole coat; the original does the same, hidden at 480p. An object's shells
// (0x78460) get a reference of 10; the furry materials that set their own (Conker's) use "10 or more". The player's
// get that too, through the game's own setter (0x537990: ecx the method, edx the value), before the shells' push
// buffer runs, so a material that sets its own still does.
X86_EXTERN_FUNC(sub_00537990);
// [[hook]] at 0x80358, after the shells' stencil enable
void ConkerFurAlphaTest(X86Context& ctx, uint8_t* base) {
    const uint32_t ecx = ctx.ecx, edx = ctx.edx, eax = ctx.eax;
    ctx.ecx = 0x4033C, ctx.edx = 0x206;  // NV097_SET_ALPHA_FUNC: greater or equal
    X86_PUSH32(0x80358u);
    sub_00537990(ctx, base);
    ctx.ecx = 0x40340, ctx.edx = 10;  // NV097_SET_ALPHA_REF
    X86_PUSH32(0x80358u);
    sub_00537990(ctx, base);
    ctx.ecx = ecx, ctx.edx = edx, ctx.eax = eax;
    fur::TagShells(ctx.ebx);  // ebx the player, as since 0x802F5
}

// An object's fur shells (0x78460, ebx the object, as 0x81FC0 took it in edi): who is drawn, before the shells' state
// and push buffer. The player's are tagged in ConkerFurAlphaTest
// [[hook]] at 0x78460, after the shell count
void ConkerFurTag(X86Context& ctx, uint8_t* base) {
    (void)base;
    fur::TagShells(ctx.ebx);
}

static bool FurDistant() {
    static const char* env = std::getenv("XBE_FUR_DISTANT");
    return env ? env[0] != '0' : fur::g_distant;
}

// [[hook]] at 0x8210C, xmm0 the object's distance from the player and xmm3 R
void ConkerFurDistant(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!FurDistant()) return;
    ctx.xmm[0].f[0] = std::min(ctx.xmm[0].f[0], ctx.xmm[3].f[0] * 0.4f);
}

// Beach Bloodbath (level 4, outside multiplayer [0x76F0E0]) draws its CombatSquirrels (type 0x58) with at most 4
// shells (esi, 16 elsewhere, then capped at 8) at half length ([esp+0xC], 0.5 for 1.0, multiplied into the length
// scale at 0x82248), so a beach full of them ran on an Xbox: thin cheeks, no fur on the eyebrows, where the same model
// in the bar (level 29) has its full coat. fur::g_distant gives them that back
// [[hook]] at 0x82071, after that case; ebx the level, edx the type
void ConkerFurBeach(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!FurDistant() || kernel::G<uint8_t>(0x76F0E0) || ctx.ebx != 4 || ctx.edx != 0x58) return;
    ctx.esi = 0x10;
    kernel::W32(ctx.esp + 0xC, 0x3F800000u);  // 1.0f
}

// 0x7F1E0 (edi the player in [0x84A5F8], 0x6D0 each, the viewport's index at [esp+0x28]) picks the model a player
// or bot is drawn with, of the [player+0x510] in its list at [player+0x50C]: its distance from the viewport's camera,
// weighed by the field of view, under 1000 the full model (0), under 5300 the middle one, past that the far blob
// (500 and 730 outside multiplayer), then a few cases that force the full one. At 0x7F452 the pick in ebx is clamped
// to the list and stored at [player+0x3A8], which the draw (0x7FB00), its skeleton ([player+0x4EC]) and the effects
// that skip coarse models ([0x7819D5], 0x822B0) read. characters::g_max_lod picks the full model always.
bool characters::g_max_lod = true;

// [[hook]] at 0x7F452, before the clamp
void ConkerCharacterLod(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (characters::g_max_lod) ctx.ebx = 0;
}

// The multiplayer classes' models, by id: four a class, registered by the frontend as it lists the classes (0x234840,
// names at 0x6A58F0): "Multi\<class>" the full one in a match, then "MultiFrontend\<class>" character select's,
// "MultiLOD\<class>" and "MultiLOD2\<class>", e.g. the WWII grunt's 0x1C5..0x1C8. A player's list ([player+0x50C])
// holds the full, middle and far ones. Character select's is finer for the squirrels, the only one with fur on the
// body and head (the match's only on the tail or hair), and for some Tediz (the Future ones, the WWII grunt).
// characters::g_menu_models draws a class's character select model in a match for its full one, either side's, the
// WWII classes' and the Future ones'.
bool characters::g_menu_models = false;

// a model's name, "" without one
static std::string ModelName(uint32_t id) {
    std::string out;
    if (id >= 0x205) return out;
    const uint32_t name = kernel::R32(0x6A58F0 + 4 * id);
    if (name < 0x10000) return out;
    for (uint32_t i = 0; i < 80; i++) {
        const char c = kernel::G<char>(name + i);
        if (!c) break;
        out += c;
    }
    return out;
}

// a class's full model (a squirrel's or a Tedi's), the one after it character select's
static bool HasMenuModel(uint32_t full) {
    return ModelName(full).rfind("Multi\\Combat", 0) == 0 && ModelName(full + 1).rfind("MultiFrontend\\", 0) == 0;
}

// the menu models asked for since the latest level change. Game thread
static std::vector<uint32_t> g_menu_requested;

// A match's level load takes a handle on each package it needs (0x63680, esi the handle, its name at +0xC: the
// level's, and its era's six classes of each side, aid\zpackage\Characters\Multi\<class>\default.rbm), then queues its
// load (0x5E5A0) on the loader the level load drains before players spawn. A class's character select model is in
// a package of its own (Characters\MultiFrontend\<class>), loaded by character select alone and released at each
// level change (0x863E0 releases the frontend's nodes, [0x84722C]): in a match it was gone, and a model whose package
// isn't in loads as an empty stand-in (0x99EF0's 0x75DB48), which the joint hits (0x2D06B0) read through, a crash.
// So as the match takes a class's package, its character select model is asked for as the frontend asks
// for a model (0x99CF0, eax the model: a node on the frontend's list, 0x99AD0, its load queued with 0x5E5A0), which
// the next level change releases with the rest of that list
X86_EXTERN_FUNC(sub_00099CF0);

// [[hook]] at 0x636AF, esi the handle, its resource found or made
void ConkerMenuPackages(X86Context& ctx, uint8_t* base) {
    if (!characters::g_menu_models) return;
    char name[160];
    uint32_t n = 0;
    for (; n + 1 < sizeof(name); n++) {
        const char c = kernel::G<char>(ctx.esi + 0xC + n);
        if (!c) break;
        name[n] = c;
    }
    name[n] = 0;
    static const char kPackage[] = "\\zpackage\\Characters\\Multi\\Combat";
    const char* at = strstr(name, kPackage);
    if (!at) return;
    const char* cls = at + sizeof("\\zpackage\\Characters\\Multi\\") - 1;
    const char* end = strchr(cls, '\\');
    const std::string want = "MultiFrontend\\" + std::string(cls, end ? size_t(end - cls) : strlen(cls));
    uint32_t model = 0;
    for (uint32_t id = 1; id < 0x205 && !model; id++)
        if (ModelName(id) == want && HasMenuModel(id - 1)) model = id;
    if (!model || std::find(g_menu_requested.begin(), g_menu_requested.end(), model) != g_menu_requested.end()) return;
    const X86Context saved = ctx;
    ctx.eax = model;
    X86_PUSH32(0x636AFu);
    sub_00099CF0(ctx, base);
    ctx = saved;
    g_menu_requested.push_back(model);
    LOGI("models: level %u loads %s (%u) too", kernel::R32(0x7FA1F8), want.c_str(), model);
}

// A level's start (0x2A9852..0x2A986A) empties the instance list ([0x847230]) and the frontend's list ([0x847228] its
// head: +4 the first node, each node +0 the one before, +4 the one after) without releasing them. The match's level
// load has asked for the menu models by then (ConkerMenuPackages), so their nodes were dropped there, never released at
// the level change (0x863E0) and their packages never freed: some 3.6 MB of the 34 MB GPU heap a match, until a later
// level's Asset_GPUAlloc found too little and the game rebooted itself on the out of memory error (0x1F05D0, unless
// DontRebootOnException.txt is there; a Future map after a WWII one, 8.6 MB asked with 6.6 MB free). When every node
// on the list is a menu model's, the list is put back after it
static uint32_t g_kept_list[2];
static bool g_keep_list;

// [[hook]] at 0x2A9852, the level's start, before the lists are emptied
void ConkerMenuKeepList(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    g_keep_list = false;
    const uint32_t first = kernel::R32(0x84722C);
    if (!characters::g_menu_models || g_menu_requested.empty() || first < 0x10000) return;
    int n = 0;
    for (uint32_t node = first; node; node = kernel::R32(node + 4), n++) {
        if (node < 0x10000 || n > 64) return;
        if (std::find(g_menu_requested.begin(), g_menu_requested.end(), kernel::R32(node + 8)) == g_menu_requested.end()) {
            LOGR("models: the frontend's list holds %s (%u) at a level's start, the menu models' nodes dropped",
                 ModelName(kernel::R32(node + 8)).c_str(), kernel::R32(node + 8));
            return;
        }
    }
    g_kept_list[0] = kernel::R32(0x847228), g_kept_list[1] = first;
    g_keep_list = true;
}

// [[hook]] at 0x2A9870, after them
void ConkerMenuKeptList(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (!g_keep_list) return;
    g_keep_list = false;
    kernel::W32(0x847228, g_kept_list[0]);
    kernel::W32(0x84722C, g_kept_list[1]);
}

// The physical heap the game's GPU assets come from (Asset_GPUAlloc, 0x63230, by 0x2AF430): made at 0x2AED70, at
// [0x84A188], 0x2070000 bytes; a debug display (0x9B27B) shows 9 words of it from +0x28, +0x30 its used bytes and
// +0x34 its free ones. Logged with the game's count of live allocations ([0x7FA4B4])
static void LogGpuHeap(const char* when) {
    const uint32_t heap = kernel::R32(0x84A188);
    if (heap < 0x10000) return;
    char words[160];
    int n = 0;
    for (uint32_t i = 0; i < 9; i++)
        n += std::snprintf(words + n, sizeof(words) - size_t(n), " %08X", kernel::R32(heap + 0x28 + 4 * i));
    LOGR("gpu heap: %s, level %u, live allocations %u, stats%s", when, kernel::R32(0x7FA1F8), kernel::R32(0x7FA4B4),
         words);
}

// [[hook]] at 0x1F05D0, the game's out of memory error (cdecl: the size, the allocation's tag), which reboots it
void ConkerOutOfMemory(X86Context& ctx, uint8_t* base) {
    (void)base;
    const uint32_t tag = kernel::R32(ctx.esp + 8);
    std::string name;
    for (uint32_t i = 0; tag >= 0x10000 && i < 64; i++) {
        const char c = kernel::G<char>(tag + i);
        if (!c) break;
        name += c;
    }
    LOGR("out of memory: %u bytes for %s", kernel::R32(ctx.esp + 4), name.c_str());
    LogGpuHeap("out of memory");
}

// [[hook]] at 0x8643D, the end of 0x863E0, the frontend's nodes released: those asked for with them
void ConkerMenuReset(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    g_menu_requested.clear();
}

// A player's setup (0x2B3260..) makes a copy of each of its models' scene for the player ([player+0x4C4 + 4i],
// 0x3ED030's size, 0x2AF0E0, 0x865C0), then its animation controller from the first model's "ndSkeleton" node
// (0x36A60, [player+0x3C4], [player+0x3C8] its handle), and hands that to every model's skeleton (0x2B3510, ebx the
// handle). The animations take only on a controller made on the class's own full model: a menu model first in the
// list stood in its bind pose, as did the class's middle one (2026-11-09). So the list stays the game's, and once
// the controller is handed out the menu model gets its instance (0x86440, eax the model, edi the type), a copy of its
// scene for the player, and the same controller (handed out through slot 0, which then gets the match model back:
// the menu model takes it for the player's draws alone, see MenuDrawn)
X86_EXTERN_FUNC(sub_00086440);
X86_EXTERN_FUNC(sub_003ED030);
X86_EXTERN_FUNC(sub_002AF0E0);
X86_EXTERN_FUNC(sub_000865C0);
X86_EXTERN_FUNC(sub_002B3510);
// XBE_FUR_SURVEY=1 (with fur.cpp's, each furry model's first draw): each level's models, logged at its first player's
// setup, after the level's packages are in. A model is in when its data is in the registry by name, as 0x99EF0 finds
// it: characters\<name> made by 0x5E4E0 (eax 0: into the game's own buffer; cdecl 3, "characters" 0x5F04F4, the
// name), looked up by 0x61ED0 (eax the name, the registry 0x84D458 pushed, popped by it). For the soak test (LT + RT
// + Black on the main menu: every level back to back)
X86_EXTERN_FUNC(sub_0005E4E0);
X86_EXTERN_FUNC(sub_00061ED0);
static void SurveyLevelModels(X86Context& ctx, uint8_t* base) {
    static const bool on = [] {
        const char* e = std::getenv("XBE_FUR_SURVEY");
        return e && *e && *e != '0';
    }();
    static uint32_t last = ~0u;
    const uint32_t level = kernel::R32(0x7FA1F8);
    if (!on || level == last) return;
    last = level;
    const X86Context saved = ctx;
    std::string line;
    int count = 0;
    for (uint32_t id = 0; id < 0x205; id++) {
        const uint32_t name = kernel::R32(0x6A58F0 + 4 * id);
        if (name < 0x10000) continue;
        ctx.eax = 0;
        X86_PUSH32(name);
        X86_PUSH32(0x5F04F4u);
        X86_PUSH32(3u);
        X86_PUSH32(0x2B3368u);
        sub_0005E4E0(ctx, base);
        ctx.esp += 12;
        const uint32_t key = ctx.eax;
        if (key < 0x10000) continue;
        X86_PUSH32(0x84D458u);
        ctx.eax = key;
        X86_PUSH32(0x2B3368u);
        sub_00061ED0(ctx, base);
        if (!ctx.eax) continue;
        line += (count++ ? ", " : "") + std::to_string(id) + " " + ModelName(id);
    }
    ctx = saved;
    LOGR("fur survey: level %u has %d models: %s", level, count, line.c_str());
}

// the players drawn with a character select model: that model, its instance and scene copy, and the match model's
// that slot 0 keeps. Game thread
//
// The game reads slot 0 ([player+0x4B0] instance, [player+0x4C4] scene copy, [player+0x4EC] skeleton node) outside
// the draw too, and expects the class's match model there: the camera took its height from it once after a spawn (with
// the character select model the camera sat 21 units low, the model drawn high against it: F3 captures 164954 and
// 165030; an F10 toggle test, 2026-11-12, put it right for good by holding the match model in slot 0 a moment), and
// effects keep indexes into its locator table ([scene+0x10]: 121 entries for the squirrel grunt's match model, 51
// for its character select one), which read through the other model a joint out of range (a crash at 0xC9356 on a
// class change). So slot 0 keeps the match model, and each draw of the player (0x7FC6E to 0x805A6) has the character
// select model's instance, copy and skeleton node in its place, its parts' shown bits copied by name from the match
// copy first (MirrorParts)
struct MenuDrawn {
    uint32_t model, inst, copy, match_inst, match_copy;
    uint32_t skel, match_skel;  // skeleton nodes ([player+0x4EC])
    std::vector<std::pair<uint16_t, uint16_t>> follow;  // bones following their parents (FollowBones)
    uint32_t level;  // [0x7FA1F8] at the setup
};
static std::unordered_map<uint32_t, MenuDrawn> g_menu_drawn;

// A player's models are released by 0x2B4310 (eax the player, from its teardown at 0x2F58D2): for each of its five
// slots the scene copy ([player+0x4C4 + 4i], 0x75640 with the model's scene), the slot's [player+0x4D8 + 4i], then a
// reference on the instance ([player+0x4B0 + 4i], [inst+8]), the model's data freed with the last. The menu model's
// instance and copy are no slot's, and were never released: 16 copies and instance references a match. So the
// release runs once more for them, alone in slot 0, the player's own slots put back after
X86_EXTERN_FUNC(sub_002B4310);
static void ReleaseMenuModel(X86Context& ctx, uint8_t* base, uint32_t player, const MenuDrawn& d) {
    uint32_t slots[15];
    for (uint32_t i = 0; i < 15; i++) slots[i] = kernel::R32(player + 0x4B0 + 4 * i);
    for (uint32_t i = 0; i < 15; i++) kernel::W32(player + 0x4B0 + 4 * i, 0);
    kernel::W32(player + 0x4B0, d.inst);
    kernel::W32(player + 0x4C4, d.copy);
    const X86Context saved = ctx;
    ctx.eax = player;
    X86_PUSH32(0x2F58D7u);
    sub_002B4310(ctx, base);
    ctx = saved;
    for (uint32_t i = 0; i < 15; i++) kernel::W32(player + 0x4B0 + 4 * i, slots[i]);
    LOGI("models: player %08X releases %s (%u), instance %08X, copy %08X", player, ModelName(d.model).c_str(), d.model,
         d.inst, d.copy);
}

// [[hook]] at 0x2F58D2, a player's teardown about to release its models (0x2B4310); ebp the player
void ConkerMenuRelease(X86Context& ctx, uint8_t* base) {
    const auto it = g_menu_drawn.find(ctx.ebp);
    if (it == g_menu_drawn.end()) return;
    // the game's own release skips the frontend's players' instances (type 0xB, 0x48, none) and ends in the
    // frontend's (level 0x46) with 0x99E50; neither has a menu model
    const uint32_t type = kernel::R32(ctx.ebp + 0x68);
    if (type != 0xFFFF && type != 0x48 && type != 0xB && kernel::R32(0x7FA1F8) != 0x46)
        ReleaseMenuModel(ctx, base, ctx.ebp, it->second);
    g_menu_drawn.erase(it);
}

static std::vector<std::pair<uint16_t, uint16_t>> FollowBones(uint32_t skel, uint32_t match_skel);

// [[hook]] at 0x2B3368, after 0x2B3510; ebp the player
void ConkerMenuSkeleton(X86Context& ctx, uint8_t* base) {
    SurveyLevelModels(ctx, base);
    // set up again without a teardown (a class change in the same level): the menu model it had is released first;
    // one left from another level is dropped, that level's models gone with it
    if (const auto it = g_menu_drawn.find(ctx.ebp); it != g_menu_drawn.end()) {
        if (it->second.level == kernel::R32(0x7FA1F8) && kernel::R32(0x7FA1F8) != 0x46)
            ReleaseMenuModel(ctx, base, ctx.ebp, it->second);
        g_menu_drawn.erase(it);
    }
    if (!characters::g_menu_models) return;
    const uint32_t player = ctx.ebp, list = kernel::R32(player + 0x50C);
    if (list < 0x10000 || kernel::R32(player + 0x4B0) == 0) return;
    const uint32_t full = kernel::R32(list), model = full + 1, type = kernel::R32(player + 0x68);
    if (!HasMenuModel(full)) return;
    if (std::find(g_menu_requested.begin(), g_menu_requested.end(), model) == g_menu_requested.end()) {
        LOGI("models: %s keeps its match model: its character select one isn't loaded", ModelName(full).c_str());
        return;
    }
    const X86Context saved = ctx;
    ctx.eax = model, ctx.edi = type;
    X86_PUSH32(0x2B3368u);
    sub_00086440(ctx, base);
    const uint32_t inst = ctx.eax, data = inst >= 0x10000 ? kernel::R32(inst + 0x24) : 0;
    const uint32_t scene = data >= 0x10000 ? kernel::R32(data + 0x8) : 0;
    if (inst == 0x75DB48 || scene < 0x10000) {
        ctx = saved;
        LOGR("models: %s has no instance (%08X), the match model kept", ModelName(model).c_str(), inst);
        return;
    }
    ctx.ecx = scene;
    X86_PUSH32(0x2B3368u);
    sub_003ED030(ctx, base);
    const uint32_t size = ctx.eax;
    X86_PUSH32(0xFFFFFFFFu);
    X86_PUSH32(0x4B9u);
    X86_PUSH32(0x606AA8u);
    X86_PUSH32(size);
    X86_PUSH32(0x2B3368u);
    sub_002AF0E0(ctx, base);
    ctx.esp += 16;
    const uint32_t copy = ctx.eax;
    if (copy < 0x10000) {
        ctx = saved;
        LOGR("models: no memory for %s's scene (%u bytes), the match model kept", ModelName(model).c_str(), size);
        return;
    }
    X86_PUSH32(copy);
    X86_PUSH32(scene);
    X86_PUSH32(0x2B3368u);
    sub_000865C0(ctx, base);
    ctx.esp += 8;
    const uint32_t match_inst = kernel::R32(player + 0x4B0), match_copy = kernel::R32(player + 0x4C4);
    const uint32_t match_skel = kernel::R32(player + 0x4EC);
    kernel::W32(player + 0x4B0, inst);
    kernel::W32(player + 0x4C4, copy);
    ctx.ecx = player, ctx.ebx = kernel::R32(player + 0x3C8);
    X86_PUSH32(0x2B3368u);
    sub_002B3510(ctx, base);
    ctx = saved;
    const uint32_t skel = kernel::R32(player + 0x4EC);
    // slot 0 keeps the match model between draws
    kernel::W32(player + 0x4B0, match_inst);
    kernel::W32(player + 0x4C4, match_copy);
    kernel::W32(player + 0x4EC, match_skel);
    auto follow = FollowBones(skel, match_skel);
    std::string follows;
    for (const auto& [b, parent] : follow) follows += " " + std::to_string(b) + "<-" + std::to_string(parent);
    g_menu_drawn[player] = {model, inst, copy, match_inst, match_copy, skel, match_skel, std::move(follow), kernel::R32(0x7FA1F8)};
    LOGI("models: player %08X draws %s (%u), instance %08X, scene %08X, bones following their parents:%s", player,
         ModelName(model).c_str(), model, inst, scene, follows.empty() ? " none" : follows.c_str());
}

// A player's draw (0x7FB00, ebx the player) draws its model's scene twice through [0x81BD70]: its solid pieces
// (0x8004D, pass 1), then its see-through ones with no depth written (0x801AE, pass 2). The Tediz Poisoner's
// character select models, both eras', have the helmet see-through, its alpha the texture's (F3 capture 190904,
// draws 1413-1414): drawn in a match for the class's full model (characters::g_menu_models), their pass 2 is drawn
// solid (d3d::OpaqueDraws); the helmet is all that pass has
static bool OpaqueHelmet(uint32_t player) {
    const auto it = g_menu_drawn.find(player);
    if (it == g_menu_drawn.end() || kernel::R32(player + 0x3A8) != 0 || kernel::R32(player + 0x4B0) != it->second.inst)
        return false;
    const uint32_t model = it->second.model;
    static std::unordered_map<uint32_t, bool> poisoner;  // by model
    const auto p = poisoner.find(model);
    if (p != poisoner.end()) return p->second;
    const std::string name = ModelName(model);
    return poisoner[model] = name == "MultiFrontend\\CombatTedizMultiPoisoner" ||
                             name == "MultiFrontend\\CombatTedizMultiWWIIPoisoner";
}

// The draw's effect passes (0x7FC05..0x7FD43: flags at [esp+0xE..0x11] by the draw's kind [ebp+0x18]) each run only
// when the drawn model's data allows them (its word +0xC: bit 1 [esp+0xE], 2 [esp+0xF], 8 [esp+0x10], 0x20
// [esp+0x11]), and 0x82730 draws them with an effect's entries of that model's own tables ([data+0x10], [data+0x14],
// by its words +0x1C..+0x26), the effect picked by the player's state: cloaked when [[player+0x568]+0x624] is set
// (0x2C38A0; its draws are of kind 10), others in action states 0x3D and 0x3F ([player+0x64], 0x82A84). The
// Sneaker's shield is none of those: it is up while the byte at [[player+0x568]+0x5DD] is set, from the raise
// (animation 0x54E at +0x474) to the lower (0x54D), found by diffing the player's objects across two shields
// (2026-11-12); the match model draws it, the character select model has no shield. The character select models have
// none of the effects' entries, so a cloaked player lost only its fur and a shielded one showed nothing. A match draws each
// player four times a frame (kinds 3, 1, 9, 2), stencil passes over each other's pixels; kind 2 asks for pass 2
// every frame, and drawn with the match model under the menu model's other passes it left a shell on screen at all
// times (2026-11-12). So while a player is cloaked or shielded every draw of it is of its match model, the one slot 0
// holds between draws; otherwise its character select model is swapped in for the draw. The player swapped, 0 for
// none. Game thread
static uint32_t g_effect_player = 0;

// XBE_SHIELD_TRACE=1: a Sneaker's shield state, each time it changes: [[player+0x568]+0x6F8] (word and float),
// the byte at +0x5DD, the animations at [player+0x474] and [[player+0x568]+0x474], the player's action state
// [player+0x64] and state byte [player+0xA1]. To find what marks the shield up after a respawn or a class change
static void TraceShield(uint32_t player, uint32_t owner) {
    static const bool on = [] {
        const char* e = std::getenv("XBE_SHIELD_TRACE");
        return e && *e && *e != '0';
    }();
    if (!on || owner < 0x10000) return;
    const uint32_t w = kernel::R32(owner + 0x6F8);
    float f;
    std::memcpy(&f, &w, 4);
    char line[200];
    std::snprintf(line, sizeof(line), "6F8 %08X (%g) 5DD %u anim %X/%X act %02X state %u", w, f,
                  kernel::G<uint8_t>(owner + 0x5DD), kernel::R32(player + 0x474), kernel::R32(owner + 0x474),
                  kernel::R32(player + 0x64), kernel::G<uint8_t>(player + 0xA1));
    static std::unordered_map<uint32_t, std::string> last;  // by player
    if (last[player] == line) return;
    last[player] = line;
    LOGR("shield: player %08X owner %08X %s", player, owner, line);
}

// Infravision (the sniper's scope and the Poisoner's mask): 0x82730 asks 0x2C38C0 of the viewport's own player
// ([[0x8470dc] + viewport * 0x770 + 0x3E0]), on when the byte at +8 of [[[viewer+0x568]+0x444]+0x280] is set, and
// then draws every other player with its model's effect entry +0x1C through 0x82690: the global effect texture 0 (a
// 64x1 heat ramp) in stage 0 on generated coordinates, the living body glow (F3 captures 031301_0, 031304_0). The
// character select models have no such entry and drew as they are. 0x2C38C0 also turns it off for one target near
// its viewer (0x2C3DA0, 0x36E70); that player keeps the match model too, drawn plain
static bool Infravision(uint32_t viewport) {
    const uint32_t views = kernel::R32(0x8470dc);
    if (views < 0x10000 || viewport > 3) return false;
    const uint32_t viewer = kernel::R32(views + viewport * 0x770 + 0x3E0);
    if (viewer < 0x10000) return false;
    const uint32_t owner = kernel::R32(viewer + 0x568);
    if (owner < 0x10000) return false;
    const uint32_t item = kernel::R32(owner + 0x444);
    if (item < 0x10000) return false;
    const uint32_t state = kernel::R32(item + 0x280);
    return state >= 0x10000 && kernel::G<uint8_t>(state + 8) != 0;
}

static bool EffectOn(uint32_t player, uint32_t kind, uint32_t model, uint32_t viewport) {
    const uint32_t owner = kernel::R32(player + 0x568), act = kernel::R32(player + 0x64);
    if (kind == 10 || act == 0x3D || act == 0x3F) return true;
    if (Infravision(viewport)) return true;
    if (owner < 0x10000) return false;
    if (kernel::G<uint8_t>(owner + 0x624)) return true;
    static std::unordered_map<uint32_t, bool> sneaker;  // by model
    auto s = sneaker.find(model);
    if (s == sneaker.end()) s = sneaker.emplace(model, ModelName(model).find("Sneaker") != std::string::npos).first;
    if (s->second && kind == 1) TraceShield(player, owner);
    // the byte at +0x5DD alone: 1 from the raise (animation 0x54E at [owner+0x474]) to the lower (0x54D), on a first
    // life and after a respawn or a class change alike (XBE_SHIELD_TRACE, 2026-11-12); the word at +0x6F8 is 1 only
    // on a first life, floats (1.0f) and other values after, which drew a Sneaker with its match model at random
    return s->second && kernel::G<uint8_t>(owner + 0x5DD) != 0;
}

// XBE_MENU_EFFECT_TRACE=1: a player drawn with a menu model, each time a kind of its draws changes what it asks for:
// the kind [ebp+0x18], viewport [ebp+0xC] and alpha [ebp+0x14], the flags at [esp+0xE..0x11], the player's action
// state [player+0x64] and state [player+0xA1], the cloak's [[player+0x568]+0x624], and the model drawn; with the
// draws of that kind since the last line
static void TraceMenuEffects(X86Context& ctx, uint32_t player, bool swap) {
    static const bool on = [] {
        const char* e = std::getenv("XBE_MENU_EFFECT_TRACE");
        return e && *e && *e != '0';
    }();
    if (!on) return;
    char line[360];
    const uint32_t owner = kernel::R32(player + 0x568), kind = kernel::R32(ctx.ebp + 0x18);
    std::snprintf(line, sizeof(line),
                  "kind %u viewport %u alpha %u flags e%u f%u g%u h%u act %02X state %u cloak %u shield %u/%u: %s",
                  kind, kernel::R32(ctx.ebp + 0xC), kernel::R32(ctx.ebp + 0x14), kernel::G<uint8_t>(ctx.esp + 0xE),
                  kernel::G<uint8_t>(ctx.esp + 0xF), kernel::G<uint8_t>(ctx.esp + 0x10),
                  kernel::G<uint8_t>(ctx.esp + 0x11), kernel::R32(player + 0x64), kernel::G<uint8_t>(player + 0xA1),
                  owner >= 0x10000 ? kernel::G<uint8_t>(owner + 0x624) : 0u,
                  owner >= 0x10000 ? kernel::R32(owner + 0x6F8) : 0u,
                  owner >= 0x10000 ? kernel::G<uint8_t>(owner + 0x5DD) : 0u,
                  swap ? "match model" : "menu model");
    struct Seen {
        std::string line;
        uint32_t draws = 0;
    };
    static std::unordered_map<uint64_t, Seen> seen;  // by player, viewport and kind
    Seen& s = seen[uint64_t(player) << 16 | (kernel::R32(ctx.ebp + 0xC) & 0xFF) << 8 | (kind & 0xFF)];
    s.draws++;
    if (s.line == line) return;
    LOGR("menu effects: player %08X %s (after %u draws)", player, line, s.draws);
    s.line = line;
    s.draws = 0;
}

static bool GuestPtr(uint32_t p) { return p >= 0x10000 && mem::IsCommitted(p) && mem::IsCommitted(p + 0x5F); }

// A scene's parts ([scene+0x14]: a count, then 20-byte entries, the name's address at +0 and the bit at +8) are shown
// or hidden by name on the player's copy in slot 0 (0x2F7A50, 0x81E60: bit n of the bytes at [copy+0xC], 0x80 >> n %
// 8), the ears, head or tail shot off among them. The two models' parts are matched by name past the group (the first
// "|name" unless it is a part's), ignoring case: the match model's are "|CombatTedizMultiDemolisher_group1|_PART_HEAD"
// where character select's are "|group1|_PART_head", and so for the squirrel grunt's and the Tediz poisoner's and
// sneaker's (XBE_MENU_PARTS_TRACE, 2026-11-16). Character select's have no stump, no force field and, the squirrels,
// no tail: their LOWER holds the tail and its fur. So the pilot of a closed cockpit, its tail hidden (0x38BF7B: the
// part "TAIL" by 0x2F7A50), drew the menu model's tail fur through the hull. While a part character select's lack is
// out of its usual state (a stump or force field shown, any other hidden) the player keeps its match model
static std::string GuestString(uint32_t at) {
    std::string out;
    for (uint32_t i = 0; GuestPtr(at) && i < 64; i++) {
        const char c = kernel::G<char>(at + i);
        if (!c) break;
        out += c;
    }
    return out;
}

// XBE_MENU_PARTS_TRACE=1: each model pair's parts (name and bit) once, then the match copy's hidden parts by name each
// time they change
static std::string PartKey(std::string n) {
    for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() > 1 && n[0] == '|' && n.compare(1, 5, "_part") != 0) {
        const size_t next = n.find('|', 1);
        if (next != std::string::npos) n.erase(0, next);
    }
    return n;
}

static bool PartsTrace() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_MENU_PARTS_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}

// the bits mirrored, false when the match model must stand in (a part character select's lack out of its usual state)
static bool MirrorParts(const MenuDrawn& d) {
    struct Map {
        std::vector<std::pair<uint32_t, uint32_t>> pairs;  // (match bit, character select bit)
        std::vector<std::pair<uint32_t, bool>> lone;        // the match parts character select's lack: bit, usually shown
    };
    static std::unordered_map<uint64_t, Map> maps;  // by both instances
    auto parts = [](uint32_t inst) -> uint32_t {
        const uint32_t data = GuestPtr(inst) ? kernel::R32(inst + 0x24) : 0;
        const uint32_t scene = GuestPtr(data) ? kernel::R32(data + 0x8) : 0;
        return GuestPtr(scene) ? kernel::R32(scene + 0x14) : 0;
    };
    const uint64_t key = uint64_t(d.match_inst) << 32 | d.inst;
    auto it = maps.find(key);
    if (it == maps.end()) {
        Map map;
        auto& pairs = map.pairs;
        const uint32_t a = parts(d.match_inst), b = parts(d.inst);
        if (GuestPtr(a) && GuestPtr(b)) {
            const uint32_t na = std::min(kernel::R32(a), 256u), nb = std::min(kernel::R32(b), 256u);
            const uint32_t ea = kernel::R32(a + 4), eb = kernel::R32(b + 4);
            std::unordered_map<std::string, uint32_t> menu_bits;
            for (uint32_t i = 0; i < nb && GuestPtr(eb + 20 * i); i++)
                menu_bits.emplace(PartKey(GuestString(kernel::R32(eb + 20 * i))), kernel::R32(eb + 20 * i + 8));
            for (uint32_t i = 0; i < na && GuestPtr(ea + 20 * i); i++) {
                const std::string name = PartKey(GuestString(kernel::R32(ea + 20 * i)));
                const uint32_t bit = kernel::R32(ea + 20 * i + 8);
                if (name.empty()) continue;
                const auto m = menu_bits.find(name);
                if (m != menu_bits.end())
                    pairs.emplace_back(bit, m->second);
                else
                    map.lone.emplace_back(bit, name.find("_part_stump") == std::string::npos &&
                                                   name.find("_part_forcefield") == std::string::npos);
            }
            if (PartsTrace()) {
                std::string la, lb;
                for (uint32_t i = 0; i < na && GuestPtr(ea + 20 * i); i++)
                    la += " " + GuestString(kernel::R32(ea + 20 * i)) + "=" + std::to_string(kernel::R32(ea + 20 * i + 8));
                for (uint32_t i = 0; i < nb && GuestPtr(eb + 20 * i); i++)
                    lb += " " + GuestString(kernel::R32(eb + 20 * i)) + "=" + std::to_string(kernel::R32(eb + 20 * i + 8));
                LOGR("menu parts: match model %08X has %u parts:%s", d.match_inst, na, la.c_str());
                LOGR("menu parts: %s has %u parts:%s", ModelName(d.model).c_str(), nb, lb.c_str());
            }
        }
        LOGI("models: %s shares %zu parts with its match model, lacks %zu", ModelName(d.model).c_str(), pairs.size(),
             map.lone.size());
        it = maps.emplace(key, std::move(map)).first;
    }
    const uint32_t from = kernel::R32(d.match_copy + 0xC), to = kernel::R32(d.copy + 0xC);
    if (!GuestPtr(from) || !GuestPtr(to)) return true;
    if (PartsTrace()) {
        // the match copy's hidden parts by name, each time they change
        std::string hidden;
        const uint32_t a = parts(d.match_inst);
        if (GuestPtr(a)) {
            const uint32_t na = std::min(kernel::R32(a), 256u), ea = kernel::R32(a + 4);
            for (uint32_t i = 0; i < na && GuestPtr(ea + 20 * i); i++) {
                const uint32_t bit = kernel::R32(ea + 20 * i + 8);
                if (bit <= 0x7FF && !(kernel::G<uint8_t>(from + (bit >> 3)) & (0x80 >> (bit & 7))))
                    hidden += " " + GuestString(kernel::R32(ea + 20 * i));
            }
        }
        static std::unordered_map<uint32_t, std::string> last;  // by match copy
        if (last[d.match_copy] != hidden) {
            last[d.match_copy] = hidden;
            LOGR("menu parts: match copy %08X (%s) hides:%s", d.match_copy, ModelName(d.model).c_str(),
                 hidden.empty() ? " none" : hidden.c_str());
        }
    }
    for (const auto& [bit, usual] : it->second.lone)
        if (bit <= 0x7FF && bool(kernel::G<uint8_t>(from + (bit >> 3)) & (0x80 >> (bit & 7))) != usual) return false;
    for (const auto& [mb, cb] : it->second.pairs) {
        if (mb > 0x7FF || cb > 0x7FF) continue;
        const bool on = kernel::G<uint8_t>(from + (mb >> 3)) & (0x80 >> (mb & 7));
        uint8_t& byte = kernel::G<uint8_t>(to + (cb >> 3));
        byte = on ? byte | (0x80 >> (cb & 7)) : byte & ~(0x80 >> (cb & 7));
    }
    return true;
}

// The character select model's skeleton is the match model's, bone for bone (29 for the WWII classes, the same
// hierarchy), but its pieces skin to a bone the match model's don't: the WWII sharpshooter squirrel's nose to bone
// 15, a child of the head (8). The match animations leave it some degrees off the head (the matrices logged
// 2026-11-15), and the nose cracked open from his face at times, the head piece and the nose meeting at vertices each
// side skins to its own bone. So each bone the character select model's palettes take and the match model's don't
// follows its parent: its matrix is the parent's, the bones all being the same matrix in the rest pose.
//
// A skeleton node ([node+0x30] bones, [node+0x34] a record of 36 bytes each, its parent the low word at +0x18, 0xFFFF
// for none) has its palette nodes, "ndMtxArray" (the registry entry at [node+0x10] begins with the type's name), in
// its subtree (first child +0x14, next sibling +0x18): [node+0x34] entries of 8 bytes at [node+0x30], the bone the
// first word
static void MtxArrayBones(uint32_t skel, std::vector<bool>& used) {
    std::vector<uint32_t> stack{GuestPtr(skel) ? kernel::R32(skel + 0x14) : 0};
    for (int seen = 0; !stack.empty() && seen < 512; seen++) {
        const uint32_t n = stack.back();
        stack.pop_back();
        if (!GuestPtr(n)) continue;
        stack.push_back(kernel::R32(n + 0x18));
        stack.push_back(kernel::R32(n + 0x14));
        if (GuestString(kernel::R32(n + 0x10)) != "ndMtxArray") continue;
        const uint32_t table = kernel::R32(n + 0x30), count = std::min(kernel::R32(n + 0x34), 256u);
        for (uint32_t i = 0; i < count && GuestPtr(table + 8 * i); i++) {
            const uint16_t b = kernel::G<uint16_t>(table + 8 * i);
            if (b < used.size()) used[b] = true;
        }
    }
}

// the (bone, parent) pairs of the character select skeleton whose bone the match model doesn't skin to, parents first
static std::vector<std::pair<uint16_t, uint16_t>> FollowBones(uint32_t skel, uint32_t match_skel) {
    std::vector<std::pair<uint16_t, uint16_t>> out;
    if (!GuestPtr(skel) || !GuestPtr(match_skel)) return out;
    const uint32_t count = kernel::R32(skel + 0x30), records = kernel::R32(skel + 0x34);
    if (count == 0 || count > 128 || kernel::R32(match_skel + 0x30) != count) return out;
    std::vector<bool> menu(count), match(count);
    MtxArrayBones(skel, menu);
    MtxArrayBones(match_skel, match);
    for (uint32_t b = 0; b < count; b++) {
        if (!menu[b] || match[b] || !GuestPtr(records + 36 * b)) continue;
        const uint16_t parent = kernel::G<uint16_t>(records + 36 * b + 0x18);
        if (parent < count) out.emplace_back(uint16_t(b), parent);
    }
    // a bone after its parent when both follow
    std::vector<std::pair<uint16_t, uint16_t>> ordered;
    std::vector<bool> done(count);
    for (size_t pass = 0; pass < out.size() && ordered.size() < out.size(); pass++)
        for (const auto& [b, parent] : out) {
            const bool parent_follows =
                std::any_of(out.begin(), out.end(), [&](const auto& o) { return o.first == parent; });
            if (!done[b] && (!parent_follows || done[parent])) {
                done[b] = true;
                ordered.emplace_back(b, parent);
            }
        }
    return ordered;
}

void characters::Skeleton(uint32_t node, uint32_t data) {
    if (!node || g_menu_drawn.empty()) return;
    for (const auto& [player, d] : g_menu_drawn) {
        if (node != d.skel || d.follow.empty()) continue;
        const uint32_t mats = kernel::R32(kernel::R32(node + 0xC) + data + 4);
        for (const auto& [b, parent] : d.follow) {
            if (!GuestPtr(mats + 64 * std::max(b, parent))) return;
            std::memmove(kernel::g_base + mats + 64 * b, kernel::g_base + mats + 64 * parent, 64);
        }
        return;
    }
}

// slot 0 between draws: the match model
static void SlotMatch(uint32_t player, const MenuDrawn& d) {
    kernel::W32(player + 0x4B0, d.match_inst);
    kernel::W32(player + 0x4C4, d.match_copy);
    kernel::W32(player + 0x4EC, d.match_skel);
}

// The character select model into slot 0 for one draw of the player (kind [ebp+0x18] of 0x7FB00's, 8 for the
// shine pass, viewport the one drawn into): unless cloaked, shielded or seen through infravision, its parts' bits
// mirrored first. True when swapped
static bool SwapInMenu(X86Context& ctx, uint32_t player, uint32_t kind, uint32_t viewport) {
    const auto it = g_menu_drawn.find(player);
    if (it == g_menu_drawn.end() || kernel::G<uint8_t>(player + 0x3A8) != 0) return false;
    const MenuDrawn& d = it->second;
    if (kernel::R32(player + 0x4B0) != d.match_inst || kernel::R32(player + 0x4C4) != d.match_copy) {
        static std::unordered_map<uint32_t, uint32_t> told;  // by player: the slot 0 instance last logged
        if (told[player] != kernel::R32(player + 0x4B0)) {
            told[player] = kernel::R32(player + 0x4B0);
            LOGR("models: player %08X slot 0 holds %08X/%08X, not its match model %08X/%08X (%s): drawn as it is",
                 player, kernel::R32(player + 0x4B0), kernel::R32(player + 0x4C4), d.match_inst, d.match_copy,
                 ModelName(d.model).c_str());
        }
        return false;
    }
    const bool effect = EffectOn(player, kind, d.model, viewport);
    if (kind != 8) TraceMenuEffects(ctx, player, effect);
    if (effect || !MirrorParts(d)) return false;
    kernel::W32(player + 0x4B0, d.inst);
    kernel::W32(player + 0x4C4, d.copy);
    kernel::W32(player + 0x4EC, d.skel);
    return true;
}

static void RestoreMatch(uint32_t& player) {
    if (!player) return;
    const auto it = g_menu_drawn.find(player);
    if (it != g_menu_drawn.end()) SlotMatch(player, it->second);
    player = 0;
}

// [[hook]] at 0x7FC6E, the effect flags set, before the model's own are read; ebx the player
void ConkerMenuEffects(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (SwapInMenu(ctx, ctx.ebx, kernel::R32(ctx.ebp + 0x18), kernel::R32(ctx.ebp + 0xC) & 0xFFFF))
        g_effect_player = ctx.ebx;
}

// [[hook]] at 0x805A6, the draw's end: the match model back in slot 0
void ConkerMenuEffectsEnd(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    RestoreMatch(g_effect_player);
}

// 0x7F4D0 (edi the player, esi the viewport) draws a player's kind 8 pass on its own, outside 0x7FB00: from 0x7F5AB
// it reads slot 0 ([player+0x4B0 + 4 lod], [player+0x4C4 + 4 lod]) and has 0x82730 draw that model's effect entries
// with mode 8, the shine on the player's own squirrel helmet; every way through reaches 0x7F709 before its tail call.
// With the match model there its helmet's shine flickered over the character select model's while moving
// (2026-11-12), so the pass gets the character select model as the draw does. The player swapped, 0 for none
static uint32_t g_shine_player = 0;

// [[hook]] at 0x7F5AB, edi the player
void ConkerMenuShine(X86Context& ctx, uint8_t* base) {
    (void)base;
    RestoreMatch(g_shine_player);
    if (SwapInMenu(ctx, ctx.edi, 8, ctx.esi & 0xFFFF)) g_shine_player = ctx.edi;
}

// [[hook]] at 0x7F709, the kind 8 pass's end
void ConkerMenuShineEnd(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    RestoreMatch(g_shine_player);
}

// [[hook]] at 0x801A2, before pass 2; ebx the player
void ConkerSolidHelmet(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (OpaqueHelmet(ctx.ebx)) d3d::OpaqueDraws(true);
}

// [[hook]] at 0x801B7, after pass 2
void ConkerSolidHelmetEnd(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    d3d::OpaqueDraws(false);
}

// 0x1310D0 paces a player's refresh in 0x7F96B's branch (the arg the player, esi its pacing: the frame last seen, the
// share built up): each frame its share grows by 0x978D0's, 1 when its size on screen ([player+0x67C]) isn't known
// yet, else that size / 100 (halved in split screen), and a whole share runs it, at most 4 players a frame (the
// count at 0x131129). A far one ran a few times a second. With characters::g_max_lod each runs every frame.
// [[hook]] at 0x1310EF, xmm0 the frame's share
void ConkerCharacterPace(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (characters::g_max_lod) ctx.xmm[0].f[0] = std::max(ctx.xmm[0].f[0], 1.0f);
}

// [[hook]] at 0x131129, ecx the players run this frame with this one
void ConkerCharacterPaceCount(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (characters::g_max_lod) ctx.ecx = std::min<uint32_t>(ctx.ecx, 1);
}

// 0x2C5260 (eax the player, kept at [0x7FAAB8]): whether a player's animation skips this frame, al 1 to hold its
// pose (0x2C53C0). In multiplayer it weighs the player's largest size on screen of the viewports ([player+0x67C]
// on): above [0x75E9E0] it animates every frame, above [0x75E9DC] every 2nd, above [0x75E9D8] every 4th, else every
// 8th, each player on its own frames ((frame [0x7F9F60] ^ [player+0x60]) & 1, 3 or 7). A far one moved at 15 FPS
// or less. With characters::g_max_lod none skips.
X86_EXTERN_FUNC(__imp__sub_002C5260);
X86_FUNC(sub_002C5260) {
    if (characters::g_max_lod) {
        kernel::W32(0x7FAAB8, ctx.eax);
        kernel::Return(ctx, ctx.eax & ~0xFFu, 0);
        return;
    }
    __imp__sub_002C5260(ctx, base);
}

// 0x4C950: the controller layer's init. It first opens the pads XGetDevices reports (0x3D19C0, which marks each
// one inserted at byte 0x13C of its 0x140-byte record in the table at [0x806FD8]), then replaces that table with a
// zeroed one. The stick filter (0x4CF80) zeroes both sticks of a pad not marked inserted, while buttons pass. On
// the console the pads enumerate later and arrive through XGetDeviceChanges into the new table; the runtime has
// them present from XInitDevices on, so mark the pads that were opened (handles at 0x806A48, record slot per port
// at 0x80692C) in the new table.
X86_EXTERN_FUNC(__imp__sub_0004C950);
X86_FUNC(sub_0004C950) {
    __imp__sub_0004C950(ctx, base);
    using kernel::R32;
    for (uint32_t port = 0; port < 4; port++)
        if (R32(0x806A48 + port * 4)) base[R32(0x806FD8) + base[0x80692C + port] * 0x140 + 0x13C] = 1;
}

// 0x538D50: begin a frame's render target and clear it. It called the library's pushbuffer submit/kickoff
// (0x53F0E0, 0x53F2F0) which read the miniport; do the same work through the host's D3D8 implementation.
// The library used SetRenderTarget(backbuffer, autoDepth) then Clear(target|depth, black, z=1).
X86_FUNC(sub_00538D50) {
    (void)base;
    uint32_t dev = kernel::R32(kD3D_pDevice);
    uint32_t rt = kernel::R32(dev + kDevBackBuffer);
    uint32_t zs = kernel::R32(dev + kDevAutoDepth);
    GUEST_CALL(D3DDevice_SetRenderTarget, ctx, {rt, zs});
    GUEST_CALL(D3DDevice_Clear, ctx, {0, 0, 3, 0, 0x3F800000u, 0});
    kernel::Return(ctx, 0, 0);
}

// 0x4C3F2F: a DSound buffer helper (buffer, flag). It reads a sub-object the library's buffer constructor
// allocates at [buffer+0x10] and sets or clears bit 0 of its byte 0xb. The runtime keeps DSound objects
// host-side (dsound.cpp's Voice map), so a buffer made by its HLE has no such sub-object, and the engine's
// XACT audio path calls this through library code the runtime does not replace. There is nothing to toggle.
X86_FUNC(sub_004C3F2F) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// The library's two voice-descriptor setters, stream 0x4C5235 (stream, descriptor; it jumps to 0x4C4EF5) and buffer
// 0x4C51B8 (buffer, descriptor; it calls 0x4C4BCB). Their shared body 0x4C3DFE stores the descriptor through the
// console voice's [voice+0xb8], which the runtime's host objects do not build. XACT fills the descriptor from the
// 3D calculator below (0x4C4613 writes it): +4 distance and +8 cone volume, +0x1C the I3DL2 direct path (occlusion),
// in hundredths of a dB, +0x24 the Doppler pitch, +0x28 the azimuth in degrees. The fields keep their last values;
// the flags at +0 say which the voice takes. XACT (0x54F082) computes every field for both voices of a 3D sound
// played through a submix, then masks the flags: the submix keeps its pan and filters but not the distance and cone
// volumes (0xDC), the voice feeding it those volumes but not the pan (0x33), so the distance is heard once. Hand the
// voice its 3D attenuation and pan; without it every 3D sound played at full volume everywhere.
static void Voice3d(uint32_t obj, uint32_t d) {
    using kernel::R32;
    auto i = [&](uint32_t o) { return int32_t(R32(d + o)); };
    auto f = [&](uint32_t o) {
        float v;
        uint32_t u = R32(d + o);
        std::memcpy(&v, &u, 4);
        return v;
    };
    static const bool trace = std::getenv("XBE_TRACE_DSOUND") != nullptr;
    if (trace)
        LOGR("dsound3d: %08X flags %02X dist %d cone %d pairA %d %d pairB %d %d i3dl2 %d %d pitch %d ang %.1f %.2f "
             "filter %d %d",
             obj, R32(d), i(4), i(8), i(0xC), i(0x10), i(0x14), i(0x18), i(0x1C), i(0x20), i(0x24), f(0x28), f(0x2C),
             i(0x30), i(0x34));
    dsound::Voice3dTerms t{};
    t.dist = i(4);
    t.cone = i(8);
    t.pair_a[0] = i(0xC);
    t.pair_a[1] = i(0x10);
    t.pair_b[0] = i(0x14);
    t.pair_b[1] = i(0x18);
    t.direct = i(0x1C);
    t.room = i(0x20);
    t.pitch = i(0x24);
    t.azimuth = f(0x28);
    t.filter[0] = i(0x30);
    t.filter[1] = i(0x34);
    dsound::SetVoice3d(obj, R32(d), t);
}
// hooked at the entries the title calls (the recompiler folds 0x4C4EF5 into 0x4C5235's jump), both (object, descriptor)
X86_FUNC(sub_004C5235) {
    (void)base;
    Voice3d(kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8));
    kernel::Return(ctx, 0, 2);
}
X86_FUNC(sub_004C51B8) {
    (void)base;
    Voice3d(kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8));
    kernel::Return(ctx, 0, 2);
}
// any other way into the shared body would write the missing console voice: skip it
X86_FUNC(sub_004C3DFE) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// The library's 3D calculator: 0x4C5D4C (reached directly, and through the 0x4C606D export thunk) works out a
// voice's 3D parameters against the listener, and the listener helpers (0x4C37C1.., reached from 0x4C4613 and
// 0x4C9C86) turn them into mix-bin volumes, filter and pitch. XACT runs them for every 3D sound and hands the result
// to 0x4C3DFE. They are math on the title's own structures, but go through the table of HRTF functions that
// DirectSoundUseFullHRTF (0x4C3976) installs at 0x4E1D1C..0x4E1D44 by calling 0x4C753A, and the runtime's HLE of
// it installs nothing. Install the table before they run.
static void EnsureHrtf(X86Context& ctx) {
    if (!kernel::R32(0x4E1D1C)) kernel::CallGuest(ctx, 0x4C753A, {});
}
#define HRTF_USER(addr)                 \
    X86_EXTERN_FUNC(__imp__sub_##addr); \
    X86_FUNC(sub_##addr) {              \
        EnsureHrtf(ctx);                \
        __imp__sub_##addr(ctx, base);   \
    }
HRTF_USER(004C5D4C)
HRTF_USER(004C37C1)
HRTF_USER(004C37EA)
HRTF_USER(004C3821)
HRTF_USER(004C3841)
HRTF_USER(004C3875)
HRTF_USER(004C3897)
HRTF_USER(004C9C86)

// 0x543170: the library's deferred state flush, which re-pushes hardware state for the dirty flags it finds.
// The runtime translates the D3D8 API directly and decodes the engine's own pushbuffer writes (d3d::Walk), so
// this path only reads guest texture/sampler structures the library would have kept. Skip it and leave the
// dirty bits it did not handle, exactly as its own tail did.
X86_FUNC(sub_00543170) {
    (void)base;
    // what the skipped sub-flushes would have pushed still has to win over the engine's raw writes (fog, the fixed
    // function combiner); the runtime models that from the dirty bits
    d3d::DeferredFlush(kernel::R32(0x5499F0));
    kernel::W32(0x5499F0, kernel::R32(0x5499F0) & 0xC0000070u);
    kernel::Return(ctx, 0, 0);
}

// 0x53AD90: the library applies the fixups queued for push buffers (RunPushBuffer with a D3DFixup): each copies
// bytes into a static push buffer, here jumps that pick one of a model's material blocks per pass. On the console
// the chip has run the earlier jumps into that buffer by then, but the runtime reads the command stream later
// (d3d::Walk), so every run of a model drawn twice in a frame read the last pass's patches and the other pass was
// lost. Read what is queued before the patches land.
X86_EXTERN_FUNC(__imp__sub_0053AD90);
X86_FUNC(sub_0053AD90) {
    d3d::FlushPending();
    __imp__sub_0053AD90(ctx, base);
}

// 0x53AE30: RunPushBuffer(push buffer, fixup), stdcall; the D3DPushBuffer's data at +4. XBE_LOD_TRACE logs who first
// ran each buffer and which stop and start from frame to frame, to tie a model's detail switch (the walk's push
// buffer) to the code that picked it. From the model node renderer (0x3F3510, returning to 0x3F3602) esi is the node.
X86_EXTERN_FUNC(__imp__sub_0053AE30);
X86_FUNC(sub_0053AE30) {
    if (d3d::LodTrace())
        if (uint32_t pb = kernel::R32(ctx.esp + 4))
            d3d::LodTraceRun(ctx, kernel::R32(pb + 4), kernel::R32(ctx.esp) == 0x3F3602 ? ctx.esi : 0);
    __imp__sub_0053AE30(ctx, base);
}

// 0x2D5540 steps a character's timers each frame (edi the character, dl the frame's ticks [0x7FA250]), most by the
// ticks, but the byte [character+0x214] by one a frame (0x2D566C). The animation scripts set it (0x30DA30 sets the
// player's [0x84A5F8] to the command's argument) and branch when it is out (0x2C6A10, the script's jump on +0x214
// zero): the last boss's scripts time the window in which the alien takes Conker's punches with it. The console runs
// at 30 FPS, two ticks a frame, so at 60 the window was half as long, and after the second toss too short to land
// all four punches. It counts console frames here, one each two ticks, as at 30.
// [[hook]] before 0x2D5662, on_true past the decrement (0x2D5674)
bool ConkerScriptTimer(X86Context& ctx, uint8_t* base) {
    uint8_t& left = base[uint32_t(ctx.edi + 0x214)];
    if (left == 0) return true;
    const uint32_t now = display::Ticks(), ticks = ctx.edx & 0xFF;
    const uint32_t frames = now / 2 - (now - ticks) / 2;  // console frames this frame's ticks end
    left = uint8_t(left > frames ? left - frames : 0);
    return true;
}

// 0x2D45D0 (the character, its argument; ebp inside) keeps a swimmer at the surface: after his rise (0x2D46E3 toward
// the surface by [0x7FDB8C] x 8, 0x2D50E4 the velocity [[+0x568]+0x28] times [0x7FDB8C]) the part of him above the
// swimming height, the water's surface [+0x228] less [0x648E38] plus 14, is cut to 0.35 (0x2D5116 takes 0.65) once a
// frame, and the velocity zeroed. Swimming up a shore, the collision (0x2A3110 from 0x2E0640) then lifts him onto the
// bank by what he moved, and once he is above the ground under him he climbs out. The console cut it every two ticks;
// at 60 FPS every tick, so he rode some 4 units lower, the bank met him as a wall and he crept along it at y -106
// (deathwatch captures 104817..104850; an F9's hitch let him out), and he floated 2 units lower in open water.
//
// With R the rise and L the lift a tick, the console's height above the swimming height after the collision, x,
// goes x' = 0.35 (x + 2R) + 2L, settling at 1.08 R + 3.08 L. A frame of one tick here leaves 0.35 E + 0.325 x of the
// height E above it before the cut, x as the frame began: x' = 0.35 (x + R) + 0.325 x + L settles at the same
// 1.08 R + 3.08 L, in open water (L = 0) and up a bank. A plain share of two ticks' cut, 1 - 0.35^(1/2) a tick,
// matched only R: 1.45 L up a bank, still too low where it rises gently (capture 113055, pinned at y -108 while 30
// FPS crept up past it, 112849). Frames of two ticks or more cut it to 0.35^(t/2), the console's at two.
namespace {
uint32_t g_swimmer = 0;   // the character 0x2D45D0 runs for
float g_swimmer_y = 0;    // his y as it began
}  // namespace

X86_EXTERN_FUNC(__imp__sub_002D45D0);
X86_FUNC(sub_002D45D0) {
    const uint32_t who = kernel::R32(ctx.esp + 4);
    g_swimmer = who;
    if (who) std::memcpy(&g_swimmer_y, base + who + 0x7C, 4);
    __imp__sub_002D45D0(ctx, base);
    g_swimmer = 0;
}

// [[hook]] before 0x2D511E: ebp the character, xmm0 his y, xmm1 his height above the swimming height times 0.65
void ConkerSwimSurface(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (display::GameRate() <= 30) return;
    // the frame's ticks, [0x7FA20C]; not the byte [0x7FA250], which goes 0, 1, 0, 1, 1 at 60 (the pull flickered)
    const uint32_t ticks = std::min(kernel::R32(0x7FA20C), 60u);
    const float above = ctx.xmm[1].f[0] / 0.65f;  // E
    float left;
    if (ticks >= 2 || g_swimmer != ctx.ebp) {
        left = above * std::pow(0.35f, float(ticks) * 0.5f);
    } else {
        const float floor_y = ctx.xmm[0].f[0] - above;  // the swimming height
        const float began = std::max(g_swimmer_y - floor_y, 0.0f);
        left = std::min(0.35f * above + 0.325f * began, above);
    }
    ctx.xmm[1].f[0] = above - left;
}

// 0x312F80 (eax the player, each frame from 0x2D87B9) works out how fast a weapon mode's walk really went: the distance
// from the frame's start (+0x84) to where it ended (+0x78), times 2, over the speed the stick asked for (+0x648, 34 a
// full axis), kept to 0..1, times that speed, into [[player+0x568]+0x390]. The 2 fits a frame of two ticks. At 60 FPS a
// frame moves half as far, so the speed read half: 0x3116E0 takes the run clips above 20 and plays a clip at 0.08 x the
// speed, so at full stick Conker walked, slowly, and ran only on a diagonal (each axis has its own 34, up to 47 in all).
// A frame of one tick counts its distance double, as two ticks' worth; longer frames were already past the clamp.
// [[hook]] before 0x312FAB: xmm0 the distance times 2
void ConkerWeaponWalkSpeed(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (display::GameRate() > 30 && kernel::R32(0x7FA20C) == 1) ctx.xmm[0].f[0] *= 2.0f;
}

// The cutscene censor box (0x2A442B, while the byte [0x8496FC] is set): a 100x100 rectangle of the frame pixelated
// in 7x7 blocks (0x1EB210) at the ints [0x849700] x, [0x849704] y, which the scene's command 0x93 (0x59002) sets
// with a velocity in pixels a frame, the ints [0x849708] and [0x84970C], added each drawn frame (0x2A4471) unless
// paused. Each value is in pixels a console frame of two ticks: at 60 FPS it was added twice as often and the box slid
// off the caveman boss as the camera panned (F3 captures 133631, 133632: y 260 to 400 in a second, x still). Above
// 30 a frame adds the velocity times its ticks over two, the odd half pixel carried to the next frame.
// [[hook]] before 0x2A4471: the add of the velocity to the position
void ConkerCensorMove(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    if (display::GameRate() <= 30) return;
    static int32_t last[2] = {}, carry[2] = {};
    const uint32_t ticks = std::clamp(kernel::R32(0x7FA20C), 1u, 60u);
    for (int i = 0; i < 2; i++) {
        const uint32_t at = 0x849700 + 4 * i;
        int32_t pos, vel;
        std::memcpy(&pos, base + at, 4);
        std::memcpy(&vel, base + at + 8, 4);
        if (pos != last[i]) carry[i] = 0;  // placed again by the scene
        const int32_t sum = vel * int32_t(ticks) + carry[i];
        const int32_t step = sum >= 0 ? sum / 2 : -((1 - sum) / 2);  // floor
        carry[i] = sum - 2 * step;
        last[i] = pos + step;
        const int32_t before = last[i] - vel;  // the game adds vel next
        std::memcpy(base + at, &before, 4);
    }
}

// The chocolate pickup ("Choco", object handler 0x707F0, edi the object) spins and bobs on a phase [+0xD8] stepped by
// [0x7FA24C]: its angles are floats, but its height, the word [+0x16], is its first height [+0xDC] plus 10 plus 10 x
// cos(2 x phase) cut to a whole number, so it rose and fell in steps of a unit, every few frames at 60 FPS. The object's
// matrix (0x6AD30) adds the float [+0x1C] to that height, zero for the chocolate (level load, 0x6BA48), so the cut-off
// fraction goes there; the word, which the pickup's reach is measured from, stays as the game made it.
// [[hook]] before 0x708B2: cx the new height, about to go into [+0x16]
void ConkerChocoBob(X86Context& ctx, uint8_t* base) {
    float phase;
    std::memcpy(&phase, base + ctx.edi + 0xD8, 4);
    const int32_t first = int32_t(kernel::R32(ctx.edi + 0xDC));
    const float exact = float(first) + 10.0f + 10.0f * std::cos(2.0f * phase);
    const float frac = exact - float(int16_t(ctx.ecx & 0xFFFF));
    std::memcpy(base + ctx.edi + 0x1C, &frac, 4);
}

// Level object type 29, "Platform Move 3D" (0x6D1A0, the object on the stack): a platform that, once stood on (the
// byte [+0x9F] bit 4), moves from its first place (the int16s [+0xDA] x, [+0xD8] y, [+0xDC] z, kept at its first
// call) by the offsets in [+0x8C] (bytes: x, z, the frames to take, y; each x 64) and back. Its phase, the byte
// [+0xDF], goes 0..255 by 255 / frames a call, the place is the first place plus phase / 255 of the offsets cut to
// whole units, and the signed byte [+0xDE] counts the 100 calls it waits at the far end (then back) and the 100 it
// rests after coming home. All of it once a call, and 0x6A390 calls it once a frame whatever the ticks, so at 60 FPS
// it went twice as fast and waited half as long; the Bats Tower lift rose in 128 frames, 2.1 s, by 6, 7, 6, 7
// (XBE_OBJ_TRACE, 832 units at 2 phase a call). Above 30 the game's call runs once every two ticks, as on the
// console, from where its last one ended; the frames between show the platform part way along that move, its
// fraction of y in the float [+0x1C] that the matrix (0x6AD30) adds. The move is taken from the phase before the
// game's cut (ExactPos): part way along the cut 6, 7 it went 3.5, 3.5, 3, 3, and a generated picture, which blends
// the platform half a frame back but not the shadow's receiver the game builds on it, left the receiver 1.75 behind
// the planks on the long steps and Conker's shadow vanished there (capture 174001). Now 3.26 a frame, evenly. At
// rest at home it's called every frame, so a step onto it starts it at once. Riders are carried by the whole move
// [+0xAC/AE/B0] (0x6A720); the change of the fraction is added to them there.
namespace {
struct Mover {
    int16_t from[3] = {}, to[3] = {}, shown[3] = {};
    float from_at[3] = {}, to_at[3] = {};  // the same before the game cut them to whole units
    uint32_t ticks = 2;   // ticks of the game's last move shown, out of its 2
    float frac = 0;       // the fraction of y in [+0x1C]
    float carry = 0;      // the fraction's change this frame, for riders
    uint32_t frame = 0;   // g_object_frame when it moved
};
std::unordered_map<uint32_t, Mover> g_movers;
uint32_t g_object_frame = 0;  // 0x6A390's runs

void ReadPos(uint32_t o, int16_t* p) {
    for (uint32_t k = 0; k < 3; k++) p[k] = int16_t(kernel::R16(o + 0x14 + 2 * k));
}
void WritePos(uint8_t* base, uint32_t o, const int16_t* p) {
    std::memcpy(base + o + 0x14, p, 6);
}
// The place 0x6D1A0 works out from the phase [+0xDF] before it cuts it to whole units, as it does it: the offsets
// (bytes of [+0x8C]: x, z, -, y; each x 64) times phase x 1/255 ([0x648CEC]) plus the first place.
void ExactPos(uint8_t* base, uint32_t o, const int16_t* fallback, float* at) {
    if (kernel::R32(o + 0xD8) == 0) {
        for (uint32_t k = 0; k < 3; k++) at[k] = float(fallback[k]);
        return;
    }
    const int32_t first[3] = {int16_t(kernel::R16(o + 0xDA)), int16_t(kernel::R16(o + 0xD8)),
                              int16_t(kernel::R16(o + 0xDC))};
    const int32_t offset[3] = {int8_t(base[o + 0x8C]), int8_t(base[o + 0x8F]), int8_t(base[o + 0x8D])};
    float step;
    std::memcpy(&step, base + 0x648CEC, 4);
    const float part = float(base[o + 0xDF]) * step;
    for (uint32_t k = 0; k < 3; k++) at[k] = float(offset[k] * 64) * part + float(first[k]);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0006D1A0);
X86_FUNC(sub_0006D1A0) {
    const uint32_t o = kernel::R32(ctx.esp + 4);
    if (display::GameRate() <= 30) {
        g_movers.erase(o);
        __imp__sub_0006D1A0(ctx, base);
        return;
    }
    const uint32_t esp = ctx.esp;
    // The game's ret pops the return address; put it back for the next call. The handler also uses its argument's
    // slot as scratch ([esp+0x50] = 0 at 0x6D1F8 clears its low byte), so the object goes back in before each call:
    // a second call in one frame (a long frame after an F4 capture) got 00E1AB00 for 00E1AB70 and wrote 0x70 bytes
    // early, its [+0xD8] over the lift's list pointer [+0x68], which 0x68C70 then crashed on.
    auto call = [&] {
        std::memcpy(base + esp + 4, &o, 4);
        __imp__sub_0006D1A0(ctx, base);
        ctx.esp = esp;
    };
    int16_t entry[3];
    ReadPos(o, entry);
    Mover& m = g_movers[o];
    if (kernel::R32(o + 0xD8) == 0 || std::memcmp(entry, m.shown, 6) != 0) {
        // first call, or placed by someone else: start from where it is, the last move done
        m = Mover{};
        std::memcpy(m.from, entry, 6);
        std::memcpy(m.to, entry, 6);
        std::memcpy(m.shown, entry, 6);
        ExactPos(base, o, entry, m.to_at);
        std::memcpy(m.from_at, m.to_at, sizeof m.to_at);
        float f1c;
        std::memcpy(&f1c, base + o + 0x1C, 4);
        m.frac = f1c;
    }
    const float frac_entry = m.frac;
    const uint32_t ticks = std::clamp(kernel::R32(0x7FA20C), 1u, 8u);
    for (uint32_t t = 0; t < ticks; t++) {
        const bool still = std::memcmp(m.from, m.to, 6) == 0 && *(base + o + 0xDE) == 0;
        if (m.ticks >= 2 || still) {
            WritePos(base, o, m.to);
            std::memcpy(m.from, m.to, 6);
            call();
            ReadPos(o, m.to);
            std::memcpy(m.from_at, m.to_at, sizeof m.to_at);
            ExactPos(base, o, m.to, m.to_at);
            m.ticks = 0;
        }
        m.ticks++;
    }
    const float part = float(m.ticks) * 0.5f;
    float at[3];
    int16_t now[3];
    for (uint32_t k = 0; k < 3; k++) {
        at[k] = m.from_at[k] + (m.to_at[k] - m.from_at[k]) * part;
        now[k] = int16_t(k == 1 ? std::floor(at[k]) : std::lround(at[k]));
    }
    WritePos(base, o, now);
    std::memcpy(m.shown, now, 6);
    m.frac = at[1] - float(now[1]);
    std::memcpy(base + o + 0x1C, &m.frac, 4);
    m.carry = m.frac - frac_entry;
    m.frame = g_object_frame;
    for (uint32_t k = 0; k < 3; k++) {
        const int16_t move = int16_t(now[k] - entry[k]);
        std::memcpy(base + o + 0xAC + 2 * k, &move, 2);
    }
    std::memcpy(base + esp + 4, &o, 4);
    ctx.esp = esp + 4;  // the return address, as the game's ret pops it
}

// 0x6A720 carries a rider by a level object's move (eax the object's number, ebx the rider's y): the fraction a
// "Platform Move 3D" platform moved in [+0x1C] goes with it.
X86_EXTERN_FUNC(__imp__sub_0006A720);
X86_FUNC(sub_0006A720) {
    const uint32_t o = kernel::R32(0x8471B8) + ctx.eax * 0x150;
    const uint32_t y_at = ctx.ebx;
    __imp__sub_0006A720(ctx, base);
    if (g_movers.empty()) return;
    const auto it = g_movers.find(o);
    if (it == g_movers.end() || it->second.carry == 0 || it->second.frame != g_object_frame) return;
    float y;
    std::memcpy(&y, base + y_at, 4);
    y += it->second.carry;
    std::memcpy(base + y_at, &y, 4);
}

// 0x6A390 runs every level object's handler ([obj+0x88]) once a frame: the array [0x8471B8], [0x76FD10] of them,
// 0x150 apiece, the position int16s at +0x14/16/18, the float [+0x1C] added to y by the matrix (0x6AD30), the frame's
// move at +0xAC/AE/B0 (what riders are carried by). XBE_OBJ_TRACE=1 logs each object whose position, angles or scale changed in a
// frame, with its type's name from the table 0x58C1F0 (0x24 apiece: handler, ..., name pointer at +0x10), to tell
// which type moves a platform.
X86_EXTERN_FUNC(__imp__sub_0006A390);
X86_FUNC(sub_0006A390) {
    g_object_frame++;
    static const bool trace = std::getenv("XBE_OBJ_TRACE") != nullptr;
    if (!trace) {
        __imp__sub_0006A390(ctx, base);
        return;
    }
    static std::vector<uint8_t> before;
    static uint32_t frame = 0;
    frame++;
    const uint32_t objs = kernel::R32(0x8471B8);
    const uint32_t count = std::min(kernel::R32(0x76FD10), 4096u);
    before.resize(size_t(count) * 0x150);
    // changes since the last frame's handlers ran: the cutscene player (0x52B50, track kind 3) places objects, angles
    // and scale too, outside this loop
    static std::vector<uint8_t> after;
    static uint32_t after_objs = 0;
    const bool cutscene = base[0x76EC82] == 1 || base[0x76F0E0] == 1;
    if (cutscene)
        LOGR("objtrace: frame %u ticks %u cutscene clock %d end %d", frame, kernel::R32(0x7FA20C),
             int32_t(kernel::R32(0x76F0F8)), int32_t(kernel::R32(0x76F0E8)));
    if (after_objs == objs && after.size() == size_t(count) * 0x150) {
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* was = after.data() + size_t(i) * 0x150;
            const uint8_t* now = base + objs + i * 0x150;
            if (std::memcmp(was, now, 0x20) == 0 && std::memcmp(was + 0x7C, now + 0x7C, 0xC) == 0) continue;
            float a[6], m[3];
            std::memcpy(a, now + 4, 12);
            std::memcpy(a + 3, was + 4, 12);
            std::memcpy(m, now + 0x20 + 0x30, 12);  // the matrix's translation (0x6AD30 builds it at +0x20)
            LOGR("objtrace: frame %u outside obj %u pos %d %d %d ang %.3f %.3f %.3f (was %.3f %.3f %.3f) mtx t %.2f %.2f %.2f",
                 frame, i, int(int16_t(kernel::R16(objs + i * 0x150 + 0x14))),
                 int(int16_t(kernel::R16(objs + i * 0x150 + 0x16))), int(int16_t(kernel::R16(objs + i * 0x150 + 0x18))),
                 double(a[0]), double(a[1]), double(a[2]), double(a[3]), double(a[4]), double(a[5]), double(m[0]),
                 double(m[1]), double(m[2]));
        }
    }
    if (count) std::memcpy(before.data(), base + objs, size_t(count) * 0x150);
    __imp__sub_0006A390(ctx, base);
    after_objs = objs;
    after.assign(base + objs, base + objs + size_t(count) * 0x150);
    auto type_name = [&](uint32_t handler, uint32_t& index) -> const char* {
        for (index = 1; index < 90; index++) {
            const uint32_t e = 0x58C1F0 + index * 0x24;
            if (kernel::R32(e) == handler && kernel::R32(e + 0x10)) return reinterpret_cast<const char*>(base + kernel::R32(e + 0x10));
        }
        index = 0;
        return "?";
    };
    auto f32 = [&](const uint8_t* p) {
        float f;
        std::memcpy(&f, p, 4);
        return double(f);
    };
    auto s16 = [&](const uint8_t* p) {
        int16_t v;
        std::memcpy(&v, p, 2);
        return int(v);
    };
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t o = objs + i * 0x150;
        const uint8_t* was = before.data() + size_t(i) * 0x150;
        const uint8_t* now = base + o;
        // the angles [+4..+0xC] (radians x [0x648E18], 0x6AC90), the place +0x14..+0x1C and the scale +0x7C..+0x84
        if (std::memcmp(was, now, 0x20) == 0 && std::memcmp(was + 0x7C, now + 0x7C, 0xC) == 0) continue;
        const uint32_t handler = kernel::R32(o + 0x88);
        uint32_t index;
        const char* name = type_name(handler, index);
        LOGR("objtrace: frame %u ticks %u obj %u (%08X) type %u '%s' handler %06X pos %d %d %d step %d %d %d move %d %d %d "
             "f1c %.3f ang %.3f %.3f %.3f (was %.3f %.3f %.3f) scale %.3f %.3f %.3f p8c %08X d8 %08X dc %08X e0 %08X",
             frame, kernel::R32(0x7FA20C), i, o, index, name, handler, s16(now + 0x14), s16(now + 0x16), s16(now + 0x18),
             s16(now + 0x14) - s16(was + 0x14), s16(now + 0x16) - s16(was + 0x16), s16(now + 0x18) - s16(was + 0x18),
             s16(now + 0xAC), s16(now + 0xAE), s16(now + 0xB0), f32(now + 0x1C), f32(now + 4), f32(now + 8),
             f32(now + 0xC), f32(was + 4), f32(was + 8), f32(was + 0xC), f32(now + 0x7C), f32(now + 0x80),
             f32(now + 0x84), kernel::R32(o + 0x8C), kernel::R32(o + 0xD8), kernel::R32(o + 0xDC), kernel::R32(o + 0xE0));
    }
}

// 0x2EBAD0 (esi the character) moves an airborne character up and down in fixed substeps: as many as the time scale
// [0x7FDB8C] (the slow motion factor times ticks / 2) times 10, truncated (0x2EBB32), each taking [+0xD4] x 0.1 off the
// vertical speed [+0xD0] and adding the speed x 0.05 to y [+0x7C]. Heist's slow motion dive across the bank hall
// runs at a factor of 0.2: the console's frame, two ticks, has the scale 0x3E4CCC00 (0.1999998), one substep. At 60 a
// frame of one tick has 0x3DCCCC00 (0.0999999), no substep, so Conker hung at his take-off height through the slow
// part (deathwatch captures 133807, 133825) and rose and fell all at once when the dive sped up again, and flew on
// into the wall. Out of slow motion it's 10 a console frame, 5 a tick, so the loss was only in slow motion and the
// ramps. Above 30 each console frame gets the console's count (the scale of two ticks, times 10, truncated). An even
// count is split evenly over its ticks. An odd one (the dive's 1) would leave a tick without a substep, and Conker shook
// a pixel up and down each frame, so it runs as twice as many half substeps instead, a tick each: half the speed's
// loss, half the speed into y. A pair of those adds a quarter of a substep's own drop, 0.25 x 0.05 x [+0xD4] x 0.1,
// more to y than the console's one; y takes that off first, so two ticks end where the console's frame does.
// [[hook]] before 0x2EBB36: eax the substeps, xmm3 the speed's loss a substep, xmm4 0.05 (the speed's share into y)
void ConkerAirSubsteps(X86Context& ctx, uint8_t* base) {
    if (display::GameRate() <= 30) return;
    const uint32_t ticks = std::clamp(kernel::R32(0x7FA20C), 1u, 60u);
    float scale;
    std::memcpy(&scale, base + 0x7FDB8C, 4);
    const int32_t per = int32_t(scale * 2.0f / float(ticks) * 10.0f);  // a console frame's substeps (x2 exact at 1 tick)
    if (per <= 0) {
        ctx.eax = 0;
        return;
    }
    if (per % 2 == 0) {
        ctx.eax = uint32_t(per / 2) * ticks;
        return;
    }
    const uint32_t halves = uint32_t(per) * ticks;
    const uint32_t at = ctx.esi + 0x7C;
    float y;
    std::memcpy(&y, base + at, 4);
    y -= float(halves) * 0.125f * ctx.xmm[4].f[0] * ctx.xmm[3].f[0];
    std::memcpy(base + at, &y, 4);
    ctx.xmm[3].f[0] *= 0.5f;
    ctx.xmm[4].f[0] *= 0.5f;
    ctx.eax = halves;
}

// 0x2D9FD0 (edi the player): eases the signed word [player+0x1D4] toward [player+0x1D6] once a frame, by a twelfth
// of the gap and one more, never past it. The target comes from the ground Conker stands on (0x2D5340: the material
// [player+0x298] & 0x1F in the table at 0x59D820, against [player+0x110]): on BarnOutside's thatched roof it drops
// to about -172 on landing and climbs back to 0 by some 6 a 60th of a second. Right after the ease 0x2E34D0 kills
// the player outright, health [player+0x3AE] to 0, once the value is under -93. The console eased it 30 times a
// second, and against that climbing target it bottomed out near -72; at 60 FPS, eased twice as often, it chased the
// target down to -94 or -95 (deathwatch captures 135519, 135938), and Conker dropped dead on the roof. Above 30 it
// eases once a console frame (two ticks), as on the console.
X86_EXTERN_FUNC(__imp__sub_002D9FD0);
X86_FUNC(sub_002D9FD0) {
    static const bool every = std::getenv("XBE_EASE_EVERY_FRAME") != nullptr;
    if (every || display::GameRate() <= 30) {
        __imp__sub_002D9FD0(ctx, base);
        return;
    }
    const uint32_t now = display::Ticks(), ticks = base[0x7FA250];
    uint32_t frames = now / 2 - (now - std::min(now, ticks)) / 2;  // console frames this frame's ticks end
    int16_t cur, target;
    std::memcpy(&cur, base + ctx.edi + 0x1D4, 2);
    std::memcpy(&target, base + ctx.edi + 0x1D6, 2);
    for (; frames && cur != target; frames--) {
        const int32_t step = (int32_t(target) - int32_t(cur)) / 12;  // the function's divide, toward zero
        const int16_t next = int16_t(cur < target ? cur + step + 1 : cur + step - 1);
        cur = cur < target ? std::min(next, target) : std::max(next, target);
    }
    std::memcpy(base + ctx.edi + 0x1D4, &cur, 2);
    kernel::Return(ctx, ctx.eax, 0);
}

// 0x1A510 (emitter, flag; stdcall): a sound emitter's state machine, run once a frame for each (0x2D440). It counts
// two timers down by the frame's step ([0x7FA218]): +0xE8 to its next cue, which plays the cue (0x1A4C0) when it
// runs out, and +0x168 to its stop. XACT starts a 3D cue's stream once the cue is played and positioned, so the cue
// is heard from the frame its timer runs out. At 30 FPS the timers run out on a 33 ms grid: Harbour's battery, four
// emitters a few tens of ms apart playing the one cannonade, starts in two pairs of sample aligned copies, one wide
// volley. At 60 the grid is 16.7 ms and the four start apart, copies of one wave that comb into a hollow, metallic
// volley. Above 30 the timers move only on a console frame (two ticks), by the steps since the last one, so emitters
// start on the console's grid. XBE_EMITTER_EVERY_FRAME counts them every frame again.
X86_EXTERN_FUNC(__imp__sub_0001A510);
X86_FUNC(sub_0001A510) {
    static const bool every = std::getenv("XBE_EMITTER_EVERY_FRAME") != nullptr;
    if (every || display::GameRate() <= 30) {
        __imp__sub_0001A510(ctx, base);
        return;
    }
    using kernel::R32;
    auto rf = [&](uint32_t a) {
        float v;
        std::memcpy(&v, base + a, 4);
        return v;
    };
    auto wf = [&](uint32_t a, float v) { std::memcpy(base + a, &v, 4); };
    // the frame's place on the console's grid: the game thread runs every emitter in a frame, so one frame's state
    // serves them all
    static uint32_t frame = ~0u, half = ~0u;
    static float pending = 0, apply = 0;
    static bool boundary = true;
    const uint32_t now = display::Ticks();
    const float step = rf(0x7FA218);
    if (now != frame) {
        frame = now;
        boundary = now / 2 != half;
        if (boundary) {
            half = now / 2;
            apply = pending;  // the steps of the frames between, which this frame's count takes as well
            pending = 0;
        } else {
            pending += step;
            apply = 0;
        }
    }
    const uint32_t e = R32(ctx.esp + 4);
    // the function takes the step off each timer it counts: off a frame between console frames, give it back first;
    // on one, take the frames between off as well
    const float add = boundary ? -apply : step;
    if (rf(e + 0xE8) > 0) wf(e + 0xE8, std::max(rf(e + 0xE8) + add, boundary ? 0.0f : step));
    if (base[e + 5] && rf(e + 0x168) > 0) wf(e + 0x168, std::max(rf(e + 0x168) + add, boundary ? 0.0f : step));
    __imp__sub_0001A510(ctx, base);
}

// 0x2AD3B0, the loader's progress step between the files it loads, puts the front buffer up as the target
// (0x2ACA70), draws the loading screen into it (0x2ACB20, stdcall, the screen), puts the old target back, which shows
// it (d3d8.cpp ShowFront), and waits for the GPU. Each shown was a Present the loader waited on, ~45 ms, and most of a
// load; drawn but unshown, the thousands of a load cost the GPU seconds in the first frame after. The loading is quick
// here, so with loading::g_fade no screen is drawn: d3d::LoadingScreenWanted fades to black instead, the frames after
// fading back in. Off: every screen drawn and shown, as on the console.
bool loading::g_fade = true;

X86_EXTERN_FUNC(__imp__sub_002ACB20);
X86_FUNC(sub_002ACB20) {
    loadtrace::Scope trace(loadtrace::kScreen);
    if (loading::g_fade && !d3d::LoadingScreenWanted()) {
        kernel::Return(ctx, ctx.eax, 1);
        return;
    }
    __imp__sub_002ACB20(ctx, base);
}

// 0x352320 (esi the profile, [0x862CD0]) works out the multiplayer cheats the player has earned: Potty Mouth from the
// campaign, Chapter X on every difficulty or the class medals (byte +0x1439), Flame Fire's gold bullets from the
// MOD#YellowBullets stat, 1,500 kills online (+0x143B), and Purple Streak from MOD#PurpleBullets, 3,000 (+0x143D).
// Each sits after its on/off byte (+0x1438, +0x143A, +0x143C), which is left to the player. With no Xbox Live stats to
// earn them from, all three are unlocked here.
X86_EXTERN_FUNC(__imp__sub_00352320);
X86_FUNC(sub_00352320) {
    const uint32_t profile = ctx.esi;
    __imp__sub_00352320(ctx, base);
    base[profile + 0x1439] = 1;
    base[profile + 0x143B] = 1;
    base[profile + 0x143D] = 1;
}

// XBE_TRACE_FILTER: where XACT's DLS2 cutoffs come from (a muffled music stream in BrontoThroat asked for 100 Hz).
// 0x551846 (esi an XACT track, [esi+4] its voice) builds the track's DSFILTERDESC: cutoff in Hz = [esi+0x22] (set by
// the sequence's events, 0x554E61, maybe ramped by 0x55526D) + [esi+0x2A] (runtime parameter curves, 0x5515FB),
// clamped to 0..8192, Hz through 0x550B94 (4096 log2(2 sin(pi Hz / 48000)), 0 above 8000); resonance [esi+0x24] +
// [esi+0x2C]. 0x55152C (eax the curve's variable mapping, edx the variable's value) maps a variable through a curve
// of up to five points. 0x54BE03 (esi a ramp, when called from the list at 0x54BD99) steps a variable ramp: step [esi+0x1C] of [esi+0x26], value step *
// [esi+0x28] + [esi+0x2C], name [esi+0x18]. Each logs when what it saw changes.
namespace {
bool TraceFilter() {
    static const bool on = std::getenv("XBE_TRACE_FILTER") != nullptr;
    return on;
}
std::mutex g_filter_mu;
float GuestF32(const uint8_t* base, uint32_t va) {
    float f;
    std::memcpy(&f, base + va, 4);
    return f;
}
// a guest pointer whose next 32 bytes are committed (XACT's sounds live high in user space, at 0x7FFAxxxx)
bool GuestRam(uint32_t va) { return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + 31); }
std::string GuestName(const uint8_t* base, uint32_t va) {
    if (!GuestRam(va)) return "-";
    std::string s;
    for (uint32_t i = 0; i < 16 && base[va + i]; i++)
        s += base[va + i] >= 32 && base[va + i] < 127 ? char(base[va + i]) : '?';
    return s;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00551846);
X86_FUNC(sub_00551846) {
    if (TraceFilter() && GuestRam(ctx.esi)) {
        const uint32_t t = ctx.esi;
        int16_t w[8];
        std::memcpy(w, base + t + 0x20, sizeof w);
        std::string key(reinterpret_cast<const char*>(w), sizeof w);
        std::lock_guard<std::mutex> lock(g_filter_mu);
        static std::unordered_map<uint32_t, std::string> seen;
        if (seen[t] != key) {
            seen[t] = key;
            const uint32_t from = kernel::R32(ctx.esp);
            // edx the sound: its curves' variables, 0x18 bytes each from [sound+0x14], [sound+0x18] of them, the
            // name at +8 and the number of curves at +3
            std::string rpcs;
            const uint32_t snd = ctx.edx, table = GuestRam(snd) ? kernel::R32(snd + 0x14) : 0;
            const uint32_t n = table ? std::min<uint32_t>(kernel::R32(snd + 0x18), 8) : 0;
            for (uint32_t i = 0; i < n && GuestRam(table + i * 0x18); i++)
                rpcs += " '" + GuestName(base, table + i * 0x18 + 8) + "'x" + std::to_string(base[table + i * 0x18 + 3]);
            // a sequence event (0x554E61, its instance at the caller's [ebp+0xC], its data at [instance+0x14]): the
            // data's first 16 bytes, type at +0, flags at +5, count +6, values +8/+0xA, duration +0xC
            std::string ev;
            if (from == 0x554E9D) {
                const uint32_t inst = kernel::R32(ctx.ebp + 0xC), data = GuestRam(inst) ? kernel::R32(inst + 0x14) : 0;
                if (GuestRam(data)) {
                    char b[96];
                    int k = std::snprintf(b, sizeof b, " event %08X step %u:", data, kernel::R16(inst + 0xA));
                    for (int i = 0; i < 16 && k < int(sizeof b) - 4; i++)
                        k += std::snprintf(b + k, sizeof b - k, " %02X", base[data + i]);
                    ev = b;
                }
            }
            LOGR("xactfilter: track %08X voice %08X sound %08X from %08X arg %u cutoff %d+%d res %d+%d (vol %d pitch "
                 "%d, +20..2E %d %d %d %d %d %d %d %d) curves%s%s",
                 t, kernel::R32(t + 4), snd, from, kernel::R32(ctx.esp + 4), w[1], w[5], w[2], w[6], w[0],
                 int16_t(kernel::R16(t + 0x1E)), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                 rpcs.empty() ? " none" : rpcs.c_str(), ev.c_str());
        }
    }
    __imp__sub_00551846(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0055152C);
X86_FUNC(sub_0055152C) {
    if (TraceFilter() && GuestRam(ctx.eax) && GuestRam(kernel::R32(ctx.esp + 4))) {
        const uint32_t m = ctx.eax, curve = kernel::R32(ctx.esp + 4);
        int16_t pts[10];
        std::memcpy(pts, base + curve, sizeof pts);
        char b[256];
        std::snprintf(b, sizeof b, "map %08X value %u (x %g + %g) curve %08X: kind %u target %u, %u=%d %u=%d %u=%d %u=%d",
                      m, ctx.edx, GuestF32(base, m + 8), GuestF32(base, m + 0xC), curve, uint16_t(pts[0]),
                      uint16_t(pts[1]), uint16_t(pts[2]), pts[3], uint16_t(pts[4]), pts[5], uint16_t(pts[6]), pts[7],
                      uint16_t(pts[8]), pts[9]);
        std::lock_guard<std::mutex> lock(g_filter_mu);
        static std::unordered_map<uint32_t, std::string> seen;
        if (seen[curve] != b) {
            seen[curve] = b;
            LOGR("xactrpc: %s from %08X track index %u", b, kernel::R32(ctx.esp), kernel::R32(ctx.esp + 8));
        }
    }
    __imp__sub_0055152C(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0054BE03);
X86_FUNC(sub_0054BE03) {
    // only the ramp list's stepper (0x54BD99); 0x54C6B7 calls it with something else in esi
    if (TraceFilter() && kernel::R32(ctx.esp) == 0x54BD9E && GuestRam(ctx.esi)) {
        const uint32_t r = ctx.esi, step = kernel::R32(r + 0x1C), count = kernel::R16(r + 0x26);
        if (step == 0 || step >= count) {
            std::lock_guard<std::mutex> lock(g_filter_mu);
            LOGR("xactramp: %08X '%s' kind %u cue %08X step %u of %u: %g a step from %g (now %g)", r,
                 GuestName(base, kernel::R32(r + 0x18)).c_str(), kernel::R16(r + 0x20), kernel::R32(r + 0x30), step,
                 count, GuestF32(base, r + 0x28), GuestF32(base, r + 0x2C),
                 float(step) * GuestF32(base, r + 0x28) + GuestF32(base, r + 0x2C));
        }
    }
    __imp__sub_0054BE03(ctx, base);
}

// 0x55144B (esi the sound, args: the variable, its value): a cue's variable set (0x55041E, from a sequence's ramp at
// 0x54BE6A or the title); the name at [variable+0x14], logged when the value changes
X86_EXTERN_FUNC(__imp__sub_0055144B);
X86_FUNC(sub_0055144B) {
    const uint32_t var = kernel::R32(ctx.esp + 4);
    if (TraceFilter() && GuestRam(var)) {
        const std::string name = GuestName(base, kernel::R32(var + 0x14));
        const uint32_t value = kernel::R32(ctx.esp + 8);
        std::lock_guard<std::mutex> lock(g_filter_mu);
        static std::unordered_map<std::string, uint32_t> seen;
        const std::string key = std::to_string(ctx.esi) + name;
        auto it = seen.find(key);
        if (it == seen.end() || it->second != value) {
            seen[key] = value;
            LOGR("xactvar: sound %08X '%s' = %u (%08X, default %08X) from %08X", ctx.esi, name.c_str(), value, value,
                 kernel::R32(var + 0xC), kernel::R32(ctx.esp));
        }
    }
    __imp__sub_0055144B(ctx, base);
}
