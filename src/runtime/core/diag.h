#pragma once
#include <cstdlib>
#include <filesystem>

// The debug features: the XBE_* environment variables, the F3 to F8 keys (captures, thread dumps, the profiler, frame
// freeze) and the settings menu's Debug tab. Built in when XBE_DEBUG_FEATURES is 1: the CMake option of the same name
// (on in run-linux.sh and run-proton.sh), the headless harness, or Setup's "Include debug features" (the kit's second
// host library). Left out, every XBE_* variable reads as unset and the code behind it is dropped.
#ifndef XBE_DEBUG_FEATURES
#define XBE_DEBUG_FEATURES 0
#endif

// diagnostics switched on by environment variables, read once
namespace diag {

inline constexpr bool kDebugFeatures = XBE_DEBUG_FEATURES != 0;

// an XBE_* debug variable's value; null when it is unset or the build leaves the debug features out
inline const char* Env([[maybe_unused]] const char* name) {
    if constexpr (kDebugFeatures) return std::getenv(name);
    else return nullptr;
}
#ifdef _WIN32
inline const wchar_t* WEnv([[maybe_unused]] const wchar_t* name) {
    if constexpr (kDebugFeatures) return _wgetenv(name);
    else return nullptr;
}
#endif

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = Env("XBE_PERF") != nullptr;
    return on;
}

// XBE_SKIP_FMV set: the file layer reports every .xmv as missing, so the title skips its cutscenes instead of
// playing them out in real time while porting
inline bool SkipFmv() {
    static const bool on = Env("XBE_SKIP_FMV") != nullptr;
    return on;
}

// XBE_DUMP_METHODS set: the raw NV2A methods seen just before each draw are logged, to compare a title's inlined
// state with what the runtime decodes
inline bool DumpMethods() {
    static const bool on = Env("XBE_DUMP_METHODS") != nullptr;
    return on;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, under app::StateDir; cap unless set. Empty, no captures: a build without
// the debug features
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
        if constexpr (!kDebugFeatures) return std::filesystem::path();
#ifdef _WIN32
        const wchar_t* v = WEnv(L"XBE_CAPTURE_DIR");
#else
        const char* v = Env("XBE_CAPTURE_DIR");
#endif
        return v && *v ? std::filesystem::path(v) : std::filesystem::path("cap");
    }();
    return dir;
}

}  // namespace diag
