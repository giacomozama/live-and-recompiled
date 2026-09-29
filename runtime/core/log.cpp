#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace logx {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();

void Init(const std::string& dir, const std::string& prefix) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    int n = 1;
    for (; n < 1000; n++) {
        char name[256];
        std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), n);
        if (!fs::exists(fs::path(dir) / name)) {
            g_path = (fs::path(dir) / name).string();
            break;
        }
    }
    if (n >= 1000) g_path = (fs::path(dir) / (prefix + ".log")).string();
    g_file = std::fopen(g_path.c_str(), "w");
}

const std::string& Path() { return g_path; }

void Write(Level lv, const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[4300];
    int n = std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), msg);
    if (g_file) {
        std::fwrite(line, 1, size_t(n), g_file);
        if (lv >= Level::Warn) std::fflush(g_file);
    }
    std::fwrite(line, 1, size_t(n), stderr);
}

void Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fflush(g_file);
}

}  // namespace logx
