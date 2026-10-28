// The APK builder: builds the Android app on the player's PC from the player's own copy of the game, so no release
// ever carries the title's code. A release is this program with a kit inside it (the recompiler for this PC, the
// Android build of everything but the title's code, the headers the generated code includes, the NDK's sysroot, the
// config and the base APK; unpacked into the working folder on the first run). It is Setup's sibling and works the
// same way, for Android rather than the PC it runs on.
//
// The builder reads only default.xbe from the player's copy (its files, a disc image or the digital release's
// package), checks it is the release the kit was made for, downloads the compiler the kit's android.toml pins for this
// PC into work/toolchain/android-<host> (once), recompiles the game, compiles the result for 64-bit ARM Android and
// links libmain.so, then puts it in the base APK and signs that with the player's own key (apk.cpp), made the first
// time. The game's files go onto the device separately: the app asks for them the first time it runs.
//
//   apk-builder-windows.exe, apk-builder-linux        a window: pick the game, the working folder and where the APK
//                                                     goes (the last two start as the defaults)
//   apk-builder-windows.exe, apk-builder-linux --cli --source PATH [--work DIR] [--out DIR] [--key FILE]
//                                  [--jobs N] [--keep] [--delete-compiler] [--toolchain DIR] [--root DIR]
//
// --key is the signing key (android-signing.toml in the settings folder, %APPDATA%\LiveAndRecompiled or
// ~/.config/LiveAndRecompiled): Android installs a new build over an old one only when one key signed both.
#include <miniz.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "apk.h"
#include "apkbuilder.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"
#include "setup/store.h"

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHost = "windows";
#else
constexpr const char* kHost = "linux";
#endif

void (*g_sink)(const char*) = nullptr;  // the window's, when there is one
std::atomic<bool> g_cancel{false};
void (*g_progress)(double) = nullptr;  // the window's bar: how much of the build is done, 0 to 1
double g_done = 0;

// the bar only moves forward
void Progress(double fraction) {
    g_done = std::max(g_done, std::min(fraction, 1.0));
    if (g_progress) g_progress(g_done);
}

void Say(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    if (g_sink) {
        char text[2048];
        va_list c;
        va_copy(c, a);
        std::vsnprintf(text, sizeof(text), fmt, c);
        va_end(c);
        g_sink(text);
    } else {
        std::vfprintf(stdout, fmt, a);
        std::fflush(stdout);
    }
    va_end(a);
}

// what the kit says, kit.toml and android.toml
struct Kit {
    std::string title, xbe, sha1;
    std::vector<std::string> also_sha1;  // releases with the same code and another header
    fs::path config, xdk, recomp, cxx, sysroot, base_apk;
    int api = 0;
    std::vector<fs::path> include;
    std::vector<std::string> driver;  // the compiler's own first arguments (zig's "c++"), before every command
    std::vector<std::string> cxxflags, ldflags, libs;
    fs::path toolchain;                           // work/toolchain/android-<host>, cxx is in it
    std::string toolchain_url, toolchain_sha256;  // [toolchain.<host>]: the compiler's archive the builder downloads
};

// a list of strings, {kit} in each the folder the kit is unpacked in
std::vector<std::string> Strings(const toml::node_view<toml::node>& n, const fs::path& kit_dir) {
    std::vector<std::string> out;
    std::string kit = kit_dir.generic_string();
    if (auto* a = n.as_array())
        for (auto& e : *a)
            if (auto s = e.value<std::string>()) {
                std::string v = *s;
                for (size_t at; (at = v.find("{kit}")) != std::string::npos;) v.replace(at, 5, kit);
                out.push_back(v);
            }
    return out;
}

fs::path ExePath() {
#ifdef _WIN32
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW(nullptr, p.data(), DWORD(p.size())));
    return fs::path(p);
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}

fs::path ExeDir() { return ExePath().parent_path(); }

