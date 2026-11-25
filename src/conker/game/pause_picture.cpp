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
// over it (the 3D with the title's own passes, no HUD; gpu::Backend::PausePicture), and each paused frame writes
// that back into the back buffer and skips the rest of the frame's drawing for the pause menu's call, which shrinks
// it as before. Single player only (a match's pause is its own), and only with a renderer that keeps pictures
// (Vulkan); otherwise the game draws as it did. XBE_PAUSE_PICTURE=0 leaves it drawing, for an A/B.
#include <cstdint>

#include "common/warp.h"
#include "core/diag.h"
#include "cpu/x86_ops.h"
#include "xdk/d3d8.h"

namespace {
constexpr uint32_t kPauseAsked = 0x7FA274, kPaused = 0x7FA275;
bool g_kept = false;  // the frame before the pause asked for its picture
}  // namespace

// [[hook]] at 0x2A3FFF in 0x2A3FD0, the frame's drawing, past its prologue: true jumps to 0x2A4497, the pause
// menu's call (0x2A60B0), stepping over the cameras' shadows and scenes, the HUD branch and the frame's passes
bool ConkerPauseFrame(X86Context& ctx, uint8_t* base) {
    (void)ctx;
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
        d3d::PausePicture(false);
        return true;
    }
    g_kept = base[kPauseAsked] != 0;
    if (g_kept) d3d::PausePicture(true);
    return false;
}
