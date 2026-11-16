#pragma once

#include <cstdint>

// Frame rate and aspect patches (src/conker/game/display.cpp), after the community widescreen and 60 FPS XBE patches.
namespace display {

// settings.toml frame_rate (0 the highest: the most of these the monitor's refresh rate allows), the pictures shown a second: 30 is the console's (present every second blank), 60 the
// patched game's limit; 120, 180 and 240 run the game at 60 and draw the frames between again (the runtime's frame
// generation). XBE_FRAME_RATE overrides it. The headless harness keeps 30 unless asked, so its frame-counted pad
// scripts hold.
extern uint32_t g_frame_rate;
// the frame rate set, the runtime's fps cap and frame generation with it: the one limit (GameInfo::init applies the
// loaded one)
void SetFrameRate(uint32_t rate);
// the frames the game itself runs a second: 30 or 60
uint32_t GameRate();
// the 60 Hz ticks the game has stepped since boot, this frame's among them (the frame timer adds them as it hands them
// out): for counting in console frames, two ticks each, what the game counts in frames
uint32_t Ticks();
void Init();

// GameInfo::aspect: the camera patch draws any shape, so the frame is the display's; XBE_ASPECT=W:H overrides it,
// and the headless harness keeps the console's 4:3 unless asked
float Aspect(float display);

// the height in degrees the last camera projected sees ([camera+0x264], 50 unzoomed): a sniper's scope narrows it.
// The game's own, before g_fov scales it
float CameraFov();

// settings.toml fov: the view's height in degrees in play, the console's 50 (kFov) scaling every camera's (a scope's
// zoom keeps its share of it); menus and cutscenes keep the console's
constexpr uint32_t kFov = 50, kFovMin = 40, kFovMax = 100;
extern uint32_t g_fov;

// settings.toml look_up: degrees the third person camera may tilt up past the console's stop (0x47DC0 holds the near
// camera's pitch to -45..-2, -2 about level, the far one's to -30..-5), so the view looks further up from below the
// player; 0 is the console's
constexpr uint32_t kLookUpMax = 60;
extern uint32_t g_look_up;

// settings.toml hud_split: a multiplayer match's HUD drawn unstretched at a display wider than 4:3, each element kept
// to its edge of the display (display.cpp, HUD); off, it stretches with the frame as on a widescreen console. Read
// each frame
extern bool g_hud_split;

// settings.toml player_tags: what a match shows over the other players, the game's tag (0x347330): their names
// (kTagNames, as on the console), a marker in place of the name (kTagMarkers: a triangle pointing down at them, green
// for a teammate, red for an opponent, the game's own colours made solid), or no name (kTagOff). The tag's icons and a
// waiting player's name with the time to their respawn stay. Read as each tag draws (player_tags.cpp)
enum : uint32_t { kTagNames = 0, kTagMarkers = 1, kTagOff = 2 };
extern uint32_t g_player_tags;
// from the hook before 0x3D3981 (menus.cpp ConkerTextQuad), a glyph's quad about to be sent: while a marker draws, its
// one glyph made the marker's triangle. Game thread
void PlayerMarkerQuad();

// The camera the game's draws go through ([0x84B868], its view at +0x10) for the runtime's motion blur
// (gpu::ViewConstants::camera): its view in the vertex constants kCameraRegister to kCameraRegister + 2, a row for each
// of view space's axes, and the frame (d3d::Presents) in kCameraRegister + 3, which no program of the game reads (the
// lowest it reads directly is c-38). Called as each model is drawn (tags.cpp); sets them when the camera or the frame
// changed, only with app::Settings::motion_blur on. Game thread
inline constexpr uint32_t kCameraRegister = 8;  // hardware order: D3D c-88 to c-85
void SendCamera();

}  // namespace display