void SetEnv(const char* name, const fs::path& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(fs::path(name).c_str(), value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// A release carries its kit inside the builder: a zip of the kit's files appended to the executable, then a footer of
// kMagic and the zip's size as 16 decimal digits (tools/package-android.cmake), as Setup's has it with its own magic.
// The tomls are read from it as they are, and the rest is unpacked into the working folder when it builds, again only
// when the folder does not hold this kit already.
constexpr char kMagic[] = "XBEAPK01";
constexpr size_t kMagicSize = 8, kFooterSize = kMagicSize + 16;

std::vector<uint8_t> g_kit_zip;
std::string g_kit_stamp;  // the zip's checksum and size: what .payload holds in a folder it is unpacked in

// reads the kit out of the builder's executable; false only when there is one and it is damaged
bool LoadPayload(std::string& error) {
    std::ifstream in(ExePath(), std::ios::binary);
    in.seekg(0, std::ios::end);
    uint64_t end = uint64_t(std::max<std::streamoff>(in.tellg(), 0));
    char footer[kFooterSize];
    if (!in || end < kFooterSize) return true;
    in.seekg(std::streamoff(end - kFooterSize));
    in.read(footer, kFooterSize);
    if (!in || std::memcmp(footer, kMagic, kMagicSize) != 0) return true;
    uint64_t size = std::strtoull(std::string(footer + kMagicSize, 16).c_str(), nullptr, 10);
    if (size == 0 || size > end - kFooterSize) {
        error = "the kit inside the APK builder is damaged";
        return false;
    }
    g_kit_zip.resize(size);
    in.seekg(std::streamoff(end - kFooterSize - size));
    in.read(reinterpret_cast<char*>(g_kit_zip.data()), std::streamsize(size));
    if (!in) {
        g_kit_zip.clear();
        error = "cannot read the kit inside the APK builder";
        return false;
    }
    char stamp[64];
    unsigned long crc = static_cast<unsigned long>(mz_crc32(MZ_CRC32_INIT, g_kit_zip.data(), g_kit_zip.size()));
    std::snprintf(stamp, sizeof(stamp), "%08lx %llu", crc, static_cast<unsigned long long>(size));
    g_kit_stamp = stamp;
    return true;
}

const char* const kNoKit = "this APK builder has no kit inside (tools/package-android.cmake makes one that does)";

// a file of the kit, read from the zip in the builder
bool KitFile(const std::string& name, std::string& out, std::string& error) {
    mz_zip_archive z{};
    if (g_kit_zip.empty() || !mz_zip_reader_init_mem(&z, g_kit_zip.data(), g_kit_zip.size(), 0)) {
        error = kNoKit;
        return false;
    }
    size_t n = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&z, name.c_str(), &n, 0);
    mz_zip_reader_end(&z);
    if (!data) {
        error = "the kit inside the APK builder has no " + name;
        return false;
    }
    out.assign(static_cast<const char*>(data), n);
    mz_free(data);
    return true;
}

// the kit's own parts in the working folder, which may be a folder the player chose: only these are ever removed
constexpr const char* kKitDirs[] = {"bin", "config", "xdk", "include", "lib", "sysroot", "apk"};
constexpr const char* kKitFiles[] = {"kit.toml", "android.toml", ".payload"};

// the kit's files into dir, the working folder: one that holds this kit already is left as it is, one that holds
// another version of it is cleared of the kit's own parts first
bool UnpackKit(const fs::path& dir, std::string& error) {
    if (g_kit_zip.empty()) {
        error = kNoKit;
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string line;
    bool ours = false;
    {
        std::ifstream have(dir / ".payload");
        ours = have && std::getline(have, line);
    }
    if (ours && line == g_kit_stamp && fs::exists(dir / "kit.toml", ec)) return true;
    if (ours) {
        for (const char* d : kKitDirs) fs::remove_all(dir / d, ec);
        for (const char* f : kKitFiles) fs::remove(dir / f, ec);
    }

    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, g_kit_zip.data(), g_kit_zip.size(), 0)) {
        error = "cannot read the kit inside the APK builder";
        return false;
    }
    bool ok = true;
    for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&z); ok && i < count; i++) {
        mz_zip_archive_file_stat st;
        ok = mz_zip_reader_file_stat(&z, i, &st);
        if (!ok || st.m_is_directory) continue;
        fs::path rel = fs::path(st.m_filename).lexically_normal();
        if (rel.is_absolute() || std::find(rel.begin(), rel.end(), "..") != rel.end()) continue;  // not below dir
        fs::path out = dir / rel;
        fs::create_directories(out.parent_path(), ec);
        {
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            auto write = [](void* o, mz_uint64, const void* buf, size_t n) -> size_t {
                return static_cast<std::ofstream*>(o)->write(static_cast<const char*>(buf), std::streamsize(n)) ? n : 0;
            };
            ok = f && mz_zip_reader_extract_to_callback(&z, i, write, &f, 0) && f.flush();
        }
        if (!ok) {
            error = "cannot unpack " + out.string();
            break;
        }
#ifndef _WIN32
        fs::permissions(out, *rel.begin() == "bin" ? fs::perms(0755) : fs::perms(0644), ec);
#endif
    }
    mz_zip_reader_end(&z);
    if (!ok) {
        if (error.empty()) error = "cannot read the kit inside the APK builder";
        return false;
    }
    std::ofstream(dir / ".payload") << g_kit_stamp << "\n";
    return true;
}

bool ParseToml(const std::string& name, toml::table& t, std::string& error) {
    std::string text;
    if (!KitFile(name, text, error)) return false;
    try {
        t = toml::parse(text, name);
    } catch (const toml::parse_error& e) {
        error = name + ": " + std::string(e.description());
        return false;
    }
    return true;
}

