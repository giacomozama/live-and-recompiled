#pragma once

// What Setup's build (main.cpp) offers its window (gui.cpp): the choices a player makes, a check of the game's source
// and the build itself, which tells its progress through a sink.
#include <filesystem>
#include <string>

namespace setup {

// what a player picks; the game starts empty, the other paths start as Defaults() fills them in
struct Options {
    std::filesystem::path root;       // the folder Setup is in
    std::filesystem::path source;     // the game: a DefaultPackage, a disc image (.iso), a folder with default.xbe, or a
                                      // folder holding one of these
    std::filesystem::path work;       // the kit, the compiler and, while building, the generated code and objects
    std::filesystem::path game_dir;   // where the game is made: its executable, and the game's files in its assets folder
    unsigned jobs = 1;
    bool source_found = false;        // Setup found source by itself: the window may put another in its place
    bool portable = false;            // put portable.txt in the game folder: settings and saves stay beside the game
    bool keep = false;                // leave the generated code, the objects and the unpacked kit in the work folder
    bool debug_features = false;      // link the host with the debug features (XBE_* variables, F3-F8, the Debug tab)
    bool launcher = false;            // start the game, building it first when it is missing or from another kit
};

// the kit's own facts: what the window shows before anything is chosen
struct KitInfo {
    std::string title;
    std::string game;                     // the executable's file name, in the game folder
    std::string error;                    // set when the kit cannot be read
};

KitInfo ReadKit();

// the default of every choice but the game: the working folder (work) and the game folder (game), both beside Setup
Options Defaults(const std::filesystem::path& root);

// the game Setup made in the game folder, when it is there and was made from this kit with the same debug features;
// empty otherwise (missing, from another Setup, or linked the other way)
std::filesystem::path BuiltGame(const Options& opt);

// runs the game from its folder; on Linux in place of Setup, on Windows waiting for it. Returns its exit code (or 1
// when it cannot be started)
int Launch(const std::filesystem::path& game);

// where the Microsoft Store installed the game (Windows), for the window to start with; empty when it isn't found
std::filesystem::path FindStoreInstall();

// what opt.game_dir holds already: a game, which a build updates (or repairs, when it came from this same kit), and the game's files in its assets folder, when this
// Setup can build from them (empty otherwise)
struct Existing {
    bool game = false;
    bool current = false;  // the game was made from this kit, with or without the debug features
    std::filesystem::path source;
};
Existing FindExisting(const Options& opt);

// what is at opt.source, for the window to show: ok when the build can use it
struct Probe {
    bool ok = false;
    std::string text;
};
Probe CheckSource(const Options& opt);

// the build, to the end; false when it failed or was cancelled. Everything it says goes to stdout, and
// to the sink when there is one.
bool Build(const Options& opt);
void SetSink(void (*sink)(const char* text));
// called from the build's thread with how much of it is done, 0 to 1; it only goes up
void SetProgress(void (*progress)(double fraction));
void Cancel();
bool Cancelled();

// a window with the three choices and the build's progress; the exit code, or kNoWindow when none could open (Linux:
// neither window module loads, for a library the desktop lacks), and the console build goes on instead
int RunWindow(const Options& defaults);
bool WindowAvailable();
constexpr int kNoWindow = -1;

// Linux: the window is a module Setup carries inside itself and loads as it opens one (gui_linux.cpp), built twice from
// gui.cpp, against FLTK for X11 alone and for Wayland alone, so Setup needs neither and a desktop needs only its own.
// The module calls the build through this (gui_module.cpp); its one export is
//   extern "C" int setup_gui_run(const GuiApi* api, const Options* defaults)   RunWindow's exit code
struct GuiApi {
    KitInfo (*read_kit)();
    Existing (*find_existing)(const Options&);
    Probe (*check_source)(const Options&);
    bool (*build)(const Options&);
    void (*set_sink)(void (*sink)(const char* text));
    void (*set_progress)(void (*progress)(double fraction));
    void (*cancel)();
};

}  // namespace setup
