// Setup: builds the game on the player's machine from the player's own copy, so no release ever carries the
// title's code. A release is this program, a kit (kit/: the recompiler, the prebuilt host library with everything
// but the title's code, the headers the generated code includes, the config) and an empty assets folder.
// Setup finds the game in assets (its files or a disc image), checks it is the release the kit was made for,
// downloads the compiler kit.toml pins for this host into kit/toolchain/<host> (once), recompiles the game, compiles
// the result with that compiler and links <exe>.exe beside itself. It runs on Windows and on Linux (the exe is for
// Windows either way, Linux players start it with Proton).
//
//   setup-windows.exe, setup-linux [--jobs N] [--keep] [--toolchain DIR] [--root DIR] [--fetch-toolchain]
//
// --keep leaves kit/work (the generated code and objects) in place; --toolchain uses another llvm-mingw of the
// kit's version (a Linux-hosted one tests a kit without Windows); --root is the folder Setup would be in;
// --fetch-toolchain only downloads the compiler.
#include <miniz.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

#include "kernel/crypto.h"
#include "kernel/disc.h"

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHost = "windows";
#else
constexpr const char* kHost = "linux";
#endif

FILE* g_log = nullptr;  // setup.log beside Setup, everything printed goes there too

void Say(const char* fmt, ...) {
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    std::vfprintf(stdout, fmt, a);
    std::fflush(stdout);
    if (g_log) {
        std::vfprintf(g_log, fmt, b);
        std::fflush(g_log);
    }
    va_end(b);
    va_end(a);
}

// what the kit says, kit/kit.toml
struct Kit {
    std::string title, exe, xbe, sha1;
    fs::path config, xdk, recomp, cxx;
    std::vector<fs::path> include;
    std::vector<std::string> cxxflags, ldflags, libs;
    fs::path toolchain;                           // kit/toolchain/<host>, cxx is in it
    std::string toolchain_url, toolchain_sha256;  // [toolchain.<host>]: the llvm-mingw archive Setup downloads
};

std::vector<std::string> Strings(const toml::node_view<toml::node>& n) {
    std::vector<std::string> out;
    if (auto* a = n.as_array())
        for (auto& e : *a)
            if (auto s = e.value<std::string>()) out.push_back(*s);
    return out;
}

