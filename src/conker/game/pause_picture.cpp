// The pause screen's picture of the game. Paused, the game kept drawing its whole frame under the pause screen each
// frame (0x2A3FD0: the shadows, the level, its glow and depth of field passes, captures 005953/005955), only for the
// pause menu (0x204060) to shrink the back buffer into a 256x256 texture (0x3EAF20 at 0x2043BC), draw that grey in
// the frame (0x3570F0) and cover the rest with its cloth. The game being paused, each of those frames was the same
// picture, and not quite the one played: the grass patches multiply by the camera's copy of the projection
// (0x1A9540), which there held the console's 4:3 aspect while the rest kept the widened one, so the grass moved to
// one side as the pause came up.
//
// The pause is a request and its taking: the pause key sets [0x7FA274] (0x1F77C0, the menu's 0x203B50), and
// 0x2A4610 copies it into [0x7FA275] after the frame's Swap (XBE_STICK_WATCH_AT=7FA274,7FA275). So the frame drawn
// with the request set and the pause not yet taken is the last played one: it keeps its picture as the 2D comes
// over it (the 3D with the title's own passes, no HUD; gpu::Backend::PausePicture), and each paused frame skips the
// cameras' shadows and scenes for the last camera loop, then writes the picture into the back buffer before the
// pause menu's call, which shrinks it as before. That loop stays: 0x2A5F30 sets up the camera the pause screen's
// frame and cloth are drawn with (0x202AC0, 60 degrees, from the level camera's view); without it they took the
// 55 degree camera 0x202250 left (capture 014612), larger against the picture's quad, the frame's dark glass showing
// past the picture at its sides. Single player only (a match's pause is its own), and only with a renderer that
// keeps pictures (Vulkan); otherwise the game draws as it did. XBE_PAUSE_PICTURE=0 leaves it drawing, for an A/B.
#include <cstdint>
#include <utility>

#include "common/warp.h"
#include "core/diag.h"
#include "cpu/x86_ops.h"
#include "xdk/d3d8.h"

namespace {
constexpr uint32_t kPauseAsked = 0x7FA274, kPaused = 0x7FA275;
bool g_kept = false;  // the frame before the pause asked for its picture
bool g_put = false;   // this paused frame stepped over its scenes: the picture goes back before the pause menu
}  // namespace

// [[hook]] at 0x2A3FFF in 0x2A3FD0, the frame's drawing, past its prologue: true jumps to 0x2A435C, the last camera
// loop (0x2A5F30 each), stepping over the cameras' shadows and scenes and the frame's passes; ebx is its count's
// start, 0 there (0x2A4260)
bool ConkerPauseFrame(X86Context& ctx, uint8_t* base) {
    static const bool off = [] {
        const char* e = diag::Env("XBE_PAUSE_PICTURE");
        return e && e[0] == '0';
    }();
    if (off || warp::Frontend() || warp::Multiplayer() || !d3d::PausePictures()) {
        g_kept = false;
        return false;
    }
    if (base[kPaused]) {
        if (!g_kept) return false;  // a pause whose request wasn't seen drawn: the game draws its own
        g_put = true;
        ctx.ebx = 0;
        return true;
    }
    g_kept = base[kPauseAsked] != 0;
    if (g_kept) d3d::PausePicture(true);
    return false;
}

// [[hook]] at 0x2A4497 in 0x2A3FD0, its call to the pause menu (0x2A60B0): the kept picture into the back buffer
// over what the camera loop drew, for the pause menu to shrink
void ConkerPausePicture(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (!std::exchange(g_put, false)) return;
    d3d::PausePicture(false);
}
