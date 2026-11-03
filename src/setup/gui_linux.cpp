// Setup's window on Linux: one of two modules Setup carries inside itself, gui.cpp built against FLTK for Wayland alone
// and for X11 alone (cmake/XbeRecomp.cmake), loaded as the window opens. Setup itself links no desktop library, so its
// console build runs on a system with neither, and a desktop needs only its own: a Wayland session's window asks for
// Wayland, xkbcommon, Cairo and Pango, an X11 one's for X11 and Xft. A Wayland session takes the Wayland module and,
// when that can't load, the X11 one through XWayland; without either, Setup builds on the console.
//
// A module goes to an anonymous memory file and is loaded from there (/proc/self/fd), so nothing is written beside
// Setup or left in /tmp; a kernel too old for memfd_create (before 3.17) gets an unlinked temporary file instead.
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "setup.h"

#if !defined(SETUP_GUI_X11) || !defined(SETUP_GUI_WAYLAND)
#error "SETUP_GUI_X11 and SETUP_GUI_WAYLAND name the window modules: built by cmake/XbeRecomp.cmake"
#endif

// the modules, linked in whole (SETUP_GUI_X11 and SETUP_GUI_WAYLAND are their paths, set by CMake)
#define SETUP_INCBIN(name, path)                                                                     \
    asm(".section .rodata\n.balign 16\n.global " #name "\n" #name ":\n.incbin \"" path "\"\n"        \
        ".global " #name "_end\n" #name "_end:\n.previous\n");                                       \
    extern "C" const unsigned char name[], name##_end[];
SETUP_INCBIN(setup_gui_x11, SETUP_GUI_X11)
SETUP_INCBIN(setup_gui_wayland, SETUP_GUI_WAYLAND)

namespace setup {

namespace {

bool Set(const char* var) {
    const char* v = std::getenv(var);
    return v && *v;
}

bool WriteAll(int fd, const unsigned char* data, size_t size) {
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= size_t(n);
    }
    return true;
}

// a file holding the module, open; -1 when none could be made
int ModuleFile(const char* name, const unsigned char* data, size_t size) {
#ifdef SYS_memfd_create
    // MFD_EXEC (0x10): a kernel set to make memory files no-exec by default (vm.memfd_noexec) still loads it; one that
    // predates the flag refuses it, and is asked again without
    int fd = int(syscall(SYS_memfd_create, name, 0x1u /* MFD_CLOEXEC */ | 0x10u));
    if (fd < 0 && errno == EINVAL) fd = int(syscall(SYS_memfd_create, name, 0x1u));
    if (fd >= 0) {
        if (WriteAll(fd, data, size)) return fd;
        close(fd);
    }
#endif
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(dir && *dir ? dir : "/tmp") + "/" + name + "-XXXXXX";
    int fd2 = mkstemp(path.data());
    if (fd2 < 0) return -1;
    unlink(path.c_str());  // the open file stays loadable through /proc/self/fd
    if (WriteAll(fd2, data, size)) return fd2;
    close(fd2);
    return -1;
}

using RunFn = int (*)(const GuiApi*, const Options*);

// the module's entry, or null with why in error
RunFn Load(const char* name, const unsigned char* begin, const unsigned char* end, std::string& error) {
    int fd = ModuleFile(name, begin, size_t(end - begin));
    if (fd < 0) {
        error = "cannot write it to memory";
        return nullptr;
    }
    std::string path = "/proc/self/fd/" + std::to_string(fd);
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    close(fd);  // a loaded module keeps its own mapping
    if (!lib) {
        const char* e = dlerror();
        error = e ? e : "dlopen failed";
        return nullptr;
    }
    auto run = reinterpret_cast<RunFn>(dlsym(lib, "setup_gui_run"));
    if (!run) error = "it has no setup_gui_run";
    return run;
}

const GuiApi kApi = {ReadKit, CheckSource, Build, SetSink, SetProgress, Cancel};

}  // namespace

bool WindowAvailable() { return Set("DISPLAY") || Set("WAYLAND_DISPLAY"); }

int RunWindow(const Options& defaults) {
    struct Module {
        const char* name;
        const unsigned char *begin, *end;
        bool usable;
    } modules[] = {
        {"setup-gui-wayland", setup_gui_wayland, setup_gui_wayland_end, Set("WAYLAND_DISPLAY")},
        {"setup-gui-x11", setup_gui_x11, setup_gui_x11_end, Set("DISPLAY")},
    };
    for (const Module& m : modules) {
        if (!m.usable) continue;
        std::string error;
        if (RunFn run = Load(m.name, m.begin, m.end, error)) return run(&kApi, &defaults);
        std::fprintf(stderr, "setup: no %s window: %s\n", m.name + 10, error.c_str());
    }
    std::fprintf(stderr, "setup: building on the console instead (setup-linux --help lists its options)\n\n");
    return kNoWindow;
}

}  // namespace setup