bool LoadKit(const fs::path& kit_dir, Kit& k, std::string& error) {
    toml::table t;
    try {
        t = toml::parse_file((kit_dir / "kit.toml").string());
    } catch (const toml::parse_error& e) {
        error = std::string(e.description());
        return false;
    }
    k.title = t["title"].value_or(std::string("the game"));
    k.exe = t["exe"].value_or(std::string());
    k.xbe = t["xbe"].value_or(std::string());
    k.sha1 = t["sha1"].value_or(std::string());
    k.config = kit_dir / t["config"].value_or(std::string());
    k.xdk = kit_dir / t["xdk"].value_or(std::string());
    k.recomp = kit_dir / t["recomp"].value_or(std::string());
    k.toolchain = kit_dir / "toolchain" / kHost;
    k.cxx = k.toolchain / t["cxx"].value_or(std::string());
    for (auto& s : Strings(t["include"])) k.include.push_back(kit_dir / s);
    k.cxxflags = Strings(t["cxxflags"]);
    k.ldflags = Strings(t["ldflags"]);
    for (auto& s : Strings(t["libs"])) k.libs.push_back(s.rfind("-", 0) == 0 ? s : (kit_dir / s).generic_string());
    k.toolchain_url = t["toolchain"][kHost]["url"].value_or(std::string());
    k.toolchain_sha256 = t["toolchain"][kHost]["sha256"].value_or(std::string());
    if (k.exe.empty() || k.xbe.empty() || k.sha1.size() != 40) {
        error = "exe, xbe and sha1 must be set";
        return false;
    }
    if (!k.toolchain_url.empty() && k.toolchain_sha256.size() != 64) {
        error = std::string("[toolchain.") + kHost + "] needs the sha256 of its url";
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

// the game in assets: beside Setup or in a folder above (as the game looks), its files or a disc image
bool FindXbe(const fs::path& root, const Kit& k, std::vector<uint8_t>& xbe) {
    fs::path assets;
    std::error_code ec;
    for (fs::path dir = root; !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_directory(dir / "assets", ec)) {
            assets = dir / "assets";
            break;
        }
        if (dir.parent_path() == dir) break;
    }
    if (assets.empty()) {
        Say("No assets folder found beside %s.\n", root.string().c_str());
        return false;
    }
    std::vector<std::string> other;  // releases found that the kit was not made for
    std::vector<uint8_t> data;
    if (ReadFile(assets / k.xbe, data)) {
        std::string sha = Sha1(data);
        if (sha == k.sha1) {
            Say("Found %s in %s\n", k.xbe.c_str(), assets.string().c_str());
            xbe = std::move(data);
            return true;
        }
        other.push_back((assets / k.xbe).string() + " (sha1 " + sha + ")");
    }
    std::vector<fs::path> isos;
    for (auto& e : fs::directory_iterator(assets, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
        if (ext == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
    }
    std::sort(isos.begin(), isos.end());
    for (auto& iso : isos) {
        kernel::Disc disc;
        std::string error;
        if (!disc.Open(iso, error)) {
            Say("  %s: %s\n", iso.filename().string().c_str(), error.c_str());
            continue;
        }
        if (!disc.Load(k.xbe, data)) {
            Say("  %s has no %s\n", iso.filename().string().c_str(), k.xbe.c_str());
            continue;
        }
        std::string sha = Sha1(data);
        if (sha == k.sha1) {
            Say("Found %s in %s\n", k.xbe.c_str(), iso.string().c_str());
            xbe = std::move(data);
            return true;
        }
        other.push_back(iso.string() + " (sha1 " + sha + ")");
    }
    if (other.empty()) {
        Say("Put the game in %s first: its disc image (.iso), or %s with its folders beside it.\n",
            assets.string().c_str(), k.xbe.c_str());
    } else {
        Say("This build is made for one release of %s (%s sha1 %s). Found only:\n", k.title.c_str(), k.xbe.c_str(),
            k.sha1.c_str());
        for (auto& o : other) Say("  %s\n", o.c_str());
    }
    return false;
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
        int status = 0;
        pid_t p = waitpid(-1, &status, 0);
        for (size_t i = 0; i < kids.size(); i++)
            if (kids[i].h == p) {
                code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
                return i;
            }
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

// the toolchain: kit.toml pins an llvm-mingw zip by URL and SHA-256, Setup downloads it into kit/toolchain once

std::string Sha256File(const fs::path& p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* b) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], c1 = h[1], c2 = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & c1) ^ (a & c2) ^ (c1 & c2));
            hh = g, g = f, f = e, e = d + t1, d = c2, c2 = c1, c1 = a, a = t1 + t2;
        }
        h[0] += a, h[1] += c1, h[2] += c2, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    };
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::vector<uint8_t> buf(1 << 20), tail;  // a multiple of 64: only the last read leaves a partial block
    uint64_t total = 0;
    for (;;) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        size_t n = size_t(in.gcount()), whole = n / 64 * 64;
        total += n;
        for (size_t i = 0; i < whole; i += 64) block(buf.data() + i);
        if (whole < n || !n) {
            tail.assign(buf.begin() + std::ptrdiff_t(whole), buf.begin() + std::ptrdiff_t(n));
            break;
        }
    }
    tail.push_back(0x80);
    while (tail.size() % 64 != 56) tail.push_back(0);
    for (int i = 7; i >= 0; i--) tail.push_back(uint8_t((total * 8) >> (i * 8)));
    for (size_t i = 0; i < tail.size(); i += 64) block(tail.data() + i);
    std::string hex;
    for (uint32_t v : h)
        for (int i = 28; i >= 0; i -= 4) hex += "0123456789abcdef"[(v >> i) & 15];
    return hex;
}

