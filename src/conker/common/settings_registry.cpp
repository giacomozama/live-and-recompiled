// the settings registry (app::Setting): every setting of the settings file that is one value, in the settings menu's
// order. A new setting is a row here, its storage declared without a value: the default is the row's. Each row says
// what the welcome screen's presets (common/presets.h) make it, in order Nostalgic, Modern, Enhanced: {} leaves it as it is
// in that one, kNo in all three
#include "common/settings_registry.h"

#include "common/achievements.h"
#include "common/characters.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/grass.h"
#include "common/hud_art.h"
#include "common/lighting.h"
#include "common/link.h"
#include "common/loading.h"
#include "common/medbeam.h"
#include "common/menus.h"
#include "common/presets.h"
#include "common/prompts.h"
#include "common/reflection.h"
#include "common/rope.h"
#include "common/subtitles.h"
#include "common/ui.h"
#include "common/vignette.h"
#include "common/water.h"

namespace settings {

std::span<const app::Setting> Registry() {
    using app::DisplayMode, app::Fog, app::Setting;
    using app::kPaperWhiteMin, app::kPaperWhiteMax, app::kPeakMin, app::kPeakMax;
    static_assert(app::kPresets == presets::kCount);
#if !defined(XBE_HEADLESS)
    constexpr int kDefaultRate = 60;
#else
    constexpr int kDefaultRate = 30;
#endif
    static const app::Presets kNo = app::Presets::None();
    app::Settings& s = app::Get();
    static const Setting kSettings[] = {
        // display
        {"display_mode", &s.display_mode, DisplayMode::Borderless, kNo},
        {"width", &s.width, 0, kNo},
        {"height", &s.height, 0, kNo},
        {"aspect", &s.aspect, "auto", {"4:3", {}, {}}},
        {"gpu", &s.gpu, "", kNo},
        {"renderer", &s.renderer, "vulkan", kNo},

        // tone and calibration
        {"hdr", &s.hdr, false, kNo},
        Setting{"hdr_paper_white", &s.hdr_paper_white, 0, kNo}.Range(kPaperWhiteMin, kPaperWhiteMax).OrZero(),
        Setting{"hdr_highlights", &s.hdr_highlights, 50, kNo}.Range(0, 100),
        Setting{"hdr_peak", &s.hdr_peak, 0, kNo}.Range(kPeakMin, kPeakMax).OrZero(),
        Setting{"gamma", &s.gamma, 220, kNo}.Range(180, 260),
        Setting{"brightness", &s.brightness, 0, kNo}.Range(-20, 20),

        // frame rate. frame_rate: 30, 60, 120, 180 or 240; 0, the old Highest, moves to a rate (display.cpp), and as a
        // preset's value picks the monitor's. It sets fps_cap and the runtime's frame_generation
        Setting{"frame_rate", &display::g_frame_rate, kDefaultRate, {30, 60, 0}}
            .ApplyBy([](int64_t rate) { display::SetFrameRate(rate ? uint32_t(rate) : display::HighestFrameRate()); }),
        {"fps_cap", &s.fps_cap, 60, kNo},
        {"vsync", &s.vsync, true, {true, true, true}},
        {"picture_timing_exact", &s.picture_timing_exact, false, kNo},
        {"adaptive_interp", &s.adaptive_interp, true, kNo},
        {"fps_overlay", &ui::g_fps_overlay, false, kNo},

        // upscaling and sharpening
        Setting{"render_percent", &s.render_percent, 100, {100, 100, 100}}.Range(25, 200),
        {"upscaler", &s.upscaler, "off", kNo},
        Setting{"upscaler_quality", &s.upscaler_quality, 1, kNo}.Range(0, 5),
        Setting{"upscaler_percent", &s.upscaler_percent, 33, kNo}.Range(10, 100),
        Setting{"dlss_network", &s.dlss_network, 0, kNo}.Range(0, 2),
        Setting{"dlss_sharpness", &s.dlss_sharpness, 0, kNo}.Range(0, 100),
        {"fsr", &s.fsr, false, {false, false, true}},
        Setting{"fsr_sharpness", &s.fsr_sharpness, 80, {80, 80, 80}}.Range(0, 100),
        Setting{"fur_sharpness", &s.fur_sharpness, 0, {0, 0, 0}}.Range(0, 100),

        // quality and post processing
        {"anisotropic_filtering", &s.aniso, 0, {0, 16, 16}},
        {"msaa", &s.msaa, 1, {1, 4, 4}},
        {"smaa", &s.smaa, false, {false, false, false}},
        {"vignette", &vignette::g_on, true, {true, true, true}},  // the game's darkened screen edges (vignette.cpp)
        Setting{"film_grain", &s.film_grain, 0, {0, 0, 0}}.Range(0, 100),
        Setting{"chromatic_aberration", &s.chromatic_aberration, 0, {0, 0, 0}}.Range(0, 100),
        Setting{"ambient_occlusion", &s.ambient_occlusion, 0, {0, 0, 100}}.Range(0, 100),
        Setting{"ambient_occlusion_radius", &s.ambient_occlusion_radius, 30, {50, 50, 50}}.Range(5, 200),
        Setting{"motion_blur", &s.motion_blur, 0, {0, 0, 0}}.Range(0, 100),

        // lighting
        {"per_pixel_lighting", &lighting::g_per_pixel, true, {false, false, true}},  // lit per pixel (lighting.cpp)
        Setting{"rim_light", &lighting::g_rim, 0, kNo}.Range(0, 100),  // edges lit against the background, per pixel lighting
        {"smooth_lightmaps", &s.smooth_lightmaps, true, {false, false, true}},
        Setting{"lightmap_smoothing", &s.lightmap_smoothing, 5, {5, 5, 5}}.Range(1, 10),
        {"lightmap_size", &s.lightmap_size, 512, {512, 512, 1024}},
        {"fog", &s.fog, Fog::Enabled, {Fog::Enabled, Fog::Enabled, Fog::Enabled}},

        // fur and detail
        {"fur_strands", &fur::g_strands, false, {false, false, true}},  // super fur: procedural strands; each model's look in [fur_models] (fur.cpp)
        {"fur_shading", &fur::g_shading, false, {false, false, false}},  // lit, soft edged fur shells, never with strands (fur.cpp)
        {"fur_distant", &fur::g_distant, true, {false, true, true}},  // distant fur keeps all its shells (hooks.cpp)
        {"dlss_fur_auto", &s.dlss_fur_auto, true, kNo},
        {"character_max_lod", &characters::g_max_lod, true, {false, true, true}},  // characters keep their full model (hooks.cpp)
        {"character_smooth_animation", &characters::g_smooth_animation, true, {false, true, true}},  // far players animate every frame (hooks.cpp)
        {"character_menu_models", &characters::g_menu_models, false, {false, false, false}},  // the classes' character select models (hooks.cpp)
        {"grass_wind", &grass::g_wind, true, {false, true, true}},  // distant grass sways too (hooks.cpp)
        {"grass_max_lod", &grass::g_max_lod, true, {false, true, true}},  // distant grass keeps all its blades (hooks.cpp)
        {"grass_fade", &grass::g_fade, false, {true, false, false}},  // distant grass fades out, else solid however far (hooks.cpp)
        {"dynamic_water", &water::g_dynamic, false, {false, true, true}},  // flowing, rippling water (water.cpp)
        {"floor_depth", &reflection::g_floor_depth, true, {false, true, true}},  // floors over reflections blur by their own depth (reflection.cpp)
        {"redrawn_hud", &hud_art::g_on, true, kNo},  // HUD art drawn from vector pictures (hud_art.cpp)
        {"shader_warmup", &s.shader_warmup, true, kNo},

        // tweaks
        Setting{"fov", &display::g_fov, display::kFov, kNo}.Range(display::kFovMin, display::kFovMax),  // the view's height in degrees in play, 50 the console's
        Setting{"look_up", &display::g_look_up, 0, kNo}.Range(0, display::kLookUpMax),  // degrees the third person camera tilts up past the console's
        {"skip_intro_fmvs", &menus::g_skip_intro, false, kNo},  // the launch movies skipped (menus.cpp, hooks.cpp)
        {"attract_video", &menus::g_attract, true, kNo},  // the idle menu's attract video (menus.cpp)
        {"keyboard_prompts", &prompts::g_on, true, kNo},  // button icons drawn as keys on kb+mouse (prompts.cpp)
        {"skippable_cutscenes", &cutscenes::g_skippable, true, kNo},  // LT skips any cutscene (hooks.cpp)
        {"english_bubbles", &cutscenes::g_english_bubbles, true, {false, true, true}},  // the throne monologues' bubbles in English (hooks.cpp)
        {"speech_subtitles", &subtitles::g_on, false, kNo},  // speech bubbles shown as subtitles instead (subtitles.cpp)
        {"redrawn_bubbles", &subtitles::g_redraw, true, {false, true, true}},  // speech bubbles drawn again by the runtime, never with subtitles (bubbles.cpp)
        {"fade_loads", &loading::g_fade, true, kNo},  // a fade to black instead of loading screens (hooks.cpp)
        {"rope_physics", &rope::g_physics, true, {false, true, true}},  // ropes swing as a weighted chain (rope.cpp)
        {"achievements", &achievements::g_enabled, true, kNo},  // achievements are earned, locally (achievements.cpp)

        // multiplayer
        {"join_ip", &s.join_ip, "", kNo},
        {"system_link_port", &s.system_link_port, 3074, kNo},
        {"hud_split", &display::g_hud_split, true, {false, true, true}},  // a match's HUD unstretched, at the display's edges (display.cpp)
        Setting{"player_tags", &display::g_player_tags, display::kTagNames, kNo}.Range(0, display::kTagOff),  // over other players: 0 names, 1 markers, 2 off (player_tags.cpp)
        {"player_ranks", &display::g_player_ranks, true, kNo},  // the rank emblem in the tag over other players
        Setting{"health_bars", &display::g_health_bars, display::kBarsBeside, kNo}.Range(0, display::kBarsOff),  // other players' health bars: 0 beside them, 1 instead of the name or marker, 2 none
        {"player_outlines", &display::g_player_outlines, false, kNo},  // other players outlined in a match
        Setting{"team_colors", &display::g_team_colors, display::kTeamGreenRed, kNo}.Range(0, display::kTeamBlueOrange),  // a match's team colours: 0 green/red, 1 blue/orange
        {"medgun_beam", &medbeam::g_enabled, true, kNo},  // the medgun's heal beam in a match (medbeam.cpp)
        {"sharp_drone_view", &display::g_sharp_drone, true, kNo},  // the Sneeker drone's view sharp, no haze (display.cpp)
        {"no_friendly_fire", &syslink::g_no_friendly_fire, false, kNo},  // a match's teammates don't hurt each other; online the host's (friendly_fire.cpp)
        {"bright_menu_hints", &menus::g_bright_hints, true, {false, true, true}},  // solid page hints (menus.cpp)

        // audio
        Setting{"volume", &s.volume, 100, kNo}.Range(0, 100),
        {"audio_device", &s.audio_device, "", kNo},
        {"surround", &s.surround, false, kNo},
        {"mute_in_background", &s.mute_in_background, false, kNo},
        {"menu_sounds", &s.menu_sounds, true, kNo},
        {"achievement_sound", &s.achievement_sound, true, kNo},

        // audio: the Mix section
        Setting{"stereo_full_backs", &s.stereo_full_backs, false, kNo}.Mix(),
        Setting{"stereo_sub", &s.stereo_sub, false, kNo}.Mix(),
        Setting{"reverb", &s.reverb, 100, kNo}.Range(0, 200).Mix(),
        Setting{"crosstalk", &s.crosstalk, true, kNo}.Mix(),
        Setting{"doppler", &s.doppler, true, kNo}.Mix(),
        Setting{"distance_filters", &s.distance_filters, true, kNo}.Mix(),
        Setting{"resampler", &s.resampler, 0, kNo}.Range(0, 2).Mix(),
        Setting{"voice_headroom", &s.voice_headroom, true, kNo}.Mix(),
        Setting{"soft_clip", &s.soft_clip, false, kNo}.Mix(),
        Setting{"envelopes", &s.envelopes, true, kNo}.Mix(),
        Setting{"lfo", &s.lfo, true, kNo}.Mix(),
        Setting{"voice_filters", &s.voice_filters, true, kNo}.Mix(),

        // language, and the settings menu
        {"language", &s.language, "english", kNo},
        {"ui_language", &s.ui_language, "english", kNo},
        Setting{"ui_accent", &ui::g_accent, 4, kNo}.Range(0, 6),  // the settings menu's accent color, 0 blue to 6 purple, 4 orange (ui_font.cpp)
        {"menu_decorations", &ui::g_decorations, true, kNo},  // outlines round the settings menu's sections and widgets (ui.cpp)
        {"menu_resizable", &ui::g_menu_resizable, false, kNo},  // Debug: the settings menu moved and sized by the mouse (ui.cpp)
        {"fast_forward", &display::g_fast_forward_enabled, false, kNo},  // Debug: F10 and the Debug tab fast forward the game (display.cpp)
    };
    return kSettings;
}

}  // namespace settings
