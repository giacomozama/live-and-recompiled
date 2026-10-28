// Where the game keeps what it writes. Windows: settings and the hard disk in %APPDATA%, logs, crash dumps, captures
// and the shader records in %LOCALAPPDATA%. Elsewhere the XDG folders: $XDG_CONFIG_HOME, $XDG_DATA_HOME,
// $XDG_STATE_HOME and $XDG_CACHE_HOME, each defaulting under $HOME as the spec says. A portable.txt beside the exe
// keeps everything in the exe's folder, as before.
#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr const wchar_t* kFolder = L"LiveAndRecompiled";

#ifdef _WIN32
fs::path Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir;
}
#else
// $var when it is an absolute path (the spec ignores a relative one), else $HOME/fallback
fs::path Xdg(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    if (v && *v == '/') return v;
    const char* home = std::getenv("HOME");
    return home && *home ? fs::path(home) / fallback : fs::path();
}
#endif

// the folder for kind, made; the exe's own when portable, or when the platform has none
fs::path Dir(int kind) {
    static const bool portable = [] {
        std::error_code ec;
        return fs::exists(ExeDir() / "portable.txt", ec);
    }();
    if (portable) return ExeDir();
#ifdef _WIN32
    fs::path base = Known(kind < 2 ? FOLDERID_RoamingAppData : FOLDERID_LocalAppData);
#else
    static const char* const kVars[][2] = {{"XDG_CONFIG_HOME", ".config"},
                                           {"XDG_DATA_HOME", ".local/share"},
                                           {"XDG_STATE_HOME", ".local/state"},
                                           {"XDG_CACHE_HOME", ".cache"}};
    fs::path base = Xdg(kVars[kind][0], kVars[kind][1]);
#endif
    if (base.empty()) return ExeDir();
    fs::path dir = base / kFolder;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return ec ? ExeDir() : dir;
}

// from beside the exe to its new place, when only the old one is there: moved, or copied where it cannot be
void Move(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (from == to || !fs::exists(from, ec) || fs::exists(to, ec)) return;
    fs::rename(from, to, ec);
    if (!ec) {
        LOGR("Moved %s to %s", from.string().c_str(), to.string().c_str());
        return;
    }
    // through a temporary name, so a copy cut short is tried again next run
    fs::path tmp = to;
    tmp += ".part";
    fs::remove_all(tmp, ec);
    fs::copy(from, tmp, fs::copy_options::recursive, ec);
    if (!ec) fs::rename(tmp, to, ec);
    if (ec) LOGW("paths: copying %s to %s: %s", from.string().c_str(), to.string().c_str(), ec.message().c_str());
    else LOGR("Copied %s to %s, the old copy left in place", from.string().c_str(), to.string().c_str());
}

}  // namespace

fs::path ConfigDir() {
    static const fs::path d = Dir(0);
    return d;
}
fs::path DataDir() {
    static const fs::path d = Dir(1);
    return d;
}
fs::path StateDir() {
    static const fs::path d = Dir(2);
    return d;
}
fs::path CacheDir() {
    static const fs::path d = Dir(3);
    return d;
}

void MoveOldData() {
    const fs::path exe = ExeDir();
    Move(exe / kGame.settings, ConfigDir() / kGame.settings);
    Move(exe / "hdd", DataDir() / "hdd");
    for (int i = 2; i <= 8; i++) Move(exe / ("hdd" + std::to_string(i)), DataDir() / ("hdd" + std::to_string(i)));
    Move(exe / "shaders.rec", CacheDir() / "shaders.rec");
    Move(exe / "shader_programs.bin", CacheDir() / "shader_programs.bin");
}

}  // namespace app