// the kit; kit_dir is where its files are unpacked
bool LoadKit(const fs::path& kit_dir, Kit& k, std::string& error) {
    toml::table t, o;
    if (!ParseToml("kit.toml", t, error) || !ParseToml("android.toml", o, error)) return false;
    k.title = t["title"].value_or(std::string("the game"));
    k.xbe = t["xbe"].value_or(std::string());
    k.sha1 = t["sha1"].value_or(std::string());
    k.also_sha1 = Strings(t["also_sha1"], kit_dir);
    k.config = kit_dir / t["config"].value_or(std::string());
    k.xdk = kit_dir / t["xdk"].value_or(std::string());
    k.recomp = kit_dir / t["recomp"].value_or(std::string());
    for (auto& s : Strings(t["include"], kit_dir)) k.include.push_back(kit_dir / s);
    k.toolchain = kit_dir / "toolchain" / (std::string("android-") + kHost);
    k.cxx = k.toolchain / o["cxx"].value_or(std::string());
    k.sysroot = kit_dir / o["sysroot"].value_or(std::string("sysroot"));
    k.api = o["api"].value_or(0);
    k.base_apk = kit_dir / "apk" / "base.apk";
    k.driver = Strings(o["driver"], kit_dir);
    k.cxxflags = Strings(o["cxxflags"], kit_dir);
    k.ldflags = Strings(o["ldflags"], kit_dir);
    for (auto& s : Strings(o["libs"], kit_dir)) k.libs.push_back(s.rfind("-", 0) == 0 ? s : (kit_dir / s).generic_string());
    k.toolchain_url = o["toolchain"][kHost]["url"].value_or(std::string());
    k.toolchain_sha256 = o["toolchain"][kHost]["sha256"].value_or(std::string());
    if (k.xbe.empty() || k.sha1.size() != 40 || !k.api) {
        error = "xbe, sha1 and api must be set";
        return false;
    }
    if (!k.toolchain_url.empty() && k.toolchain_sha256.size() != 64) {
        error = std::string("android.toml: [toolchain.") + kHost + "] needs the sha256 of its url";
        return false;
    }
#ifdef _WIN32
    k.recomp += ".exe";
    k.cxx += ".exe";
#endif
    return true;
}

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool WriteFile(const fs::path& p, const void* data, size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    return f.write(static_cast<const char*>(data), std::streamsize(n)) && f.flush();
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    return s;
}

// the player's copy: its default.xbe, from wherever it is
enum class Kind { Missing, Files, Iso, Package };
struct Source {
    Kind kind = Kind::Missing;
    fs::path path;
    std::vector<uint8_t> xbe;        // checked
    std::vector<std::string> other;  // releases found that the kit was not made for
};

bool MatchXbe(std::vector<uint8_t>& data, const Kit& k, const fs::path& where, Source& s) {
    std::string sha = Sha1(data);
    if (sha != k.sha1 && std::find(k.also_sha1.begin(), k.also_sha1.end(), sha) == k.also_sha1.end()) {
        s.other.push_back(where.string() + " (sha1 " + sha + ")");
        return false;
    }
    s.xbe = std::move(data);
    return true;
}

bool TryFiles(const fs::path& dir, const Kit& k, Source& s) {
    std::vector<uint8_t> data;
    if (!ReadFile(dir / k.xbe, data) || !MatchXbe(data, k, dir / k.xbe, s)) return false;
    s.kind = Kind::Files;
    s.path = dir;
    return true;
}

bool TryIso(const fs::path& iso, const Kit& k, Source& s) {
    kernel::Disc disc;
    std::vector<uint8_t> data;
    std::string error;
    if (!disc.Open(iso, error)) {
        s.other.push_back(iso.string() + ": " + error);
        return false;
    }
    if (!disc.Load(k.xbe, data)) {
        s.other.push_back(iso.string() + " has no " + k.xbe);
        return false;
    }
    if (!MatchXbe(data, k, iso, s)) return false;
    s.kind = Kind::Iso;
    s.path = iso;
    return true;
}

bool TryPackage(const fs::path& given, const Kit& k, Source& s) {
    std::vector<uint8_t> data;
    std::string error;
    if (!store::LoadFile(given, k.xbe, data, error)) {
        s.other.push_back(given.string() + ": " + error);
        return false;
    }
    if (!MatchXbe(data, k, given, s)) return false;
    s.kind = Kind::Package;
    s.path = given;
    return true;
}

