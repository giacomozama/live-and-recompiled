#pragma once

#include <atomic>
#include <cstdint>

// Frame rate and aspect patches (src/conker/game/display.cpp), after the community widescreen and 60 FPS XBE patches.
namespace display {

// settings.toml frame_rate, the pictures shown a second: 30 is the console's (present every second blank), 60 the
// patched game's limit; 120, 180 and 240 run the game at 60 and draw the frames between again (the runtime's frame
// generation). XBE_FRAME_RATE overrides it. The headless harness keeps 30 unless asked, so its frame-counted pad
// scripts hold.
extern uint32_t g_frame_rate;
// the frame rate set, the runtime's fps cap and frame generation with it: the one limit (GameInfo::init applies the
// loaded one)
void SetFrameRate(uint32_t rate);
// the highest frame rate offered: the first of 60, 120, 180 and 240 to reach the monitor's refresh (a rate just under,
// as 59.94 for 60, counts), past which more pictures only cost; 240 past 240 Hz. A setting above it runs at it
uint32_t MostFrameRate();
// the most of 30, 60, 120, 180 and 240 the monitor shows (a rate just under, as 59.94 for 60, counts), up to
// MostFrameRate: 120 at 165 Hz. The Enhanced preset's, and what an old "Highest" setting (0) becomes
uint32_t HighestFrameRate();
// in a system link session with another machine: the frame rate setting can't change, and the game's rate stays as it
// was when the other came in (peers must tick alike, link.cpp). Any thread
bool FrameRateLocked();
// the frames the game itself runs a second: 30 or 60
uint32_t GameRate();
// the 60 Hz ticks the game has stepped since boot, this frame's among them (the frame timer adds them as it hands them
// out): for counting in console frames, two ticks each, what the game counts in frames
uint32_t Ticks();
void Init();

// fast forward (debug features): the game's clock and its sound run g_fast_forward_speed times as fast (2 to 30;
// XBE_FAST_FORWARD, else 4) while F10 is held, once g_fast_forward_enabled (the Debug tab; settings.toml fast_forward,
// off by default; XBE_FAST_FORWARD turns it on too). Off, F10 is an ordinary key. The speed isn't kept in the settings
extern bool g_fast_forward_enabled;
bool FastForwardEnabled();
extern std::atomic<uint32_t> g_fast_forward_speed;
constexpr uint32_t kFastForwardMin = 2, kFastForwardMax = 30;

// GameInfo::aspect: the camera patch draws any shape, so the frame is the display's; XBE_ASPECT=W:H overrides it,
// and the headless harness keeps the console's 4:3 unless asked
float Aspect(float display);

// the height in degrees the last camera projected sees ([camera+0x264], 50 unzoomed): a sniper's scope narrows it.
// The game's own, before g_fov scales it
float CameraFov();

// around 0x3F4E0 (the player camera P): a zoomed aim's camera moves to the scope as its distance eases in, rather
// than all at once when the distance arrives
void ScopeCameraBefore(uint32_t player_camera);
void ScopeCameraAfter(uint32_t player_camera);

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

// settings.toml sharp_drone_view: the screen grab (0x3EB67B, laid over the MP Sneeker drone's whole view, refracted by
// water and heat) made at the frame's size and copied into pixel for pixel, and the drone's haze (0x1D9790) skipped;
// off, the grab is 256x256 and filtered, the haze pulses as on the console (display.cpp). Read each frame
extern bool g_sharp_drone;

// settings.toml player_tags: what a match shows over the other players, the game's tag (0x347330): their names
// (kTagNames, as on the console), a marker in place of the name (kTagMarkers: a triangle pointing down at them, green
// for a teammate, red for an opponent, the game's own colours made solid), or no name (kTagOff). The tag's icons and a
// waiting player's name with the time to their respawn stay. Read as each tag draws (player_tags.cpp)
enum : uint32_t { kTagNames = 0, kTagMarkers = 1, kTagOff = 2 };
extern uint32_t g_player_tags;
// from the hook before 0x3D3981 (menus.cpp ConkerTextQuad), a glyph's quad about to be sent: while a marker draws, its
// one glyph made the marker's triangle. Game thread
void PlayerMarkerQuad();
// settings.toml player_ranks: the tag's rank emblem (one of the sprites 0x174..0x18B, by the player's rank 0..23,
// drawn by 0x357CF0 from 0x34810A) beside the name; off, that call is skipped (player_tags.cpp). Read as each tag draws
extern bool g_player_ranks;
// settings.toml health_bars: the health bar a match shows by other players (0x348FB0, from 0x346C90): the game's own,
// upright beside them (kBarsBeside), a small one across in place of their name or marker, in a border of the marker's
// colour (kBarsInstead; one with no name, a player's past the name's reach, across where the game's was), or none
// (kBarsOff). Read as each bar draws (player_tags.cpp)
enum : uint32_t { kBarsBeside = 0, kBarsInstead = 1, kBarsOff = 2 };
extern uint32_t g_health_bars;

// settings.toml player_outlines: in a match the other players are outlined where they are in view, green for a
// teammate and red for an opponent as the game's test (0x3488E0) has them for the view's player; never through a
// wall (gpu::g_outlines, player_tags.cpp). Read each frame
extern bool g_player_outlines;

// settings.toml team_colors: the colours a match tells teammates from opponents by: the game's green and red
// (kTeamGreenRed), or blue and orange (kTeamBlueOrange), which red-green colour blindness keeps apart; a neutral's light
// blue then light grey. Each place the game picks green or red by team follows it (player_tags.cpp): by its test
// 0x3488E0 the tags' text (0x800EF4) and the weapon's target colour (0x1D3CB0), the outlines (gpu::g_team_colors);
// by the team itself the score (0x1CB330), the team icons (0x1CC500, and over a player 0x357CF0), the flag held
// (0x1EC7F0 / 0x1EC870), the flag over its carrier (0x34822F) and the message log's colour table (0x1ED983). Read as
// each draws
enum : uint32_t { kTeamGreenRed = 0, kTeamBlueOrange = 1 };
extern uint32_t g_team_colors;
// 0x1F25F0 about to add a text entry from site (its return address): the team score's in the chosen team colours,
// the text colour put back after (display.cpp's 0x1F25F0, player_tags.cpp). Game thread
struct TeamText {
    bool changed = false;
    uint8_t was[3] = {};
};
TeamText TeamTextBegin(uint32_t site);
void TeamTextEnd(const TeamText& t);

// The camera the game's draws go through ([0x84B868], its view at +0x10) for the runtime's motion blur
// (gpu::ViewConstants::camera): its view in the vertex constants kCameraRegister to kCameraRegister + 2, a row for each
// of view space's axes, and the frame (d3d::Presents) in kCameraRegister + 3, which no program of the game reads (the
// lowest it reads directly is c-38). Called as each model is drawn (tags.cpp); sets them when the camera or the frame
// changed, only with app::Settings::motion_blur on. Game thread
inline constexpr uint32_t kCameraRegister = 8;  // hardware order: D3D c-88 to c-85
void SendCamera();

}  // namespace display
