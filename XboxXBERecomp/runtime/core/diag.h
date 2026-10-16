#pragma once
#include <cstdlib>
#include <filesystem>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// XBE_SKIP_FMV set: the file layer reports every .xmv as missing, so the title skips its cutscenes instead of
// playing them out in real time while porting
inline bool SkipFmv() {
    static const bool on = std::getenv("XBE_SKIP_FMV") != nullptr;
    return on;
}

// XBE_DUMP_METHODS set: the raw NV2A methods seen just before each draw are logged, to compare a title's inlined
// state with what the runtime decodes
inline bool DumpMethods() {
    static const bool on = std::getenv("XBE_DUMP_METHODS") != nullptr;
    return on;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

}  // namespace diag