#ifdef _WIN32
// WinHTTP, which every Windows and Wine have (curl.exe is not in Wine)
bool Download(const std::string& url, const fs::path& to, const fs::path&) {
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
    HINTERNET session =
        WinHttpOpen(L"Setup", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host.c_str(), u.nPort, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    // redirects (GitHub sends one to its file host) are followed by default
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
        if (ok) Say("The download failed: the server answered %lu.\n", (unsigned long)status);
        else Say("The download failed (error %lu). Check the internet connection and run Setup again.\n",
                 (unsigned long)error);
        return false;
    }
    return true;
}
#else
// curl, which every Linux desktop and SteamOS have; its progress bar goes to the terminal
bool Download(const std::string& url, const fs::path& to, const fs::path&) {
    return Run("Downloading", "/usr/bin/env", {"curl", "-fL", "--retry", "3", "-#", "-o", to.string(), url}, {});
}
#endif

// what a Windows x86_64 build needs of an llvm-mingw, by path below its top folder: the compiler and linker (bin, and
// the LLVM libraries a Linux-hosted one loads), the headers without their .idl sources (a Linux-hosted one keeps them in
// generic-w64-mingw32), clang's headers and x86_64
// builtins, the x86_64 libraries
bool Wanted(const std::string& name) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".idl") == 0) return false;
    if (name.rfind("lib/clang/", 0) == 0) {  // lib/clang/<version>/...
        size_t v = name.find('/', 10);
        std::string sub = v == std::string::npos ? std::string() : name.substr(v + 1);
        return sub.rfind("include/", 0) == 0 || sub == "lib/windows/libclang_rt.builtins-x86_64.a";
    }
    static const char* const kKeep[] = {"bin/",        "include/",         "x86_64-w64-mingw32/", "generic-w64-mingw32/",
                                        "lib/libLLVM", "lib/libclang-cpp", "LICENSE.TXT"};
    return std::any_of(std::begin(kKeep), std::end(kKeep), [&](const char* k) { return name.rfind(k, 0) == 0; });
}

// the zip's wanted files below its top folder into dest
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
        if (!Wanted(name)) continue;
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

#ifndef _WIN32
// the files Wanted does not keep, then the folders left empty
void Prune(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> drop, dirs;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) dirs.push_back(it->path());
        else if (!Wanted(it->path().lexically_relative(dir).generic_string())) drop.push_back(it->path());
    }
    for (auto& f : drop) fs::remove(f, ec);
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& x, const fs::path& y) {
        return x.native().size() > y.native().size();  // the deepest first
    });
    for (auto& d : dirs) fs::remove(d, ec);  // only removes it when empty
}
#endif

// a .zip (the Windows-hosted builds) with miniz; a .tar.xz (the Linux-hosted ones) with the system tar
bool Unpack(const fs::path& archive, const std::string& url, const fs::path& dest,
            [[maybe_unused]] const fs::path& log) {
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0) return Unzip(archive, dest);
#ifdef _WIN32
    Say("Cannot unpack %s: Setup on Windows reads only .zip\n", url.c_str());
    return false;
#else
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (!Run("Unpacking", "/usr/bin/env",
             {"tar", "-xJf", archive.string(), "-C", dest.string(), "--strip-components=1"}, log))
        return false;
    Prune(dest);
    return true;
#endif
}

