#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
// the title's distance fog: as the title draws it, every surface unfogged, or the haze gone and the fogged draws
// tinted by the fog color's hue as far as the fog would reach (gpu::PixelKey::fog_tint)
enum class Fog : uint32_t { Enabled, Disabled, TintOnly };

// a folder in ModsDir and whether it is in use
struct ModEntry {
    std::string name;
    bool enabled = true;
    bool operator==(const ModEntry&) const = default;
};

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the size the frame is shown at (RenderScale)
    // a temporal upscaler over the frame's pictures, which then sets the render size itself: "off", or "dlss" (an
    // NVIDIA RTX GPU, with the DLSS module beside the game); its quality, XbeDlssQuality (gpu/dlss.h)
    std::string upscaler = "off";
    uint32_t upscaler_quality = 1;
    // the model DLSS runs, XbeDlssModel (gpu/dlss.h): K, M, L, or NVIDIA's pick for the quality mode
    uint32_t dlss_model = 0;
    // RCAS (FSR 1's sharpening) over DLSS's picture, in % (0 off); fsr_sharpness is FSR 1's own
    uint32_t dlss_sharpness = 0;
    // super fur's shells shaded once per 2x1 or 2x2 pixels as far as the frame rate needs it (VulkanBackend::AutoFurRate),
    // else once per pixel, with DLSS or without: under DLSS it rebuilds their strands from the jitter alike, at a
    // fraction of the cost in a close-up (gpu::ProgramShading::coarse). The name is from when it was DLSS's alone
    bool dlss_fur_auto = true;
    uint32_t fps_cap = 60;           // 0 unlimited
    // pictures past the monitor's refresh rate with vsync, one a refresh (VulkanBackend::InStep): false, each frame's
    // own picture shown at the refresh nearest it, up to half a refresh off, one scene drawn a picture; true, every
    // picture at its own time, most of them drawn again (dearer)
    bool picture_timing_exact = false;
    bool vsync = true;
    // HDR output (an HDR10 swap chain, where the display and compositor offer one): the SDR picture shown with its
    // white at hdr_paper_white nits, 0 the display's (HdrPaperWhite)
    bool hdr = false;
    uint32_t hdr_paper_white = 0;
    // the 3D picture's highlights taken above paper white, under the 2D: how far down they start in % (0 off), and
    // the nits white reaches, 0 the display's peak (HdrPeak)
    uint32_t hdr_highlights = 50;
    uint32_t hdr_peak = 0;
    // the display gamma the picture is shown for, in hundredths: 220 as it was drawn, higher darker in the midtones
    uint32_t gamma = 220;
    // the black level in % of white, -20 to 20: raised above 0, shadows taken down to black below it; white kept
    int32_t brightness = 0;
    // pictures shown per frame the title presents: 1 (off), or 2 to 4, the ones between made from the frames around
    // them; the title sets it (Conker from its frame rate), it isn't saved
    uint32_t frame_generation = 1;
    uint32_t aniso = 0;              // 0 as the game asks
    uint32_t msaa = 1;               // samples per pixel of the frame-sized targets: 1 (off), 2, 4 or 8
    // effects over the 3D picture, before the title's 2D draws: SMAA, then film grain and chromatic aberration in %
    bool smaa = false;
    uint32_t film_grain = 0;
    uint32_t chromatic_aberration = 0;
    // ambient occlusion from the depth buffer, first: its strength in % (0 off) and its reach in the title's units
    uint32_t ambient_occlusion = 0;
    uint32_t ambient_occlusion_radius = 30;
    // motion blur from the camera's turning and moving (the picture's depth through the camera's view now and a frame
    // before, gpu::ViewConstants::camera): the blur's length as a % of the camera's motion between two pictures, 0 off
    uint32_t motion_blur = 0;
    Fog fog = Fog::Enabled;
    // the levels' baked lightmaps made larger, their aliased light and shadow edges smoothed along themselves
    // (gpu::SmoothLightmap)
    bool smooth_lightmaps = true;
    uint32_t lightmap_smoothing = 5;   // how far along an edge each texel averages, in lightmap texels (1 to 10)
    uint32_t lightmap_size = 512;      // the largest a smoothed lightmap gets across: 256 to 2048, powers of two
    // AMD FidelityFX Super Resolution 1.0 (EASU spatial upscaling + RCAS contrast-adaptive sharpening)
    bool fsr = false;
    uint32_t fsr_sharpness = 80;
    uint32_t fur_sharpness = 0;  // the share of fsr_sharpness fur takes, in % (ProgramShading::unsharpened)
    uint32_t volume = 100;           // the game's sound in %, the mixer's output scaled by its square
    std::string audio_device;         // an audio::Device id, "" the system default
    bool menu_sounds = true;          // the settings menu's sounds (audio::Play)
    bool achievement_sound = true;    // the sound as an achievement's toast appears
    bool mute_in_background = false;  // silent while the game window lacks the focus (Background)
    // the speakers the console reports and the mixer plays on: stereo, or 5.1 surround; read at launch (audio::Channels)
    bool surround = false;
    // the mixer's choices where no one agrees what the console sounds like; the defaults are what the console's own
    // code does (dsound.cpp, dsfx.cpp), read as the mix runs
    bool stereo_full_backs = false;  // on two speakers the backs fold into their side at 0 dB, not the encoder's -3 dB
    bool stereo_sub = false;         // on two speakers the sub folds into both sides at -3 dB, which the encoder drops
    uint32_t reverb = 100;           // the room reverb's level in % (the DSP's reverb is approximated, not run)
    bool crosstalk = true;           // the GP's crosstalk cancellers on 3D sounds; off passes their inputs through
    bool doppler = true;             // 3D voices' Doppler pitch shift
    bool distance_filters = true;    // 3D voices' I3DL2 direct and room low-pass filters
    uint32_t resampler = 0;          // how voices are read between their samples: 0 linear, 1 cubic, 2 sinc
    bool voice_headroom = true;      // voices keep the library's headroom (-6 dB on most 2D ones) and SetHeadroom's
    bool soft_clip = false;          // the mix bends towards full scale instead of clipping at it
    bool envelopes = true;           // XACT's volume, pitch and cutoff envelopes, release envelopes and release tails
    bool lfo = true;                 // XACT's LFOs (vibrato, tremolo, filter sweeps); their rate and depth are a guess
    bool voice_filters = true;       // XACT's per-sound DLS2 low-pass filters
    bool shader_warmup = true;       // make the shaders shaders.rec lists while the title boots
    std::string gpu;                  // the name of the graphics adapter to draw with, "" the best there is (read at launch)
    std::string renderer = "vulkan";  // which gpu::Backend draws, read at launch (gpu/backend.cpp)
    std::string aspect = "auto";     // "W:H" ("4:3", "16:9", "21:9", ...), or "auto": the output's own shape
    std::string join_ip;              // host[:port]: a system link host past the LAN (or on this PC) the search also asks
    uint32_t system_link_port = 3074;  // the UDP port system link binds, read as the network starts
    // the console's language (XC_LANGUAGE), by its name in kLanguages; the title reads it once at boot
    std::string language = "english";
    // the language of the settings menu and its prompts (by name in kLanguages, as far as it has text for), live; apart
    // from the game's; a file without it gets the game's
    std::string ui_language = "english";
    // the mods in ModsDir, the first winning where two replace the same file; read at launch (MountMods)
    std::vector<ModEntry> mods;
};