// what `given` is: a disc image, the xbe or a folder with it, a package (DefaultPackage, its .data folder, or the
// folder holding both), or a folder holding one of these (the files before images before a package, as the game
// looks)
Source Resolve(const fs::path& given, const Kit& k) {
    Source s;
    std::error_code ec;
    if (fs::is_regular_file(given, ec)) {
        std::string name = Lower(given.filename().string()), ext = Lower(given.extension().string());
        if (ext == ".iso") TryIso(given, k, s);
        else if (name == Lower(k.xbe)) TryFiles(given.parent_path(), k, s);
        else if (name == "defaultpackage" && store::HasPackage(given.parent_path())) TryPackage(given, k, s);
        return s;
    }
    if (!fs::is_directory(given, ec)) return s;
    if (TryFiles(given, k, s)) return s;
    std::vector<fs::path> isos;
    for (auto& e : fs::directory_iterator(given, ec))
        if (Lower(e.path().extension().string()) == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
    std::sort(isos.begin(), isos.end());
    for (auto& iso : isos)
        if (TryIso(iso, k, s)) return s;
    if (store::HasPackage(given)) TryPackage(given, k, s);
    else if (Lower(given.extension().string()) == ".data" && store::HasPackage(given.parent_path()))
        TryPackage(given.parent_path(), k, s);
    return s;
}

// child processes: one command each, output into a log file

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
// one argument as CommandLineToArgvW reads it back
std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
using Handle = HANDLE;
#else
using Handle = pid_t;
#endif

struct Child {
    Handle h{};
    size_t job = 0;
};

bool Spawn(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log, Child& c) {
#ifdef _WIN32
    std::wstring cmd = Quote(exe.wstring());
    for (auto& a : args) cmd += L" " + Quote(Wide(a));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
    CloseHandle(out);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    c.h = pi.hProcess;
    return true;
#else
    std::vector<std::string> all{exe.string()};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!log.empty()) {  // without one, the child writes to the terminal
        posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    int r = posix_spawn(&c.h, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    return r == 0;
#endif
}

// waits for one of the children; its index and exit code
size_t WaitAny(const std::vector<Child>& kids, int& code) {
#ifdef _WIN32
    std::vector<HANDLE> hs;
    for (auto& k : kids) hs.push_back(k.h);
    DWORD r = WaitForMultipleObjects(DWORD(hs.size()), hs.data(), FALSE, INFINITE);
    size_t i = r - WAIT_OBJECT_0;
    DWORD ec = 1;
    GetExitCodeProcess(hs[i], &ec);
    CloseHandle(hs[i]);
    code = int(ec);
    return i;
#else
    for (;;) {
        for (size_t i = 0; i < kids.size(); i++) {
            int status = 0;
            if (waitpid(kids[i].h, &status, WNOHANG) == kids[i].h) {
                code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
                return i;
            }
        }
        usleep(5000);
    }
#endif
}

void ShowLog(const fs::path& log, size_t max_lines = 30) {
    std::ifstream f(log);
    std::vector<std::string> lines;
    for (std::string s; std::getline(f, s);) lines.push_back(s);
    size_t from = lines.size() > max_lines ? lines.size() - max_lines : 0;
    for (size_t i = from; i < lines.size(); i++) Say("    %s\n", lines[i].c_str());
}

// runs one command to the end
bool Run(const char* what, const fs::path& exe, const std::vector<std::string>& args, const fs::path& log) {
    Child c;
    if (!Spawn(exe, args, log, c)) {
        Say("%s: cannot start %s\n", what, exe.string().c_str());
        return false;
    }
    int code = 0;
    WaitAny({c}, code);
    if (code && log.empty()) {
        Say("%s failed (exit code %d).\n", what, code);
        return false;
    }
    if (code) {
        Say("%s failed (exit code %d). The end of %s:\n", what, code, log.string().c_str());
        ShowLog(log);
        return false;
    }
    return true;
}

// the toolchain: the kit's android.toml pins the compiler's archive by URL and SHA-256, the builder downloads it into
// work/toolchain once

#ifdef _WIN32
// WinHTTP, which every Windows and Wine have (curl.exe is not in Wine)
bool Download(const std::string& url, const fs::path& to) {
    std::wstring wurl = Wide(url);
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof(u);
    u.dwHostNameLength = u.dwUrlPathLength = u.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) {
        Say("Cannot read the download address %s\n", url.c_str());
        return false;
    }
    std::wstring host(u.lpszHostName, u.dwHostNameLength), path(u.lpszUrlPath, u.dwUrlPathLength);
    if (u.lpszExtraInfo) path.append(u.lpszExtraInfo, u.dwExtraInfoLength);
    HINTERNET session = WinHttpOpen(L"APK builder", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host.c_str(), u.nPort, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    bool ok = req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    DWORD error = ok ? 0 : GetLastError(), status = 0, total = 0, size = sizeof(DWORD);
    if (ok)
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (ok && status == 200) {
        size = sizeof(DWORD);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);
        std::ofstream f(to, std::ios::binary | std::ios::trunc);
        std::vector<char> buf(1 << 20);
        uint64_t got = 0, shown = ~0ull;
        DWORD n = 0;
        while ((ok = f && WinHttpReadData(req, buf.data(), DWORD(buf.size()), &n)) && n) {
            ok = bool(f.write(buf.data(), n));
            got += n;
            if (got >> 20 != shown) {
                shown = got >> 20;
                if (total) Progress(0.05 + 0.15 * double(got) / double(total));
                if (total) Say("\r  %llu / %llu MB", (unsigned long long)shown, (unsigned long long)(total >> 20));
                else Say("\r  %llu MB", (unsigned long long)shown);
            }
        }
        if (!ok) error = GetLastError();
        ok = ok && f.flush() && (!total || got == total);
        Say("\n");
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    if (!ok || status != 200) {
        if (ok) Say("The download failed (the server answered %lu). Try again later.\n", (unsigned long)status);
        else Say("The download failed (error %lu). Check the internet connection and try again.\n",
                 (unsigned long)error);
        return false;
    }
    return true;
}
#else
// curl, which every Linux desktop and SteamOS have; its progress bar goes to the terminal
bool Download(const std::string& url, const fs::path& to) {
    return Run("Downloading", "/usr/bin/env",
               {"curl", "-fL", "--retry", "3", g_sink ? "-sS" : "-#", "-o", to.string(), url}, {});
}
#endif

// the zip's files below its top folder into dest
bool Unzip(const fs::path& zip, const fs::path& dest) {
    std::ifstream in(zip, std::ios::binary);
    std::error_code ec;
    uint64_t size = fs::file_size(zip, ec);
    mz_zip_archive z{};
    z.m_pIO_opaque = &in;
    z.m_pRead = [](void* io, mz_uint64 ofs, void* buf, size_t n) -> size_t {
        auto& s = *static_cast<std::ifstream*>(io);
        s.clear();
        s.seekg(std::streamoff(ofs));
        s.read(static_cast<char*>(buf), std::streamsize(n));
        return size_t(s.gcount());
    };
    if (!in || ec || !mz_zip_reader_init(&z, size, 0)) {
        Say("Cannot read %s as a zip.\n", zip.string().c_str());
        return false;
    }
    bool ok = true;
    size_t files = 0;
    for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&z); ok && i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) {
            ok = false;
            break;
        }
        std::string name = st.m_filename;
        size_t slash = name.find('/');
        if (slash == std::string::npos) continue;
        name.erase(0, slash + 1);
        fs::path rel = fs::path(name).lexically_normal();
        if (rel.is_absolute() || std::find(rel.begin(), rel.end(), "..") != rel.end()) continue;  // not below dest
        fs::path out = dest / rel;
        if (st.m_is_directory) {
            fs::create_directories(out, ec);
            continue;
        }
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        auto write = [](void* o, mz_uint64, const void* buf, size_t n) -> size_t {
            return static_cast<std::ofstream*>(o)->write(static_cast<const char*>(buf), std::streamsize(n)) ? n : 0;
        };
        ok = f && mz_zip_reader_extract_to_callback(&z, i, write, &f, 0) && f.flush();
        if (!ok) Say("\nCannot unpack %s\n", out.string().c_str());
        if (++files % 100 == 0) Say("\r  %zu files", files);
    }
    Say("\r  %zu files\n", files);
    mz_zip_reader_end(&z);
    return ok;
}

