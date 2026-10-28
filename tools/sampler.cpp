// sampler - a sampling profiler for Game.exe on Windows, with nothing to install: it suspends the chosen threads about
// a thousand times a second, walks their stacks and names the frames from Game.pdb with the system's dbghelp, inline
// frames included. See docs/performance.md.
//
//   toolchain\zig-0.16.0-windows\zig.exe c++ -O2 -target x86_64-windows-gnu tools\sampler.cpp -o sampler.exe ^
//       -ldbghelp -lwinmm -lpsapi
//   sampler.exe <pid> <seconds> [thread names, comma separated] [output file]
//   sampler.exe (Get-Process Game).Id 15 "render,guest" profile.txt
//
// Per thread: self time by function and by function and line, memset/memcmp/memcpy charged to their callers, and
// inclusive time by function. Game.exe needs a --pdb build (tools\build.bat --pdb).
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct Thread {
    DWORD id;
    HANDLE h;
    std::string name;
    uint64_t samples = 0;
    std::vector<std::vector<uint64_t>> stacks;
};

static std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += char(*w < 128 ? *w : '?');
    return s;
}

// a name in the comma separated list, as part of it
static bool Wanted(const std::string& name, const std::string& filter) {
    if (filter.empty()) return true;
    for (size_t p = 0; p <= filter.size();) {
        size_t q = filter.find(',', p);
        if (q == std::string::npos) q = filter.size();
        if (q > p && name.find(filter.substr(p, q - p)) != std::string::npos) return true;
        p = q + 1;
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 3) return printf("sampler <pid> <seconds> [thread names] [output file]\n"), 1;
    DWORD pid = DWORD(atoi(argv[1]));
    double secs = atof(argv[2]);
    std::string filter = argc > 3 ? argv[3] : "";
    if (argc > 4 && !freopen(argv[4], "w", stdout)) return printf("can't write %s\n", argv[4]), 1;
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) return printf("OpenProcess failed %lu\n", GetLastError()), 1;
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    auto get_desc = reinterpret_cast<GetDesc>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));

    // the threads by the names the runtime gives them (render, guest N, dsound, ...)
    std::vector<Thread> threads;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR d = nullptr;
        std::string name;
        if (get_desc && SUCCEEDED(get_desc(h, &d)) && d) {
            name = Narrow(d);
            LocalFree(d);
        }
        if (name.empty()) name = "tid" + std::to_string(te.th32ThreadID);
        if (!Wanted(name, filter)) {
            CloseHandle(h);
            continue;
        }
        threads.push_back({te.th32ThreadID, h, name});
    }
    CloseHandle(snap);
    printf("%zu threads\n", threads.size());

    // not deferred: the pdb loads now, from beside the exe, which names it bare
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_ANYTHING | SYMOPT_LOAD_LINES);
    if (!SymInitialize(proc, nullptr, TRUE)) return printf("SymInitialize failed %lu\n", GetLastError()), 1;
    {
        HMODULE mods[1];
        DWORD need = 0;
        EnumProcessModules(proc, mods, sizeof(mods), &need);
        wchar_t path[MAX_PATH];
        GetModuleFileNameExW(proc, mods[0], path, MAX_PATH);
        std::wstring dir = path;
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        SymSetSearchPathW(proc, dir.c_str());
        SymUnloadModule64(proc, DWORD64(mods[0]));
        SymLoadModuleExW(proc, nullptr, path, nullptr, DWORD64(mods[0]), 0, nullptr, 0);
        IMAGEHLP_MODULE64 m{sizeof(m)};
        SymGetModuleInfo64(proc, DWORD64(mods[0]), &m);
        // SymPdb (3) with the pdb's path, or the names are only the exports'
        printf("main module %s, symbols type %d from '%s'\n", m.ModuleName, int(m.SymType), m.LoadedPdbName);
    }

    auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs);
    timeBeginPeriod(1);
    uint64_t rounds = 0;
    while (std::chrono::steady_clock::now() < end) {
        for (Thread& t : threads) {
            if (SuspendThread(t.h) == DWORD(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t.h, &ctx)) {
                STACKFRAME64 f{};
                f.AddrPC.Offset = ctx.Rip;
                f.AddrPC.Mode = AddrModeFlat;
                f.AddrFrame.Offset = ctx.Rbp;
                f.AddrFrame.Mode = AddrModeFlat;
                f.AddrStack.Offset = ctx.Rsp;
                f.AddrStack.Mode = AddrModeFlat;
                std::vector<uint64_t> st;
                for (int i = 0; i < 48; i++) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, t.h, &f, &ctx, nullptr, SymFunctionTableAccess64,
                                     SymGetModuleBase64, nullptr))
                        break;
                    if (!f.AddrPC.Offset) break;
                    st.push_back(f.AddrPC.Offset);
                }
                t.stacks.push_back(std::move(st));
                t.samples++;
            }
            ResumeThread(t.h);
        }
        rounds++;
        Sleep(1);
    }
    printf("%llu rounds in %.1f s\n", (unsigned long long)rounds, secs);

    char buf[sizeof(SYMBOL_INFO) + 1024];
    auto sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    auto Clear = [&] {
        std::memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 1000;
    };
    // the inline frame functions, not in mingw-w64's dbghelp.h
    using IncludeFn = DWORD(WINAPI*)(HANDLE, DWORD64);
    using QueryFn = BOOL(WINAPI*)(HANDLE, DWORD64, DWORD, DWORD64, DWORD64, LPDWORD, LPDWORD);
    using FromFn = BOOL(WINAPI*)(HANDLE, DWORD64, ULONG, PDWORD64, PSYMBOL_INFO);
    using LineFn = BOOL(WINAPI*)(HANDLE, DWORD64, ULONG, DWORD64, PDWORD, PIMAGEHLP_LINE64);
    HMODULE dh = GetModuleHandleW(L"dbghelp.dll");
    auto include_inline = reinterpret_cast<IncludeFn>(reinterpret_cast<void*>(GetProcAddress(dh, "SymAddrIncludeInlineTrace")));
    auto query_inline = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(dh, "SymQueryInlineTrace")));
    auto from_inline = reinterpret_cast<FromFn>(reinterpret_cast<void*>(GetProcAddress(dh, "SymFromInlineContext")));
    auto line_inline = reinterpret_cast<LineFn>(reinterpret_cast<void*>(GetProcAddress(dh, "SymGetLineFromInlineContext")));

    // a path from src\ on, or the file's name
    auto Short = [](std::string f) {
        size_t p = f.find("src\\");
        if (p != std::string::npos) return f.substr(p + 4);
        p = f.find_last_of("\\/");
        return p == std::string::npos ? f : f.substr(p + 1);
    };
    // an address as its functions, innermost inline first, each with its file:line
    struct Fn {
        std::string name, line;
    };
    std::unordered_map<uint64_t, std::vector<Fn>> expanded;
    auto Expand = [&](uint64_t a) -> const std::vector<Fn>& {
        auto it = expanded.find(a);
        if (it != expanded.end()) return it->second;
        std::vector<Fn> v;
        DWORD n = include_inline && query_inline && from_inline ? include_inline(proc, a) : 0;
        DWORD ictx = 0, iframe = 0;
        if (n && query_inline(proc, a, 0, a, a, &ictx, &iframe)) {
            for (DWORD i = 0; i < n; i++) {
                Clear();
                DWORD64 disp = 0;
                Fn f;
                f.name = from_inline(proc, a, ictx + i, &disp, sym) ? sym->Name : "?";
                IMAGEHLP_LINE64 l{sizeof(l)};
                DWORD d = 0;
                if (line_inline && line_inline(proc, a, ictx + i, 0, &d, &l))
                    f.line = Short(l.FileName) + ":" + std::to_string(l.LineNumber);
                v.push_back(f);
            }
        }
        Fn o;
        Clear();
        DWORD64 disp = 0;
        if (SymFromAddr(proc, a, &disp, sym)) {
            o.name = sym->Name;
        } else {
            IMAGEHLP_MODULE64 m{sizeof(m)};
            o.name = SymGetModuleInfo64(proc, a, &m) ? std::string(m.ModuleName) + "!?" : "?";
        }
        IMAGEHLP_LINE64 l{sizeof(l)};
        DWORD d = 0;
        if (SymGetLineFromAddr64(proc, a, &d, &l)) o.line = Short(l.FileName) + ":" + std::to_string(l.LineNumber);
        v.push_back(o);
        return expanded[a] = std::move(v);
    };
    // compiler_rt's and the C library's names for them
    auto IsMem = [](const std::string& n) {
        return n.rfind("mem", 0) == 0 || n.rfind("copy", 0) == 0 || n.rfind("_platform_mem", 0) == 0;
    };
    auto Top = [](std::map<std::string, uint64_t>& m, uint64_t total, size_t most, const char* title) {
        std::vector<std::pair<uint64_t, std::string>> v;
        for (auto& [n, c] : m) v.push_back({c, n});
        std::sort(v.rbegin(), v.rend());
        printf("--- %s\n", title);
        for (size_t k = 0; k < v.size() && k < most; k++)
            printf("%6.2f%%  %s\n", 100.0 * double(v[k].first) / double(total), v[k].second.c_str());
    };

    std::sort(threads.begin(), threads.end(), [](const Thread& a, const Thread& b) { return a.name < b.name; });
    for (Thread& t : threads) {
        if (!t.samples) continue;
        std::map<std::string, uint64_t> self, self_line, incl, mem_callers;
        for (auto& st : t.stacks) {
            if (st.empty()) continue;
            // the whole stack, inline frames expanded, innermost first; a return address names the call before it
            std::vector<Fn> fr;
            for (size_t k = 0; k < st.size(); k++) {
                const auto& e = Expand(k ? st[k] - 1 : st[k]);
                fr.insert(fr.end(), e.begin(), e.end());
            }
            self[fr[0].name]++;
            self_line[fr[0].name + "  @ " + fr[0].line]++;
            if (IsMem(fr[0].name)) {
                for (size_t k = 1; k < fr.size(); k++)
                    if (!IsMem(fr[k].name)) {
                        mem_callers[fr[0].name + "  <-  " + fr[k].name + "  @ " + fr[k].line]++;
                        break;
                    }
            }
            std::set<std::string> seen;
            for (auto& f : fr)
                if (seen.insert(f.name).second) incl[f.name]++;
        }
        // a thread with nothing to do shows its wait (ZwWaitForAlertByThreadId, ...) at the top of self
        printf("\n=== thread %s (%lu): %llu samples\n", t.name.c_str(), t.id, (unsigned long long)t.samples);
        Top(self, t.samples, 50, "self by function");
        Top(self_line, t.samples, 60, "self by function and line");
        Top(mem_callers, t.samples, 40, "mem* by caller");
        Top(incl, t.samples, 120, "inclusive by function");
    }
    return 0;
}