// the console's languages: the language setting's name for each and its XC_LANGUAGE number
struct Language {
    const char* key;
    const char* name;
    uint32_t code;
};
inline constexpr Language kLanguages[] = {
    {"english", "English", 1},   {"japanese", "Japanese", 2},     {"german", "German", 3},
    {"french", "French", 4},     {"spanish", "Spanish", 5},       {"italian", "Italian", 6},
    {"korean", "Korean", 7},     {"chinese", "Chinese", 8},       {"portuguese", "Portuguese", 9},
};
// the language setting's XC_LANGUAGE, English for a name it doesn't know
inline uint32_t LanguageCode(const std::string& key) {
    for (const Language& l : kLanguages)
        if (key == l.key) return l.code;
    return 1;
}

Settings& Get();
void Load();
void Save();
// there was no settings file to Load: the game's first launch here (the welcome screen shows, RunWelcome)
bool FirstRun();
// Save, when the settings differ from what the file had at Load or last Save; true when it wrote
bool SaveChanged();
std::string SettingsText();  // the settings as Save writes them
void LogSystem();  // the machine, at a run's start
void InstallCrashHandler();          // crash.cpp: unhandled exceptions, aborts and terminate write a report to the log
void ApplyDisplay();  // any thread
void OpenFolder(const std::filesystem::path& dir);  // in the system's file manager
// the HDR levels the display reports, in nits, 0 not known: the SDR white the compositor maps and the display's peak
// (on Wayland the surface's preferred image description, ReadDisplayHdr); any thread
void SetDisplayHdr(uint32_t paper_white, uint32_t peak);
uint32_t DisplayHdrPaperWhite();
uint32_t DisplayHdrPeak();
// the levels asked for again (the platform's; an HDR swap chain was made, the display may have just turned HDR on)
void ReadDisplayHdr();
// the settings' paper white and peak, or the display's where a setting is 0 (within the settings' ranges), else 200
// and 1000
uint32_t HdrPaperWhite();
uint32_t HdrPeak();
constexpr uint32_t kPaperWhiteMin = 80, kPaperWhiteMax = 500, kPeakMin = 300, kPeakMax = 4000;
// F11 and Alt+Enter: windowed, or back to the fullscreen or borderless it was in last; any thread
void ToggleWindowed();
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
// host pixels per console pixel across and down: the guest frame drawn at the size the window shows it (the output
// letterboxed to FrameAspect) times render_percent, so a 4:3 frame shown 16:9 draws with more pixels across than down
struct Scale {
    float x, y;
};
Scale RenderScale(uint32_t guest_width, uint32_t guest_height);
// the display shape the aspect setting asks for, width over height: with "auto" the output's own
float Aspect();
// the display is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();
// the shape of the frame the title draws, which the window letterboxes: GameInfo::aspect's for a title patched to
// draw any, else 16:9 on a widescreen TV and 4:3 on a 4:3 one
float FrameAspect();