// a .zip (the Windows-hosted builds) with miniz; a .tar.xz (the Linux-hosted ones) with the system tar
bool Unpack(const fs::path& archive, const std::string& url, const fs::path& dest, [[maybe_unused]] const fs::path& log) {
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0) return Unzip(archive, dest);
#ifdef _WIN32
    Say("Cannot unpack %s: the APK builder on Windows reads only .zip\n", url.c_str());
    return false;
#else
    std::error_code ec;
    fs::create_directories(dest, ec);
    return Run("Unpacking", "/usr/bin/env",
               {"tar", "-xJf", archive.string(), "-C", dest.string(), "--strip-components=1"}, log);
#endif
}

// work/toolchain/android-<host> holds the archive android.toml pins for this host once it is downloaded, checked and
// unpacked; sha256.txt in it says which
bool EnsureToolchain(const fs::path& kit_dir, const Kit& k) {
    fs::path tc = k.toolchain, stamp = tc / "sha256.txt";
    std::error_code ec;
    std::vector<uint8_t> have;
    if (fs::exists(k.cxx, ec) && (k.toolchain_url.empty() || (ReadFile(stamp, have) &&
                                                              std::string(have.begin(), have.end()) == k.toolchain_sha256)))
        return true;
    if (k.toolchain_url.empty()) {
        Say("The tools for building the game are missing (%s) and this builder has no address to download them from.\n",
            k.cxx.string().c_str());
        return false;
    }
    fs::path archive = tc.string() + ".download", fresh = tc.string() + ".new";
    fs::create_directories(tc.parent_path(), ec);
    fs::remove(archive, ec);
    fs::remove_all(fresh, ec);
    Say("Downloading the tools needed to build the game (about 100 MB, only the first time):\n  %s\n",
        k.toolchain_url.c_str());
    if (!Download(k.toolchain_url, archive)) {
        fs::remove(archive, ec);
        return false;
    }
    std::string sha = apk::Sha256File(archive);
    if (sha != k.toolchain_sha256) {
        Say("The downloaded file is not the one this builder expects (checksum %s, expected %s).\n"
            "Try again; if this keeps happening, please report it.\n",
            sha.c_str(), k.toolchain_sha256.c_str());
        fs::remove(archive, ec);
        return false;
    }
    Say("Unpacking the downloaded tools ...\n");
    bool ok = Unpack(archive, k.toolchain_url, fresh, kit_dir / "unpack.log") &&
              WriteFile(fresh / "sha256.txt", sha.data(), sha.size());
    fs::remove(archive, ec);
    if (ok) {
        fs::remove_all(tc, ec);
        fs::rename(fresh, tc, ec);
        ok = !ec;
    }
    if (!ok || !fs::exists(k.cxx, ec)) {
        Say("Could not unpack the downloaded tools into %s\n", tc.string().c_str());
        fs::remove_all(fresh, ec);
        return false;
    }
    return true;
}

