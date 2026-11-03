// Mods: folders in ConfigDir/mods, each laid out like the game's disc (D:), whose files the game reads in place of
// its own. Nothing on disk is replaced: the kernel's file layer looks in the mods first (kernel/files.cpp). The
// settings keep their order and which are in use; a change starts from the next launch.
#include <algorithm>
#include <system_error>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace app {

namespace {

namespace fs = std::filesystem;

std::vector<std::string> g_mounted;  // the mods this launch mounted, in order

std::vector<std::string> InUse() {
    std::vector<std::string> names;
    for (const ModEntry& m : Get().mods)
        if (m.enabled) names.push_back(m.name);
    return names;
}

}  // namespace

fs::path ModsDir() {
    static const fs::path d = [] {
        fs::path dir = ConfigDir() / "mods";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }();
    return d;
}

void ScanMods() {
    std::vector<std::string> found;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(ModsDir(), ec))
        if (e.is_directory(ec)) found.push_back(e.path().filename().string());
    std::sort(found.begin(), found.end());
    std::vector<ModEntry>& mods = Get().mods;
    auto has = [&](const std::string& name) { return std::find(found.begin(), found.end(), name) != found.end(); };
    mods.erase(std::remove_if(mods.begin(), mods.end(), [&](const ModEntry& m) { return !has(m.name); }), mods.end());
    for (const std::string& name : found)
        if (std::none_of(mods.begin(), mods.end(), [&](const ModEntry& m) { return m.name == name; }))
            mods.push_back({name, true});
}

void MountMods() {
    const std::vector<ModEntry> before = Get().mods;
    ScanMods();
    if (Get().mods != before) SaveChanged();  // new folders join the list in the file
    g_mounted = InUse();
    std::vector<fs::path> dirs;
    for (const std::string& name : g_mounted) dirs.push_back(ModsDir() / name);
    if (!dirs.empty()) LOGR("Mods: %zu in use from %s", dirs.size(), ModsDir().string().c_str());
    kernel::MountMods(dirs);
}

bool ModsChanged() { return InUse() != g_mounted; }

}  // namespace app
