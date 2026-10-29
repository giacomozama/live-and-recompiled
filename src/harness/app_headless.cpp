// the host side without a window: settings at their defaults, the run folder as the exe folder
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "app/app.h"

namespace app {

Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
void Save() {}
void ApplyDisplay() {}
Mode Desktop() { return {1280, 720}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
float Aspect() { return 16.0f / 9.0f; }
bool Widescreen() { return true; }
float FrameAspect() { return kGame.aspect ? kGame.aspect(Aspect()) : 16.0f / 9.0f; }

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = std::filesystem::current_path();
    return dir;
}
int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}
int PortArg() {
    static const int n = [] {
        const char* e = std::getenv("PORT");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}
const std::string& JoinArg() {
    static const std::string s = std::getenv("JOIN") ? std::getenv("JOIN") : "";
    return s;
}
void* Window() { return nullptr; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace(uint32_t) {}

// no keyboard, window or display: the pad script drives port 0
bool KeyDown(int) { return false; }
void Post(uint32_t) {}
void SetTitle(const std::wstring&) {}
bool Focused() { return false; }
bool Background() { return false; }
void HoldMouse(bool) {}
bool Alert(const std::wstring&, bool) { return false; }
std::vector<Mode> DisplayModes() { return {Desktop()}; }

}  // namespace app
