#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// the host side: window, settings, options menu
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited
    bool fps_counter = false;
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    uint32_t shadow_resolution = 0;  // shadow map edge, 0 follows the render scale; 512 is the console's, edge blur included
    uint32_t shader_model = 3;       // 3 turns on the PC style features below, 1 is the console's look
    bool hdr_rendering = true;
    bool tone_mapping = true;
    bool hq_soft_shadows = true;
    float mouse_sensitivity = 1.0f;
    bool invert_mouse = false;
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};

Settings& Get();
void Load();
void Save();
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();

std::filesystem::path ExeDir();
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go
// the game menus' cursor (HCURSOR): cursor.png or cursor.dds beside the exe sized to the frame, else the system arrow
void* MenuCursor();

// options menu, drawn by the renderer each frame with the lock held
std::recursive_mutex& UiLock();
bool MenuOpen();
bool Capturing();  // a key binding waits for a key
void ToggleMenu();
void DrawUi(double fps);

// option rows shared by the F10 panel and the game's options menu; page 0 recomp settings, 1 mouse and keyboard
namespace rows {
enum class Kind { Heading, Value, Bind, Text };
struct Row {
    Kind kind;
    int id;  // a bind row's input action
    const char* label;
};
const Row* Page(int page, size_t* count);
std::string Value(const Row& row);
void Step(const Row& row, int step);
void Defaults(int page);
// for the game's rows, whose ids start at 100
constexpr int kJoinIp = 15;
const char* OnOff(bool on);
int StepTable(const uint32_t* values, int count, uint32_t& value, int step);  // index nearest value, stepped first
void Changed(bool display);                                                   // applies and saves
}  // namespace rows

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct LevelList {
    const char* title;
    std::vector<const char*> maps;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when the game files are not found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    uint32_t console_shadow;     // shadow resolution labelled CONSOLE, 0 none
    std::vector<Option> options;
    std::vector<rows::Row> rows;                // appended to the settings page
    std::string (*row_text)(int id, int step);  // steps first when step is not zero
    void (*defaults)();
    void (*init)();                             // after the settings load, before input and boot
    void (*loaded)();                           // image in memory, before its entry runs
    void (*command)(const std::string& line);   // console line; null drops it
    std::vector<std::string> (*output)();
    const std::vector<LevelList>& (*levels)();  // null: no level select
    void (*open_level)(const char* map);
};
extern const GameInfo kGame;

// the console on the backtick key, drawn by DrawUi
bool ConsoleOpen();
void ToggleConsole();
void DrawConsole();

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app