// Zig takes where Android's C library is from a file: the kit's sysroot, the API level's link stubs
bool WriteLibc(const fs::path& file, const Kit& k) {
    std::string inc = (k.sysroot / "usr" / "include").generic_string();
    std::string text = "include_dir=" + inc + "\nsys_include_dir=" + inc + "/aarch64-linux-android\ncrt_dir=" +
                       (k.sysroot / "usr" / "lib" / "aarch64-linux-android" / std::to_string(k.api)).generic_string() +
                       "\nmsvc_lib_dir=\nkernel32_lib_dir=\ngcc_dir=\n";
    return WriteFile(file, text.data(), text.size());
}

// the user's settings folder: %APPDATA% or $XDG_CONFIG_HOME (~/.config), each with LiveAndRecompiled, as the game has it
fs::path SettingsDir(const fs::path& fallback) {
#ifdef _WIN32
    if (const wchar_t* a = _wgetenv(L"APPDATA"); a && *a) return fs::path(a) / "LiveAndRecompiled";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x == '/') return fs::path(x) / "LiveAndRecompiled";
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".config" / "LiveAndRecompiled";
#endif
    return fallback;
}

// double-clicked: the console would close before the player reads it
void Pause() {
#ifdef _WIN32
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) > 1) return;
    std::printf("\nPress Enter to close.");
    std::fflush(stdout);
    std::getchar();
#endif
}

bool SayCancelled() {
    if (!g_cancel) return false;
    Say("\nCancelled.\n");
    return true;
}

}  // namespace

