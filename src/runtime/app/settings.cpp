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
static bool g_first_run = false;

bool FirstRun() { return g_first_run; }

void Load() {
    Settings& s = Get();
    std::error_code ec;
    g_first_run = !std::filesystem::exists(File(), ec);
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
    s.render_percent = std::min<uint32_t>(t["render_percent"].value_or(s.render_percent), 200);
    s.upscaler = t["upscaler"].value_or(s.upscaler);
    s.upscaler_quality = std::min<uint32_t>(t["upscaler_quality"].value_or(s.upscaler_quality), 4);
    s.dlss_model = std::min<uint32_t>(t["dlss_model"].value_or(s.dlss_model), 3);
    s.dlss_sharpness = std::min<uint32_t>(t["dlss_sharpness"].value_or(s.dlss_sharpness), 100);
    s.dlss_fur_auto = t["dlss_fur_auto"].value_or(s.dlss_fur_auto);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.picture_timing_exact = t["picture_timing_exact"].value_or(s.picture_timing_exact);
    s.hdr = t["hdr"].value_or(s.hdr);
    // 0 the display's
    if (uint32_t v = t["hdr_paper_white"].value_or(s.hdr_paper_white))
        s.hdr_paper_white = std::clamp(v, kPaperWhiteMin, kPaperWhiteMax);
    s.hdr_highlights = std::min<uint32_t>(t["hdr_highlights"].value_or(s.hdr_highlights), 100);
    if (uint32_t v = t["hdr_peak"].value_or(s.hdr_peak)) s.hdr_peak = std::clamp(v, kPeakMin, kPeakMax);
    s.gamma = std::clamp<uint32_t>(t["gamma"].value_or(s.gamma), 180, 260);
    s.brightness = std::clamp<int32_t>(t["brightness"].value_or(s.brightness), -20, 20);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.msaa = t["msaa"].value_or(s.msaa);
    s.smaa = t["smaa"].value_or(s.smaa);
    if (s.smaa) s.msaa = 1;  // SMAA or MSAA, never both
    s.film_grain = t["film_grain"].value_or(s.film_grain);
    s.chromatic_aberration = t["chromatic_aberration"].value_or(s.chromatic_aberration);
    s.ambient_occlusion = t["ambient_occlusion"].value_or(s.ambient_occlusion);
    s.ambient_occlusion_radius = t["ambient_occlusion_radius"].value_or(s.ambient_occlusion_radius);
    s.motion_blur = std::min<uint32_t>(t["motion_blur"].value_or(s.motion_blur), 100);
    // fog was a bool before the tint (and fog_tint a bool beside it for a while)
    if (std::optional<bool> fog = t["fog"].value<bool>()) {
        s.fog = *fog ? Fog::Enabled : t["fog_tint"].value_or(false) ? Fog::TintOnly : Fog::Disabled;
    } else if (std::optional<std::string> fog = t["fog"].value<std::string>()) {
        for (uint32_t i = 0; i < 3; i++)
            if (*fog == kFogModes[i]) s.fog = Fog(i);
    }
    s.fsr = t["fsr"].value_or(s.fsr);
    s.smooth_lightmaps = t["smooth_lightmaps"].value_or(s.smooth_lightmaps);
    s.lightmap_smoothing = std::clamp<uint32_t>(t["lightmap_smoothing"].value_or(s.lightmap_smoothing), 1, 10);
    s.lightmap_size = t["lightmap_size"].value_or(s.lightmap_size);
    if (s.lightmap_size < 256 || s.lightmap_size > 2048 || !std::has_single_bit(s.lightmap_size)) s.lightmap_size = 512;
    s.fsr_sharpness = std::min<uint32_t>(t["fsr_sharpness"].value_or(s.fsr_sharpness), 100);
    s.fur_sharpness = std::min<uint32_t>(t["fur_sharpness"].value_or(s.fur_sharpness), 100);
    s.volume = std::min<uint32_t>(t["volume"].value_or(s.volume), 100);
    s.audio_device = t["audio_device"].value_or(s.audio_device);
    s.menu_sounds = t["menu_sounds"].value_or(s.menu_sounds);
    s.achievement_sound = t["achievement_sound"].value_or(s.achievement_sound);
    s.mute_in_background = t["mute_in_background"].value_or(s.mute_in_background);
    s.surround = t["surround"].value_or(s.surround);
    s.stereo_full_backs = t["stereo_full_backs"].value_or(s.stereo_full_backs);
    s.stereo_sub = t["stereo_sub"].value_or(s.stereo_sub);
    s.reverb = std::min<uint32_t>(t["reverb"].value_or(s.reverb), 200);
    s.crosstalk = t["crosstalk"].value_or(s.crosstalk);
    s.doppler = t["doppler"].value_or(s.doppler);
    s.distance_filters = t["distance_filters"].value_or(s.distance_filters);
    s.resampler = std::min<uint32_t>(t["resampler"].value_or(s.resampler), 2);
    s.voice_headroom = t["voice_headroom"].value_or(s.voice_headroom);
    s.soft_clip = t["soft_clip"].value_or(s.soft_clip);
    s.envelopes = t["envelopes"].value_or(s.envelopes);
    s.lfo = t["lfo"].value_or(s.lfo);
    s.voice_filters = t["voice_filters"].value_or(s.voice_filters);
    s.shader_warmup = t["shader_warmup"].value_or(s.shader_warmup);
    s.gpu = t["gpu"].value_or(s.gpu);
    s.renderer = t["renderer"].value_or(s.renderer);
    s.aspect = t["aspect"].value_or(s.aspect);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.system_link_port = t["system_link_port"].value_or(s.system_link_port);
    s.language = t["language"].value_or(s.language);
    s.ui_language = t["ui_language"].value_or(s.language);
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
                  {"upscaler", s.upscaler},
                  {"upscaler_quality", int64_t(s.upscaler_quality)},
                  {"dlss_sharpness", int64_t(s.dlss_sharpness)},
                  {"dlss_model", int64_t(s.dlss_model)},
                  {"dlss_fur_auto", s.dlss_fur_auto},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"picture_timing_exact", s.picture_timing_exact},
                  {"hdr", s.hdr},
                  {"hdr_paper_white", int64_t(s.hdr_paper_white)},
                  {"hdr_highlights", int64_t(s.hdr_highlights)},
                  {"hdr_peak", int64_t(s.hdr_peak)},
                  {"gamma", int64_t(s.gamma)},
                  {"brightness", int64_t(s.brightness)},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"msaa", int64_t(s.msaa)},
                  {"smaa", s.smaa},
                  {"film_grain", int64_t(s.film_grain)},
                  {"chromatic_aberration", int64_t(s.chromatic_aberration)},
                  {"ambient_occlusion", int64_t(s.ambient_occlusion)},
                  {"ambient_occlusion_radius", int64_t(s.ambient_occlusion_radius)},
                  {"motion_blur", int64_t(s.motion_blur)},
                  {"fog", kFogModes[uint32_t(s.fog)]},
                  {"fsr", s.fsr},
                  {"smooth_lightmaps", s.smooth_lightmaps},
                  {"lightmap_smoothing", int64_t(s.lightmap_smoothing)},
                  {"lightmap_size", int64_t(s.lightmap_size)},
                  {"fsr_sharpness", int64_t(s.fsr_sharpness)},
                  {"fur_sharpness", int64_t(s.fur_sharpness)},
                  {"volume", int64_t(s.volume)},
                  {"audio_device", s.audio_device},
                  {"menu_sounds", s.menu_sounds},
                  {"achievement_sound", s.achievement_sound},
                  {"mute_in_background", s.mute_in_background},
                  {"surround", s.surround},
                  {"stereo_full_backs", s.stereo_full_backs},
                  {"stereo_sub", s.stereo_sub},
                  {"reverb", s.reverb},
                  {"crosstalk", s.crosstalk},
                  {"doppler", s.doppler},
                  {"distance_filters", s.distance_filters},
                  {"resampler", s.resampler},
                  {"voice_headroom", s.voice_headroom},
                  {"soft_clip", s.soft_clip},
                  {"envelopes", s.envelopes},
                  {"lfo", s.lfo},
                  {"voice_filters", s.voice_filters},
                  {"shader_warmup", s.shader_warmup},
                  {"gpu", s.gpu},
                  {"renderer", s.renderer},
                  {"aspect", s.aspect},
                  {"join_ip", s.join_ip},
                  {"system_link_port", int64_t(s.system_link_port)},
                  {"language", s.language},
                  {"ui_language", s.ui_language},
                  {"auto_controllers", input::AutoAssign()}};
    toml::array mods;
    for (const ModEntry& m : s.mods) mods.push_back(toml::table{{"name", m.name}, {"enabled", m.enabled}});
    t.insert("mods", std::move(mods));
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