// kit/toolchain/<host> holds the archive kit.toml pins for this host once it is downloaded, checked and unpacked;
// sha256.txt in it says which
bool EnsureToolchain(const fs::path& kit_dir, const Kit& k) {
    fs::path tc = k.toolchain, stamp = tc / "sha256.txt";
    std::error_code ec;
    std::vector<uint8_t> have;
    if (fs::exists(k.cxx, ec) && (k.toolchain_url.empty() || (ReadFile(stamp, have) &&
                                                              std::string(have.begin(), have.end()) == k.toolchain_sha256)))
        return true;
    if (k.toolchain_url.empty()) {
        Say("This kit has no compiler for %s: %s is missing and kit.toml names none to download.\n", kHost,
            k.cxx.string().c_str());
        return false;
    }
    fs::path archive = tc.string() + ".download", fresh = tc.string() + ".new";
    fs::create_directories(tc.parent_path(), ec);
    fs::remove(archive, ec);
    fs::remove_all(fresh, ec);
    Say("Downloading the compiler (llvm-mingw), once:\n  %s\n", k.toolchain_url.c_str());
    if (!Download(k.toolchain_url, archive, kit_dir / "download.log")) {
        fs::remove(archive, ec);
        return false;
    }
    std::string sha = Sha256File(archive);
    if (sha != k.toolchain_sha256) {
        Say("The download is not the compiler this kit was made with (sha256 %s, expected %s).\n"
            "Run Setup again; if this keeps happening, the address in kit/kit.toml is wrong.\n",
            sha.c_str(), k.toolchain_sha256.c_str());
        fs::remove(archive, ec);
        return false;
    }
    Say("Unpacking the compiler ...\n");
    bool ok = Unpack(archive, k.toolchain_url, fresh, kit_dir / "unpack.log") &&
              WriteFile(fresh / "sha256.txt", sha.data(), sha.size());
    fs::remove(archive, ec);
    if (ok) {
        fs::remove_all(tc, ec);
        fs::rename(fresh, tc, ec);
        ok = !ec;
    }
    if (!ok || !fs::exists(k.cxx, ec)) {
        Say("Cannot set up the compiler in %s\n", tc.string().c_str());
        fs::remove_all(fresh, ec);
        return false;
    }
    return true;
}

fs::path ExeDir() {
#ifdef _WIN32
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW(nullptr, p.data(), DWORD(p.size())));
    return fs::path(p).parent_path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
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

int Fail() {
    Say("\nSetup did not finish. setup.log has the whole run.\n");
    Pause();
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = ExeDir(), toolchain;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    bool keep = false, fetch_only = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--jobs" && i + 1 < argc) jobs = unsigned(std::max(1, std::atoi(argv[++i])));
        else if (a == "--keep") keep = true;
        else if (a == "--fetch-toolchain") fetch_only = true;
        else if (a == "--toolchain" && i + 1 < argc) toolchain = fs::absolute(argv[++i]);
        else if (a == "--root" && i + 1 < argc) root = fs::absolute(argv[++i]);
        else {
            std::fprintf(stderr, "usage: setup [--jobs N] [--keep] [--toolchain DIR] [--root DIR] [--fetch-toolchain]\n");
            return 2;
        }
    }
#ifdef _WIN32
    jobs = std::min(jobs, unsigned(MAXIMUM_WAIT_OBJECTS));