namespace apkbuilder {

void SetSink(void (*sink)(const char*)) { g_sink = sink; }
void SetProgress(void (*progress)(double)) { g_progress = progress; }
void Cancel() { g_cancel = true; }
bool Cancelled() { return g_cancel; }

KitInfo ReadKit() {
    KitInfo info;
    toml::table t, o;
    if (!ParseToml("kit.toml", t, info.error)) return info;
    info.title = t["title"].value_or(std::string("the game"));
    ParseToml("android.toml", o, info.error);
    return info;
}

Options Defaults(const fs::path& root) {
    Options o;
    o.root = root;
    o.jobs = std::max(1u, std::thread::hardware_concurrency());
    o.work = root / "work";
    o.out_dir = root;
    o.key = SettingsDir(root) / "android-signing.toml";
    return o;
}

Probe CheckSource(const Options& opt) {
    Probe p;
    Kit k;
    std::string error;
    if (!LoadKit(opt.work, k, error)) {
        p.text = error;
        return p;
    }
    std::error_code ec;
    if (opt.source.empty()) {
        p.text = "Pick your copy of the game.";
        return p;
    }
    if (!fs::exists(opt.source, ec)) {
        p.text = "Nothing found there.";
        return p;
    }
    Source s = Resolve(opt.source, k);
    switch (s.kind) {
        case Kind::Files: p.text = "Found the game's files. This is the right version."; break;
        case Kind::Iso: p.text = "Found a disc image. This is the right version."; break;
        case Kind::Package: p.text = "Found the digital release. This is the right version."; break;
        default:
            if (s.other.empty()) p.text = "This isn't the game. Choose the DefaultPackage file, a disc image (.iso) or the folder with " + k.xbe + " in it.";
            else p.text = "This is a different version of the game than this builder supports (" + s.other[0] + ").";
            return p;
    }
    p.ok = true;
    return p;
}

bool Build(const Options& opt) {
    g_cancel = false;
    g_done = 0;
    Progress(0);
    auto start = std::chrono::steady_clock::now();
    unsigned jobs = opt.jobs;
#ifdef _WIN32
    jobs = std::min(jobs, unsigned(MAXIMUM_WAIT_OBJECTS));
#endif
    std::error_code ec;
    auto fail = [&]() {
        Say("\nThe APK could not be built.\n");
        return false;
    };

    // the working folder holds the kit, the compiler and, while building, the generated code and objects
    fs::path work = opt.work.empty() ? opt.root / "work" : opt.work;
    Kit k;
    std::string error;
    if (!LoadKit(work, k, error) || !UnpackKit(work, error)) {
        Say("%s\n", error.c_str());
        return fail();
    }
    if (!opt.toolchain.empty()) k.cxx = opt.toolchain / k.cxx.lexically_relative(k.toolchain);
    fs::path out_dir = opt.out_dir.empty() ? opt.root : opt.out_dir;
    fs::create_directories(out_dir, ec);
    // Zig's caches: the C++ library it builds for Android once beside the downloaded compiler (with --toolchain, in the
    // work folder like the rest)
    fs::path zig_cache = opt.toolchain.empty() ? k.toolchain / "cache" : work / "zig-cache";
    SetEnv("ZIG_GLOBAL_CACHE_DIR", zig_cache);
    SetEnv("ZIG_LOCAL_CACHE_DIR", work / "zig-cache");
    Say("Building %s for Android. This takes a few minutes.\n\n", k.title.c_str());

    // 1. the game's default.xbe
    Source src = Resolve(opt.source, k);
    if (src.kind == Kind::Missing) {
        if (opt.source.empty()) {
            Say("Say where your copy of the game is with --source: the DefaultPackage file, a disc image (.iso) or the "
                "folder with %s in it.\n", k.xbe.c_str());
        } else if (src.other.empty()) {
            Say("There is no game at %s. Choose the DefaultPackage file, a disc image (.iso) or the folder with %s in "
                "it.\n", opt.source.string().c_str(), k.xbe.c_str());
        } else {
            Say("This builder works with one version of %s, and your copy is a different one. What it found:\n",
                k.title.c_str());
            for (auto& o : src.other) Say("  %s\n", o.c_str());
        }
        return fail();
    }
    Say("Using your game at: %s\n", src.path.string().c_str());
    Progress(0.03);
    if (SayCancelled()) return false;
    if (opt.toolchain.empty() && !EnsureToolchain(work, k)) return fail();
    Progress(0.2);
    if (SayCancelled()) return false;

    // the work folder's own parts only: it may be a folder the player chose
    fs::path gen = work / "generated", obj = work / "obj", logs = work / "logs", so = work / "libmain.so";
    auto clear = [&]() {
        for (const char* d : {"generated", "obj", "logs", "zig-cache"}) fs::remove_all(work / d, ec);
        for (const fs::path& f : {work / "link.rsp", work / "libc.txt", so, work / k.xbe}) fs::remove(f, ec);
        fs::remove(work, ec);  // only when it is empty now
    };
    clear();
    // what a finished (or cancelled) build leaves in the work folder, as the player chose: the unpacked kit and the
    // generated code with it, and the compiler
    auto finish = [&]() {
        if (!opt.keep) {
            clear();
            for (const char* d : kKitDirs) fs::remove_all(work / d, ec);
            for (const char* f : kKitFiles) fs::remove(work / f, ec);
            for (const char* f : {"download.log", "unpack.log"}) fs::remove(work / f, ec);
        }
        if (!opt.keep_compiler) fs::remove_all(work / "toolchain", ec);
        fs::remove(work, ec);  // only when it is empty now
    };
    fs::create_directories(gen, ec);
    fs::create_directories(obj, ec);
    fs::create_directories(logs, ec);
    if (ec) {
        Say("Could not create the folder %s: %s\n", work.string().c_str(), ec.message().c_str());
        return fail();
    }
    fs::path xbe_path = work / k.xbe;
    if (!WriteFile(xbe_path, src.xbe.data(), src.xbe.size()) || !WriteLibc(work / "libc.txt", k)) {
        Say("Could not write into %s\n", work.string().c_str());
        return fail();
    }
    SetEnv("ZIG_LIBC", work / "libc.txt");

    // 2. recompile
    Say("Step 1 of 4: converting the game's code ...\n");
    if (!Run("Recompiling", k.recomp,
             {k.config.string(), xbe_path.string(), gen.string(), "--xdk", k.xdk.string()}, logs / "recompile.log"))
        return fail();
    // the host library was built against these addresses; another set would call the wrong functions
    std::vector<uint8_t> made, expected;
    for (auto& inc : k.include)
        if (ReadFile(inc / "image_symbols.h", expected)) break;
    if (!ReadFile(gen / "image_symbols.h", made) || made != expected) {
        Say("The converted code does not match what this builder expects. Delete the working folder and try again.\n");
        return fail();
    }

    // 3. compile, every file on its own
    std::vector<fs::path> sources;
    for (auto& e : fs::directory_iterator(gen, ec))
        if (e.path().extension() == ".cpp") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end(), [](const fs::path& a, const fs::path& b) {
        return fs::file_size(a) > fs::file_size(b);  // the big ones first, so the last to finish is short
    });
    Progress(0.25);
    Say("Step 2 of 4: building the game for Android (%zu parts, %u at a time) ...\n", sources.size(), jobs);
    std::vector<std::string> base = k.cxxflags;
    for (auto& inc : k.include) base.push_back("-I" + inc.generic_string());
    base.push_back("-I" + gen.generic_string());
    std::vector<Child> running;
    std::vector<fs::path> objects(sources.size());
    size_t next = 0, done = 0;
    bool failed = false;
    while (done < sources.size() && !(failed && running.empty())) {
        if (g_cancel) failed = true;
        while (!failed && next < sources.size() && running.size() < jobs) {
            size_t i = next++;
            objects[i] = obj / (sources[i].stem().string() + ".o");
            std::vector<std::string> args = k.driver;
            args.insert(args.end(), base.begin(), base.end());
            args.insert(args.end(), {"-c", sources[i].generic_string(), "-o", objects[i].generic_string()});
            Child c;
            c.job = i;
            if (!Spawn(k.cxx, args, logs / (sources[i].stem().string() + ".log"), c)) {
                Say("Could not start the build tools at %s\n", k.cxx.string().c_str());
                failed = true;
                break;
            }
            running.push_back(c);
        }
        if (running.empty()) break;
        int code = 0;
        size_t r = WaitAny(running, code);
        size_t job = running[r].job;
        running.erase(running.begin() + std::ptrdiff_t(r));
        done++;
        if (code) {
            if (!failed) {
                Say("Building %s failed:\n", sources[job].filename().string().c_str());
                ShowLog(logs / (sources[job].stem().string() + ".log"));
            }
            failed = true;
            continue;
        }
        Say("\r  %zu / %zu", done, sources.size());
        Progress(0.25 + 0.65 * double(done) / double(sources.size()));
    }
    Say("\n");
    if (g_cancel) {
        SayCancelled();
        finish();
        return false;
    }
    if (failed) return fail();

