// the settings file in ConfigDir
#include <algorithm>
#include <atomic>
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
// a player's devices: [player1] to [player4], keyboard, mouse and controller (input.h), the game adding its own
const char* kSlotKeys[input::kKinds] = {"keyboard", "mouse", "controller"};

std::filesystem::path File() { return ConfigDir() / kGame.settings; }

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

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.msaa = t["msaa"].value_or(s.msaa);
    s.smaa = t["smaa"].value_or(s.smaa);
    s.film_grain = t["film_grain"].value_or(s.film_grain);
    s.chromatic_aberration = t["chromatic_aberration"].value_or(s.chromatic_aberration);
    s.ambient_occlusion = t["ambient_occlusion"].value_or(s.ambient_occlusion);
    s.ambient_occlusion_radius = t["ambient_occlusion_radius"].value_or(s.ambient_occlusion_radius);
    s.volume = std::min<uint32_t>(t["volume"].value_or(s.volume), 100);
    s.mute_in_background = t["mute_in_background"].value_or(s.mute_in_background);
    s.shader_warmup = t["shader_warmup"].value_or(s.shader_warmup);
    s.renderer = t["renderer"].value_or(s.renderer);
    s.aspect = t["aspect"].value_or(s.aspect);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    input::SetAutoAssign(t["auto_controllers"].value_or(input::AutoAssign()));
    for (int p = 0; p < input::kPorts; p++) {
        const toml::table* player = t["player" + std::to_string(p + 1)].as_table();
        if (!player) continue;
        for (int k = 0; k < input::kKinds; k++)
            if (std::optional<std::string> key = (*player)[kSlotKeys[k]].value<std::string>())
                input::Assign(p, input::Kind(k), *key);
    }
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    g_written = SettingsText();  // what this file holds, as far as the settings go
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"msaa", int64_t(s.msaa)},
                  {"smaa", s.smaa},
                  {"film_grain", int64_t(s.film_grain)},
                  {"chromatic_aberration", int64_t(s.chromatic_aberration)},
                  {"ambient_occlusion", int64_t(s.ambient_occlusion)},
                  {"ambient_occlusion_radius", int64_t(s.ambient_occlusion_radius)},
                  {"volume", int64_t(s.volume)},
                  {"mute_in_background", s.mute_in_background},
                  {"shader_warmup", s.shader_warmup},
                  {"renderer", s.renderer},
                  {"aspect", s.aspect},
                  {"join_ip", s.join_ip},
                  {"auto_controllers", input::AutoAssign()}};
    for (int p = 0; p < input::kPorts; p++) {
        toml::table player;
        for (int k = 0; k < input::kKinds; k++) player.insert(kSlotKeys[k], input::Assigned(p, input::Kind(k)));
        t.insert("player" + std::to_string(p + 1), std::move(player));
    }
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
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

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
