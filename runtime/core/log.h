#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>

namespace logx {

enum class Level { Debug, Info, Warn, Error };

void Init(const std::string& dir, const std::string& prefix);
void Write(Level lv, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Flush();
const std::string& Path();

}  // namespace logx

#define LOGD(...) ::logx::Write(::logx::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::logx::Write(::logx::Level::Info, __VA_ARGS__)
#define LOGW(...) ::logx::Write(::logx::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::logx::Write(::logx::Level::Error, __VA_ARGS__)

// logs only the first n times a call site is reached
#define LOG_FIRST(n, ...)                                  \
    do {                                                   \
        static std::atomic<int> _c{0};                     \
        if (_c.fetch_add(1, std::memory_order_relaxed) < (n)) LOGI(__VA_ARGS__); \
    } while (0)