    // 4. link libmain.so; a response file keeps the command line short
    Say("Step 3 of 4: putting libmain.so together (the first time, the C++ library is built for Android too) ...\n");
    fs::path rsp = work / "link.rsp";
    {
        std::ofstream f(rsp);
        auto put = [&](const std::string& s) { f << '"' << s << "\"\n"; };
        // in the order the project's own build links them (the mapping, then the partitions by number), so every
        // player's library has the layout of the developers' and a crash address means the same in both
        auto rank = [](const fs::path& o) {
            std::string s = o.stem().string();
            size_t dot = s.rfind('.');
            return dot == std::string::npos ? -1 : std::atoi(s.c_str() + dot + 1);
        };
        std::vector<fs::path> order = objects;
        std::sort(order.begin(), order.end(), [&](const fs::path& a, const fs::path& b) { return rank(a) < rank(b); });
        for (auto& o : order) put(o.generic_string());
        for (auto& s : k.ldflags) put(s);
        for (auto& s : k.libs) put(s);
        put("-o");
        put(so.generic_string());
    }
    std::vector<std::string> link = k.driver;
    link.push_back("@" + rsp.generic_string());
    Progress(0.92);
    if (!Run("Linking", k.cxx, link, logs / "link.log")) return fail();

    // 5. the APK, signed with the player's key
    Say("Step 4 of 4: making the APK and signing it ...\n");
    apk::Key key;
    fs::path key_path = opt.key.empty() ? SettingsDir(opt.root) / "android-signing.toml" : opt.key;
    bool new_key = !fs::exists(key_path, ec);
    fs::path apk_path = out_dir / apkbuilder::kApkName;
    if (!apk::LoadOrMakeKey(key_path, key, error) || !apk::Build(k.base_apk, so, apk_path, key, error)) {
        Say("%s\n", error.c_str());
        return fail();
    }
    if (new_key) Say("  A signing key was made for this PC: %s\n  Keep it to install later builds over this one.\n",
                     key_path.string().c_str());

    // the generated code and objects are the title's code: only kept when asked for
    finish();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Progress(1);
    Say("\nDone in %.0f seconds. The app is ready:\n  %s\n\n"
        "Install it on your device (copy it there and open it, or `adb install`), then copy your game (the disc image, "
        "or the folder with the extracted files or the Xbox Store package) to the device. The app asks for it the "
        "first time it starts.\n",
        secs, apk_path.string().c_str());
    return true;
}

}  // namespace apkbuilder

int main(int argc, char** argv) {
    fs::path root = ExeDir();
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], "--root") == 0) root = fs::absolute(argv[i + 1]);
    {
        std::string error;
        if (!LoadPayload(error)) {
            std::fprintf(stderr, "apk-builder: %s\n", error.c_str());
            return 1;
        }
    }
    apkbuilder::Options opt = apkbuilder::Defaults(root);
    bool cli = argc > 1, gui = false;
    const char* usage = "usage: apk-builder [--cli] [--source GAME] [--work DIR] [--out DIR] [--key FILE] [--jobs N] "
                        "[--keep] [--delete-compiler] [--toolchain DIR] [--root DIR]\n";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool more = i + 1 < argc;
        if (a == "--jobs" && more) opt.jobs = unsigned(std::max(1, std::atoi(argv[++i])));
        else if (a == "--keep") opt.keep = true;
        else if (a == "--delete-compiler") opt.keep_compiler = false;
        else if (a == "--cli") cli = true;
        else if (a == "--gui") gui = true;
        else if (a == "--source" && more) opt.source = fs::absolute(argv[++i]);
        else if (a == "--work" && more) opt.work = fs::absolute(argv[++i]);
        else if (a == "--out" && more) opt.out_dir = fs::absolute(argv[++i]);
        else if (a == "--key" && more) opt.key = fs::absolute(argv[++i]);
        else if (a == "--toolchain" && more) opt.toolchain = fs::absolute(argv[++i]);
        else if (a == "--root" && more) i++;
        else if (a == "--help" || a == "-h") {
            std::printf("%s  no options: a window with the same choices\n", usage);
            return 0;
        } else {
            std::fprintf(stderr, "%s", usage);
            return 2;
        }
    }
    if (gui || (!cli && apkbuilder::WindowAvailable())) {
#ifdef _WIN32
        DWORD ids[2];
        if (GetConsoleProcessList(ids, 2) <= 1) FreeConsole();  // double-clicked: no console behind the window
#endif
        return apkbuilder::RunWindow(opt);
    }
    bool ok = apkbuilder::Build(opt);
    Pause();
    return ok ? 0 : 1;
}