std::filesystem::path ExeDir();
// this executable run again with args (UTF-8), waited for: its exit code, -1 when it couldn't start or stop was set
// (the run killed then). Any thread
int RunSelf(const std::vector<std::string>& args, const std::atomic<bool>* stop = nullptr);
// where the game writes (paths.cpp), each folder made: the platform's for the user (%APPDATA% and %LOCALAPPDATA%, the
// XDG folders elsewhere), or ExeDir with a portable.txt beside the exe
std::filesystem::path ConfigDir();  // the settings file
std::filesystem::path DataDir();    // the hard disk: saves
std::filesystem::path StateDir();   // logs, crash dumps, captures
std::filesystem::path CacheDir();   // shaders.rec and shader_programs.bin
// mods (mods.cpp): each a folder in ModsDir laid out like the game's disc, its files used in place of the game's
std::filesystem::path ModsDir();  // ConfigDir/mods, made
// the mods setting brought in line with ModsDir's folders: ones gone are dropped, new ones added at the end, in use
void ScanMods();
// at launch, before Boot: scans, then D: shows the mods in use through it (kernel::MountMods)
void MountMods();
// the mods in use, in order, differ from the ones this launch mounted
bool ModsChanged();
// at launch, after the log is open: a settings file, hard disks and shader records left beside the exe by an older
// build go to their folders above, when those have none yet
void MoveOldData();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
// FindGame's folder (its extracted files, or the one with the disc image kernel::MountedDisc reads), empty before
const std::filesystem::path& GameDir();
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
int PortArg();   // --port N on the command line, 0 without: this copy's system link port, over the setting
const std::string& JoinArg();  // --join host[:port] on the command line, "" without: over the join_ip setting
void* Window();  // the SDL_Window
void* NativeWindow();  // the SDL window's HWND on Windows (the D3D11 renderer's swap chain), else null

// The SDL window as the game's hooks see it. GameInfo::message's numbers are Win32's, and the window sends them the
// Win32 way: kMsgKeyDown with the
// virtual-key code in wp and bit 30 of lp set on auto repeat, kMsgMouseWheel as WM_MOUSEWHEEL, kMsgActivate with LOWORD(wp) 0 when the window loses
// the focus, kMsgSize and kMsgMove without arguments, kMsgChar as WM_CHAR (one character, while TextInput is on), and
// messages Post sent.
enum : uint32_t {
    kMsgMove = 0x0003,
    kMsgSize = 0x0005,
    kMsgActivate = 0x0006,
    kMsgKeyDown = 0x0100,
    kMsgChar = 0x0102,  // a character typed, its code point in wp
    kMsgMouseWheel = 0x020A,  // the signed turn in HIWORD(wp), 120 a notch, away from the player positive
    kMsgApp = 0x8000,                  // WM_APP: a game's own from here
    // raw relative motion: int32 dx in wp's low half and dy in its high half (MotionWp), the mouse's input::Device id
    // in lp; only while HoldMouse holds it
    kMsgMouseMotion = kMsgApp + 0x3F,
};
inline uintptr_t MotionWp(int32_t dx, int32_t dy) { return uintptr_t(uint32_t(dx)) | uintptr_t(uint32_t(dy)) << 32; }
// a key or mouse button held, by Windows virtual-key code (the codes settings.toml's key names stand for); any thread
bool KeyDown(int vk);
// the cursor in the game window's client area, in its own units (w by h the area); false without a window or when the
// cursor is outside it; any thread
bool Cursor(float& x, float& y, float& w, float& h);
void Post(uint32_t msg);                    // any thread: msg to GameInfo::message on the window thread
void Quit();                                // any thread: closes the window, as its X does
void SetTitle(const std::wstring& title);   // window thread
bool Focused();                             // window thread: the game window has the keyboard
bool Background();                          // any thread: the game window lost the focus, as of its last message
void HoldMouse(bool hold);                  // window thread: the cursor hidden, motion as kMsgMouseMotion
// any thread: typed text comes as kMsgChar while on (the SDL window's text input)
void TextInput(bool on);
// a message box over the game; ask: Yes and No, true for Yes
bool Alert(const std::wstring& text, bool ask = false);
// the display's modes, de-duplicated by size, largest first
std::vector<Mode> DisplayModes();
#ifdef _WIN32
// the graphics driver's version for the adapter with this PCI vendor and device ID, its four parts packed 16 bits each
// (31.0.101.2135 is 0x001F000000650857), 0 when Windows does not say
uint64_t GpuDriverVersion(uint32_t vendor, uint32_t device);
#endif

