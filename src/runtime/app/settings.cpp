// the settings file in ConfigDir
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
const char* kFogModes[] = {"enabled", "disabled", "tint_only"};  // Fog
// a player's devices: [player1] to [player4], keyboard, mouse and controller (input.h), the game adding its own
const char* kSlotKeys[input::kKinds] = {"keyboard", "mouse", "controller"};

std::filesystem::path File() { return ConfigDir() / kGame.settings_file; }

// one setting from the file, kept as it is when the file lacks it
void Read(const toml::table& t, const Setting& setting) {
    std::visit(
        [&](auto* to) {
            using T = std::remove_pointer_t<decltype(to)>;
            if constexpr (std::is_same_v<T, DisplayMode>) {
                const std::string mode = t[setting.key].value_or(std::string());
                for (uint32_t i = 0; i < 3; i++)
                    if (mode == kModes[i]) *to = DisplayMode(i);
            } else if constexpr (std::is_same_v<T, Fog>) {
                // fog was a bool before the tint (and fog_tint a bool beside it for a while)
                if (std::optional<bool> fog = t[setting.key].value<bool>()) {
                    *to = *fog ? Fog::Enabled : t["fog_tint"].value_or(false) ? Fog::TintOnly : Fog::Disabled;
                } else if (std::optional<std::string> fog = t[setting.key].value<std::string>()) {
                    for (uint32_t i = 0; i < 3; i++)
                        if (*fog == kFogModes[i]) *to = Fog(i);
                }
            } else if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int32_t>) {
                const T v = t[setting.key].value_or(*to);
                if (setting.min == setting.max || (setting.zero_ok && v == 0)) *to = v;
                else *to = T(std::clamp<int64_t>(v, setting.min, setting.max));
            } else {
                *to = t[setting.key].value_or(*to);
            }
        },
        setting.storage);
}

void Write(toml::table& t, const Setting& setting) {
    std::visit(
        [&](auto* at) {
            using T = std::remove_pointer_t<decltype(at)>;
            if constexpr (std::is_same_v<T, DisplayMode>) t.insert(setting.key, kModes[uint32_t(*at)]);
            else if constexpr (std::is_same_v<T, Fog>) t.insert(setting.key, kFogModes[uint32_t(*at)]);
            else if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int32_t>) t.insert(setting.key, int64_t(*at));
            else t.insert(setting.key, *at);
        },
        setting.storage);
}

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void ToggleWindowed() {
    static DisplayMode last = DisplayMode::Borderless;  // what windowed returns to
    Settings& s = Get();
    if (s.display_mode == DisplayMode::Windowed) {
        s.display_mode = last;
    } else {
        last = s.display_mode;
        s.display_mode = DisplayMode::Windowed;
    }
    ApplyDisplay();
}

// the text the file last had from Load or Save, for SaveChanged
static std::string g_written;
static bool g_first_run = false;

bool FirstRun() { return g_first_run; }