#endif
    auto start = std::chrono::steady_clock::now();
    g_log = std::fopen((root / "setup.log").string().c_str(), "w");

    fs::path kit_dir = root / "kit";
    Kit k;
    std::string error;
    if (!LoadKit(kit_dir, k, error)) {
        Say("%s: %s\n", (kit_dir / "kit.toml").string().c_str(), error.c_str());
        return Fail();
    }
    if (!toolchain.empty()) k.cxx = toolchain / "bin" / k.cxx.filename();
    if (fetch_only) {
        if (!EnsureToolchain(kit_dir, k)) return Fail();
        Say("The compiler is in %s\n", k.cxx.parent_path().parent_path().string().c_str());
        if (g_log) std::fclose(g_log);
        return 0;
    }
    Say("Building %s from your copy of the game. This takes a few minutes, once.\n\n", k.title.c_str());

    // 1. the game
    std::vector<uint8_t> xbe;
    if (!FindXbe(root, k, xbe)) return Fail();
    if (toolchain.empty() && !EnsureToolchain(kit_dir, k)) return Fail();

    fs::path work = kit_dir / "work", gen = work / "generated", obj = work / "obj", logs = work / "logs";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(gen);
    fs::create_directories(obj);
    fs::create_directories(logs);
    fs::path xbe_path = work / k.xbe;
    if (!WriteFile(xbe_path, xbe.data(), xbe.size())) {
        Say("Cannot write %s\n", xbe_path.string().c_str());
        return Fail();
    }

    // 2. recompile
    Say("Recompiling %s ...\n", k.xbe.c_str());
    if (!Run("Recompiling", k.recomp,
             {k.config.string(), xbe_path.string(), gen.string(), "--xdk", k.xdk.string()}, logs / "recompile.log"))
        return Fail();
    // the host library was built against these addresses; another set would call the wrong functions
    std::vector<uint8_t> made, expected;
    for (auto& inc : k.include)
        if (ReadFile(inc / "image_symbols.h", expected)) break;
    if (!ReadFile(gen / "image_symbols.h", made) || made != expected) {
        Say("The recompiled symbols differ from the ones this kit was built with. The kit is damaged or mixed up.\n");
        return Fail();
    }

    // 3. compile, every file on its own
    std::vector<fs::path> sources;
    for (auto& e : fs::directory_iterator(gen, ec))
        if (e.path().extension() == ".cpp") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end(), [](const fs::path& a, const fs::path& b) {
        return fs::file_size(a) > fs::file_size(b);  // the big ones first, so the last to finish is short
    });
    Say("Compiling %zu files on %u threads ...\n", sources.size(), jobs);
    std::vector<std::string> base = k.cxxflags;
    for (auto& inc : k.include) base.push_back("-I" + inc.generic_string());
    base.push_back("-I" + gen.generic_string());
    std::vector<Child> running;
    std::vector<fs::path> objects(sources.size());
    size_t next = 0, done = 0;
    bool failed = false;
    while (done < sources.size() && !(failed && running.empty())) {
        while (!failed && next < sources.size() && running.size() < jobs) {
            size_t i = next++;
            objects[i] = obj / (sources[i].stem().string() + ".o");
            std::vector<std::string> args = base;
            args.insert(args.end(), {"-c", sources[i].generic_string(), "-o", objects[i].generic_string()});
            Child c;
            c.job = i;
            if (!Spawn(k.cxx, args, logs / (sources[i].stem().string() + ".log"), c)) {
                Say("Cannot start the compiler %s\n", k.cxx.string().c_str());
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
                Say("Compiling %s failed:\n", sources[job].filename().string().c_str());
                ShowLog(logs / (sources[job].stem().string() + ".log"));
            }
            failed = true;
            continue;
        }
        Say("\r  %zu / %zu", done, sources.size());
    }
    Say("\n");
    if (failed) return Fail();

    // 4. link beside Setup; a response file keeps the command line short
    Say("Linking %s.exe ...\n", k.exe.c_str());
    fs::path rsp = work / "link.rsp", out = root / (k.exe + ".exe"), tmp = root / (k.exe + ".exe.new");
    {
        std::ofstream f(rsp);
        auto put = [&](const std::string& s) { f << '"' << s << "\"\n"; };
        // in the order the SDK's own build links them (the mapping, then the partitions by number), so every player's
        // exe has the layout of the developers' and a crash address means the same in both
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
        put(tmp.generic_string());
    }
    if (!Run("Linking", k.cxx, {"@" + rsp.generic_string()}, logs / "link.log")) return Fail();
    fs::rename(tmp, out, ec);
    if (ec) {
        Say("Cannot replace %s: %s (is the game still running?)\n", out.string().c_str(), ec.message().c_str());
        return Fail();
    }

    // the generated code and objects are the title's code: only kept when asked for
    if (!keep) fs::remove_all(work, ec);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Say("\nDone in %.0f s: %s\nStart the game with %s.exe.\n", secs, out.string().c_str(), k.exe.c_str());
    if (g_log) std::fclose(g_log);
    Pause();
    return 0;
}
