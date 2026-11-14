// The first launch's welcome screen (GameInfo::welcome): before the title boots, so what it picks (the game's language,
// which the title reads once as it boots) is in place for it. The renderer is made here instead of at the title's
// CreateDevice, draws the screen over black at the window's own size, and is then the one the device takes over
// (gpu::g_early).
#include <atomic>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"

namespace app {

namespace {
enum State { kOff, kUp, kDone };  // kDone: the screen is gone, black until the title shows a picture
std::atomic<int> g_state{kOff};
}  // namespace

bool Welcoming() { return g_state.load() != kOff; }

void DrawWelcome() {
    if (g_state.load() == kUp && kGame.welcome()) g_state.store(kDone);
}

bool RunWelcome() {
    if (!FirstRun() || !kGame.welcome) return true;
    bool ok = false;
    auto backend = gpu::CreateBackend(640, 480, ok);  // the title's device sets its own size (SetGuestSize)
    if (!ok) return true;                              // nothing to draw it with: the title boots as ever
    LOGR("First launch: welcome screen");
    g_state.store(kUp);
    while (g_state.load() == kUp) {
        if (!Pump()) return false;  // the window closed: no settings file, so it shows again next launch
        backend->Redisplay(false);
        PaceMenu();
    }
    backend->Redisplay(false);  // black, until the title's first picture
    g_state.store(kOff);
    Save();
    gpu::g_early = std::move(backend);
    return true;
}

}  // namespace app