void Load() {
    Settings& s = Get();
    ResetSettings();
    std::error_code ec;
    g_first_run = !std::filesystem::exists(File(), ec);
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    for (const Setting& setting : kGame.settings()) Read(t, setting);
    if (s.smaa) s.msaa = 1;  // SMAA or MSAA, never both
    if (s.lightmap_size < 256 || s.lightmap_size > 2048 || !std::has_single_bit(s.lightmap_size))
        SettingAt(&s.lightmap_size).Set(SettingAt(&s.lightmap_size).def);
    if (!t["ui_language"].value<std::string>()) s.ui_language = s.language;
    if (const toml::array* mods = t["mods"].as_array()) {
        s.mods.clear();
        for (const toml::node& n : *mods)
            if (const toml::table* m = n.as_table())
                if (std::optional<std::string> name = (*m)["name"].value<std::string>())
                    s.mods.push_back({*name, (*m)["enabled"].value_or(true)});
    }
    input::SetAutoAssign(t["auto_controllers"].value_or(input::AutoAssign()));
    for (int p = 0; p < input::kPorts; p++) {
        const toml::table* player = t["player" + std::to_string(p + 1)].as_table();
        if (!player) continue;
        for (int k = 0; k < input::kKinds; k++)
            if (std::optional<std::string> key = (*player)[kSlotKeys[k]].value<std::string>())
                input::Assign(p, input::Kind(k), *key);
    }
    if (kGame.load) kGame.load(&t);
    g_written = SettingsText();  // what this file holds, as far as the settings go
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"auto_controllers", input::AutoAssign()}};
    for (const Setting& setting : kGame.settings()) Write(t, setting);
    toml::array mods;
    for (const ModEntry& m : s.mods) mods.push_back(toml::table{{"name", m.name}, {"enabled", m.enabled}});
    t.insert("mods", std::move(mods));
    for (int p = 0; p < input::kPorts; p++) {
        toml::table player;
        for (int k = 0; k < input::kKinds; k++) player.insert(kSlotKeys[k], input::Assigned(p, input::Kind(k)));
        t.insert("player" + std::to_string(p + 1), std::move(player));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

std::string SettingsText() {
    std::ostringstream text;
    text << Table() << "\n";
    return text.str();
}

void Save() {
    g_written = SettingsText();
    std::ofstream f(File());
    f << g_written;
}

bool SaveChanged() {
    if (SettingsText() == g_written) return false;
    Save();
    return true;
}

static std::atomic<uint32_t> g_display_paper{0}, g_display_peak{0};

void SetDisplayHdr(uint32_t paper_white, uint32_t peak) {
    g_display_paper.store(paper_white, std::memory_order_relaxed);
    g_display_peak.store(peak, std::memory_order_relaxed);
}
uint32_t DisplayHdrPaperWhite() { return g_display_paper.load(std::memory_order_relaxed); }
uint32_t DisplayHdrPeak() { return g_display_peak.load(std::memory_order_relaxed); }

uint32_t HdrPaperWhite() {
    if (uint32_t v = Get().hdr_paper_white) return v;
    uint32_t d = DisplayHdrPaperWhite();
    return d ? std::clamp(d, kPaperWhiteMin, kPaperWhiteMax) : 200;
}

uint32_t HdrPeak() {
    if (uint32_t v = Get().hdr_peak) return v;
    uint32_t d = DisplayHdrPeak();
    return d ? std::clamp(d, kPeakMin, kPeakMax) : 1000;
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

float Aspect() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<float> output{16.0f / 9.0f};
    const Settings& s = Get();
    float w = 0, h = 0;
    if (std::sscanf(s.aspect.c_str(), "%f:%f", &w, &h) == 2 && w > 0 && h > 0) return std::clamp(w / h, 0.5f, 4.0f);
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        if (m.width && m.height) output = std::clamp(float(m.width) / float(m.height), 0.5f, 4.0f);
    }
    return output;
}

// the aspect setting picks the TV the console reports; the title reads it once at boot
bool Widescreen() { return Aspect() >= 1.5f - 1e-4f; }  // 3:2 or wider

float FrameAspect() {
    if (kGame.aspect) return std::clamp(kGame.aspect(Aspect()), 0.5f, 4.0f);
    return Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
}

Scale RenderScale(uint32_t guest_width, uint32_t guest_height) {
    Mode m = Output();
    if (!guest_width || !guest_height || !m.width || !m.height) return {1, 1};
    // the frame as gpu's DrawFrame shows it: the output at FrameAspect, letterboxed or pillarboxed
    const float aspect = FrameAspect();
    const float w = std::min(float(m.width), float(m.height) * aspect);
    const float h = w / aspect;
    const float k = Get().render_percent / 100.0f;
    return {std::max(w * k / guest_width, 0.1f), std::max(h * k / guest_height, 0.1f)};
}

}  // namespace app