// the SDL window for the Vulkan renderer: the instance extensions it needs, its surface (VkInstance, VkSurfaceKHR*),
// its size in pixels, and ImGui's platform frame; any thread
std::vector<const char*> VulkanExtensions();
bool VulkanSurface(void* instance, void* surface);
void PixelSize(uint32_t& w, uint32_t& h);
void UiNewFrame();
// any thread, before handing the window over to a relaunched instance (kernel::Relaunch): losing the focus to it no
// longer minimizes an exclusive fullscreen window, which would give the desktop's mode back for the new one's start
void KeepFullscreenOnFocusLoss();
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go
// window thread: the window's waiting messages handled; false once the window is to close
bool Pump();
// the first launch (FirstRun) of a game with GameInfo::welcome: before Boot, on the window thread, the renderer made
// early and the welcome screen drawn over black until the player is done, then the settings saved; the title's device
// takes the renderer over (gpu::g_early). False when the window was closed on it
bool RunWelcome();
bool Welcoming();  // any thread: RunWelcome is up
void DrawWelcome();  // the renderer, inside the ImGui frame while Welcoming, in place of GameInfo::ui; UiLock held

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();
// the overlay's font (ui_font.cpp), sized to the window's height
void UiFont();        // after ImGui::CreateContext
void UiAccent(int index);  // the accent color, 0 to 6; UiLock held
void UiAccentColor(int index, float rgb[3]);  // that accent, for the settings menu's swatches
void UiFontResize();  // each frame before ImGui's, UiLock held

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file in ConfigDir
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // the shape of the frame the title draws for a display of this shape (width over height), for a title whose
    // projection the game patches to fit any; null: the console's 16:9 or 4:3 (Widescreen)
    float (*aspect)(float display);
    // the first launch's welcome screen (RunWelcome), before boot: each frame inside the ImGui frame in place of ui,
    // UiLock held; true once the player is done with it. Null: no welcome screen
    bool (*welcome)();
    // shader records shipped with the game, in shaders.rec's format with gpu::kRecordLayout: what the title's own code
    // draws, which the disc's records (gpu/shader_gen.h) can't find, made ahead with them. Null: none
    const uint8_t* base_shaders = nullptr;
    size_t base_shaders_size = 0;
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap, and for the console's presentation interval: a title that
// presents every vblanks-th 60 Hz blank (D3DPRESENT_INTERVAL_TWO is 2) never runs faster than 60 / vblanks
void Pace(uint32_t vblanks = 1);
// the same for a picture of the held title shown again under the host menu (Redisplay): at the monitor's refresh rate,
// not the cap, so the menu moves as smoothly as the display can show it
void PaceMenu();
// the window's monitor's refresh rate in Hz (60 when unknown)
uint32_t MonitorRefresh();
// the same, unrounded where the system gives it (164.958 for a 165 Hz mode): frame generation spaces the pictures it
// shows a refresh apart by it
double MonitorHz();
// vsync forced on: the pictures a second (the cap times frame generation) pass the monitor's refresh, where without
// it they would tear and some never show; Vsync, the swap chain's: the setting or that
inline bool VsyncForced(const Settings& s) {
    return s.fps_cap && double(s.fps_cap) * double(s.frame_generation > 1 ? s.frame_generation : 1) > MonitorHz() + 0.5;
}
inline bool Vsync(const Settings& s) { return s.vsync || VsyncForced(s); }

}  // namespace app
