// The log's opening lines everywhere but Windows (app/system_info.cpp): what the game runs on and with which settings.
// The GPU is the renderer's to name, once it picked one.
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <sys/utsname.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

namespace app {

namespace {

#ifdef __ANDROID__
std::string Property(const char* key) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(key, v);
    return v;
}
#endif

// the distribution's name (Android's version and the device), then the kernel
std::string OsVersion() {
    std::string name;
#ifdef __ANDROID__
    name = "Android " + Property("ro.build.version.release") + " (API " + Property("ro.build.version.sdk") + "), " +
           Property("ro.product.manufacturer") + " " + Property("ro.product.model");
#endif
    std::ifstream f("/etc/os-release");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        name = line.substr(12);
        if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);
    }
    utsname u{};
    if (uname(&u) == 0) name += (name.empty() ? "" : ", ") + std::string(u.sysname) + " " + u.release;
    return name.empty() ? "unknown" : name;
}

std::string CpuName() {
#if !defined(__x86_64__) && !defined(__i386__)
#ifdef __ANDROID__
    std::string soc = Property("ro.soc.manufacturer") + " " + Property("ro.soc.model");
    if (soc != " ") return soc;
    return Property("ro.board.platform");
#else
    return "unknown";
#endif
#else
    unsigned r[4] = {};
    if (!__get_cpuid(0x80000000, &r[0], &r[1], &r[2], &r[3]) || r[0] < 0x80000004) return "unknown";
    char brand[49] = {};
    for (unsigned i = 0; i < 3; i++) {
        __get_cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3]);
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = brand;
    s.erase(0, s.find_first_not_of(' '));
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
#endif
}

}  // namespace

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s", kGame.title, IMAGE_XBE, date);
    LOGR("Path: %s", ExeDir().c_str());
    LOGR("OS: %s", OsVersion().c_str());
    LOGR("CPU: %s (%u threads)", CpuName().c_str(), std::thread::hardware_concurrency());
    LOGR("RAM: %.1f GB", double(sysconf(_SC_PHYS_PAGES)) * double(sysconf(_SC_PAGE_SIZE)) / 1073741824.0);
    Mode desk = Desktop();
    LOGR("Desktop: %ux%u", desk.width, desk.height);
}

}  // namespace app
