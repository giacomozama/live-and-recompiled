#pragma once

// What Setup's build (main.cpp) offers its window (gui.cpp): the choices a player makes, a check of the game's source
// and the build itself, which tells its progress through a sink.
#include <filesystem>
#include <string>

namespace setup {

// what a player picks; every path empty means "the default" and Defaults() fills them in
struct Options {
    std::filesystem::path root;       // the folder Setup is in: kit/ is below it
    std::filesystem::path source;     // the game: a DefaultPackage, a disc image (.iso), a folder with default.xbe, or a
                                      // folder holding one of these
    std::filesystem::path work;       // where the generated code and the objects go while building
    std::filesystem::path game_dir;   // where the game is made: its executable, with the game's files in assets/ below it
    std::filesystem::path toolchain;  // another copy of the target's compiler (testing)
    std::string target;               // windows or linux, empty: this system's
    unsigned jobs = 1;
    bool keep = false;                // leave the work folder in place
};

// the kit's own facts: what the window shows before anything is chosen
struct KitInfo {
    std::string title;
    bool has_windows = false, has_linux = false;  // the targets it can build
    std::string error;                    // set when the kit cannot be read
};

KitInfo ReadKit(const std::filesystem::path& root);

// the default of every empty choice: the game where Setup looks for it (assets, or on Windows the Microsoft Store's
// install), kit/work, and Setup's own folder
Options Defaults(const std::filesystem::path& root);

// what is at opt.source, for the window to show: ok when the build can use it
struct Probe {
    bool ok = false;
    std::string text;
};
Probe CheckSource(const Options& opt);

// the build, to the end; false when it failed or was cancelled. Everything it says goes to stdout (and setup.log), and
// to the sink when there is one.
bool Build(const Options& opt);
void SetSink(void (*sink)(const char* text));
void Cancel();
bool Cancelled();

// a window with the three choices and the build's progress; the exit code
int RunWindow(const Options& defaults);
bool WindowAvailable();

}  // namespace setup
