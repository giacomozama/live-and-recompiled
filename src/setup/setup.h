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
    std::filesystem::path toolchain;  // another copy of the target's compiler (testing)
    unsigned jobs = 1;
    bool keep = false;                // leave the generated code, the objects and the unpacked kit in the work folder
    bool keep_compiler = true;        // leave the downloaded compiler there too, for the next install
};

// the kit's own facts: what the window shows before anything is chosen
struct KitInfo {
    std::string title;
    std::string error;                    // set when the kit cannot be read
};

KitInfo ReadKit();

// the default of every choice but the game: the working folder (work) and the game folder (game), both beside Setup
Options Defaults(const std::filesystem::path& root);

// where the Microsoft Store installed the game (Windows), for the window to start with; empty when it isn't found
std::filesystem::path FindStoreInstall();

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

// a window with the three choices and the build's progress; the exit code
int RunWindow(const Options& defaults);
bool WindowAvailable();

}  // namespace setup
