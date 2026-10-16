// Setup: builds the game on the player's machine from the player's own copy, so no release ever carries the
// title's code. A release is this program, a kit (kit/: the recompiler, the prebuilt host library with everything
// but the title's code, the headers the generated code includes, the config, a compiler) and an empty assets folder.
// Setup finds the game in assets (its files or a disc image), checks it is the release the kit was made for,
// recompiles it, compiles the result with the kit's compiler and links <exe>.exe beside itself.
//
//   Setup [--jobs N] [--keep] [--toolchain DIR] [--root DIR]
//
// --keep leaves kit/work (the generated code and objects) in place; --toolchain uses another llvm-mingw of the
// kit's version (a Linux-hosted one tests a kit without Windows); --root is the folder Setup would be in.
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
    k.cxx = kit_dir / t["cxx"].value_or(std::string());
    for (auto& s : Strings(t["include"])) k.include.push_back(kit_dir / s);
    k.cxxflags = Strings(t["cxxflags"]);
    k.ldflags = Strings(t["ldflags"]);
    for (auto& s : Strings(t["libs"])) k.libs.push_back(s.rfind("-", 0) == 0 ? s : (kit_dir / s).generic_string());
    if (k.exe.empty() || k.xbe.empty() || k.sha1.size() != 40) {
        error = "exe, xbe and sha1 must be set";
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
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
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
    if (code) {
        Say("%s failed (exit code %d). The end of %s:\n", what, code, log.string().c_str());
        ShowLog(log);
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
    bool keep = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--jobs" && i + 1 < argc) jobs = unsigned(std::max(1, std::atoi(argv[++i])));
        else if (a == "--keep") keep = true;
        else if (a == "--toolchain" && i + 1 < argc) toolchain = fs::absolute(argv[++i]);
        else if (a == "--root" && i + 1 < argc) root = fs::absolute(argv[++i]);
        else {
            std::fprintf(stderr, "usage: Setup [--jobs N] [--keep] [--toolchain DIR] [--root DIR]\n");
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
    Say("Building %s from your copy of the game. This takes a few minutes, once.\n\n", k.title.c_str());

    // 1. the game
    std::vector<uint8_t> xbe;
    if (!FindXbe(root, k, xbe)) return Fail();

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
