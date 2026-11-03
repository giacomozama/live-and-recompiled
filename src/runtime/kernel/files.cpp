#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

#include "core/loadtrace.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/exports.h"

namespace fs = std::filesystem;

namespace kernel {

namespace {

std::mutex g_links_lock;
std::map<std::string, std::string> g_links;    // upper case "\??\D:" -> "\Device\CdRom0"
std::map<std::string, fs::path> g_devices;     // upper case device -> host directory
constexpr char kCdRom[] = "\\DEVICE\\CDROM0";

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(uint8_t(c)));
    return s;
}

std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string out;
    for (char c : p) {
        if (c == '\\' && !out.empty() && out.back() == '\\') continue;
        out += c;
    }
    while (out.size() > 1 && out.back() == '\\') out.pop_back();
    return out;
}

// host lookup that ignores case on file systems that do not
fs::path MatchCase(const fs::path& dir, const std::vector<std::string>& parts) {
    fs::path cur = dir;
    for (auto& part : parts) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }
        fs::path direct = cur / part;
        std::error_code ec;
        if (fs::exists(direct, ec)) {
            cur = direct;
            continue;
        }
        bool found = false;
        std::string want = Upper(part);
        for (auto& e : fs::directory_iterator(cur, ec)) {
            if (Upper(e.path().filename().string()) == want) {
                cur = e.path();
                found = true;
                break;
            }
        }
        if (!found) cur = direct;
    }
    return cur;
}

// a folder's entry as NtQueryDirectoryFile returns it
struct Listed {
    std::string name;
    uint64_t time = 0, size = 0;
    bool dir = false;
};

struct File : Object {
    fs::path host;
    std::string xbox;
    FILE* fp = nullptr;
    const Disc::Entry* entry = nullptr;  // on the disc image instead of a host path
    bool dir = false;
    bool readonly = false;  // a mod's file, or a folder only mods have: nothing is written, renamed or deleted
    bool on_cd = false;     // on D:, where the mods' files show through
    std::string cd;         // there: the path from D:'s root, upper case, "" the root itself
    uint64_t pos = 0;
    std::vector<Listed> listing;
    size_t next = 0;
    bool listed = false;
    bool delete_on_close = false;
    bool async = false;  // opened without FILE_SYNCHRONOUS_IO_*: a read completes after the call returns
    std::mutex io;  // threads sharing a handle: seek and transfer stay together
    File() : Object(ObjType::File) {}
    ~File() override {
        if (fp) std::fclose(fp);
        if (delete_on_close) {
            std::error_code ec;
            fs::remove(host, ec);
        }
    }
};

struct Link : Object {
    std::string target;
    Link() : Object(ObjType::SymLink) {}
};

uint64_t ToFileTime(fs::file_time_type t) {
    using namespace std::chrono;
    auto sys = time_point_cast<nanoseconds>(t - fs::file_time_type::clock::now() + system_clock::now());
    return uint64_t(sys.time_since_epoch().count() / 100 + 116444736000000000ll);
}

void FillNetworkInfo(uint32_t out, uint64_t t, uint64_t size, bool dir) {
    for (int i = 0; i < 4; i++) W64(out + i * 8, t);
    W64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    W64(out + 0x28, size);
    W32(out + 0x30, dir ? 0x10 : 0x80);  // FILE_ATTRIBUTE_DIRECTORY or NORMAL
}

void FillNetworkInfo(uint32_t out, const fs::path& p) {
    std::error_code ec;
    bool dir = fs::is_directory(p, ec);
    FillNetworkInfo(out, ToFileTime(fs::last_write_time(p, ec)), dir ? 0 : fs::file_size(p, ec), dir);
}

void FillNetworkInfo(uint32_t out, const Disc::Entry& e) {
    FillNetworkInfo(out, MountedDisc()->Time(), e.dir ? 0 : e.size, e.dir);
}

Listed ListHost(const fs::path& p) {
    std::error_code ec;
    Listed l{p.filename().string(), ToFileTime(fs::last_write_time(p, ec)), 0, fs::is_directory(p, ec)};
    if (!l.dir) l.size = fs::file_size(p, ec);
    return l;
}

// Mods: folders laid out like D:, set before Boot and only read after. A file in one stands in for the game's at
// the same path, or adds one; the first mod listed wins where two have the same file
std::map<std::string, fs::path> g_mod_files;  // D: path, upper case -> the winning mod's file
struct ModChild {
    std::string name;  // as the first mod that has it spells it
    fs::path host;     // the winning file, or the first mod's folder
};
// D: folder, upper case ("" the root) -> what the mods have in it, by upper case name
std::map<std::string, std::map<std::string, ModChild>> g_mod_dirs;

// a folder only mods have: the time of the first mod's
uint64_t ModDirTime(const std::string& cd) {
    const size_t cut = cd.rfind('\\');
    const std::string parent = cut == std::string::npos ? "" : cd.substr(0, cut);
    auto dir = g_mod_dirs.find(parent);
    if (dir != g_mod_dirs.end())
        if (auto it = dir->second.find(cd.substr(cut == std::string::npos ? 0 : cut + 1)); it != dir->second.end())
            return ListHost(it->second.host).time;
    return 0;
}

// the parts from D:'s root as a key: upper case, '\' between, "." and ".." taken out
std::string CdKey(const std::vector<std::string>& parts) {
    std::vector<std::string> path;
    for (const std::string& part : parts) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!path.empty()) path.pop_back();
            continue;
        }
        path.push_back(Upper(part));
    }
    std::string key;
    for (const std::string& part : path) key += (key.empty() ? "" : "\\") + part;
    return key;
}

// The title copies files from D: to its cache on Z: (Conker: Audio\X.xwb from D:\dvddata\Audio\X.xwb) and reads
// them there from then on, copying again only what is missing. So a cached file whose D: file a mod now replaces, or
// replaced last launch and no longer does, is deleted, and the title copies it again through the mods. Z.mods beside
// the partitions keeps the mods' files as of the last launch. A cached file is taken to come from the D: file whose
// path ends in the cached file's own
void DropStaleCache(const fs::path& data) {
    std::error_code ec;
    std::map<std::string, std::string> now, was;  // D: path -> host, size and time
    for (auto& [key, host] : g_mod_files)
        now[key] = host.string() + "\t" + std::to_string(uint64_t(fs::file_size(host, ec))) + "\t" +
                   std::to_string(int64_t(fs::last_write_time(host, ec).time_since_epoch().count()));
    const fs::path file = data / "Z.mods";
    {
        std::ifstream in(file, std::ios::binary);
        std::string line;
        while (std::getline(in, line))
            if (size_t tab = line.find('\t'); tab != std::string::npos) was[line.substr(0, tab)] = line.substr(tab + 1);
    }
    if (now == was) return;
    std::vector<std::string> changed;
    for (auto& [key, v] : now)
        if (auto it = was.find(key); it == was.end() || it->second != v) changed.push_back(key);
    for (auto& [key, v] : was)
        if (!now.count(key)) changed.push_back(key);

    // listed first, deleted after: the listing isn't changed under the iterator
    const fs::path z = data / "Z";
    std::vector<std::pair<fs::path, std::string>> stale;
    ec.clear();
    for (auto it = fs::recursive_directory_iterator(z, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::vector<std::string> parts;
        for (const fs::path& part : it->path().lexically_relative(z)) parts.push_back(part.string());
        const std::string cached = CdKey(parts);
        for (const std::string& key : changed)
            if (key == cached || (key.size() > cached.size() && key[key.size() - cached.size() - 1] == '\\' &&
                                  key.compare(key.size() - cached.size(), cached.size(), cached) == 0)) {
                stale.emplace_back(it->path(), cached);
                break;
            }
    }
    bool ok = true;
    for (auto& [path, cached] : stale) {
        std::error_code rm;
        if (fs::remove(path, rm)) {
            LOGR("Mods: dropped Z:\\%s from the title's cache, to be copied again", cached.c_str());
        } else {
            LOGW("files: deleting %s: %s", path.string().c_str(), rm.message().c_str());
            ok = false;  // tried again next launch
        }
    }
    if (!ok) return;
    if (now.empty()) {
        fs::remove(file, ec);
        return;
    }
    std::ofstream out(file, std::ios::binary);
    for (auto& [key, v] : now) out << key << '\t' << v << '\n';
}

bool WildMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::toupper(uint8_t(pat[p])) == std::toupper(uint8_t(s[i])))) {
            p++;
            i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

// past 2 GB a long offset wraps on Windows
void Seek(FILE* fp, uint64_t pos) {
#ifdef _WIN32
    _fseeki64(fp, int64_t(pos), SEEK_SET);
#else
    fseeko(fp, off_t(pos), SEEK_SET);
#endif
}

void IoStatus(uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    W32(iosb, status);
    W32(iosb + 4, info);
}

std::shared_ptr<File> FileFromHandle(uint32_t h) {
    ObjectRef o = FromHandle(h);
    if (!o || o->type != ObjType::File) return nullptr;
    return std::static_pointer_cast<File>(o);
}

std::string ObjectName(uint32_t attrs, uint32_t* root = nullptr) {
    if (root) *root = R32(attrs);
    return ReadAnsiString(R32(attrs + 4));
}

void Complete(uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    if (event)
        if (ObjectRef e = FromHandle(event)) SetEvent(*e);
    if (apc && Current()) QueueApc(*Current(), apc, apc_ctx, iosb, 0);
}

}  // namespace

std::string ReadAnsiString(uint32_t str) {
    if (!str) return {};
    uint16_t len = R16(str);
    uint32_t buf = R32(str + 4);
    if (!buf) return {};
    return std::string(reinterpret_cast<const char*>(HostPtr(buf)), len);
}

std::string ReadCString(uint32_t va, size_t max) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < max; i++) {
        char c = char(G<uint8_t>(va + uint32_t(i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void MountDefaults(const std::string& game_dir, const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    fs::path d = data_dir;
    g_devices[kCdRom] = game_dir;
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION1"] = d / "E";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION2"] = d / "C";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION3"] = d / "X";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION4"] = d / "Y";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION5"] = d / "Z";
    DropStaleCache(d);
    for (auto& [k, v] : g_devices) {
        std::error_code ec;
        fs::create_directories(v, ec);
    }
    g_links["\\??\\D:"] = "\\Device\\CdRom0";
    const Disc* disc = MountedDisc();
    LOGI("files: D: is %s, hard disk under %s", disc ? disc->Path().string().c_str() : game_dir.c_str(), data_dir.c_str());
}

bool CreateSymbolicLink(const std::string& link, const std::string& target) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_links[Upper(Normalize(link))] = Normalize(target);
    LOGI("files: link %s -> %s", link.c_str(), target.c_str());
    return true;
}

bool DeleteSymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    return g_links.erase(Upper(Normalize(link))) != 0;
}

std::string QuerySymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    auto it = g_links.find(Upper(Normalize(link)));
    return it == g_links.end() ? std::string() : it->second;
}

void MountMods(const std::vector<fs::path>& dirs) {
    g_mod_files.clear();
    g_mod_dirs.clear();
    for (const fs::path& root : dirs) {
        size_t files = 0;  // the ones no mod above has
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path rel = it->path().lexically_relative(root);
            std::vector<std::string> parts;
            for (const fs::path& part : rel) parts.push_back(part.string());
            if (parts.empty()) continue;
            const std::string name = parts.back();
            parts.pop_back();
            const std::string parent = CdKey(parts);
            const std::string up = Upper(name);
            const bool dir = it->is_directory(ec);
            if (!dir && !it->is_regular_file(ec)) continue;
            if (!dir) {
                const std::string key = parent.empty() ? up : parent + "\\" + up;
                if (!g_mod_files.emplace(key, it->path()).second) continue;  // a mod above has it
                files++;
            }
            g_mod_dirs[parent].emplace(up, ModChild{name, it->path()});
        }
        LOGR("Mod %s: %zu files in use", root.filename().string().c_str(), files);
    }
}

namespace {

struct Target {
    std::string host;                    // empty on the disc image or unmapped
    bool disc = false;                   // on the mounted disc image
    const Disc::Entry* entry = nullptr;  // there: the file or folder, null when missing
    bool on_cd = false;                  // on D:
    std::string cd;                      // there: the path from the root, upper case (CdKey)
    bool mod = false;                    // host is a mod's file, read only
    bool mod_dir = false;                // a folder only the mods have
};

Target Resolve(const std::string& xbox_path, uint32_t root_handle) {
    std::string p = Normalize(xbox_path);
    if (root_handle) {
        if (auto f = FileFromHandle(root_handle)) p = Normalize(f->xbox + "\\" + p);
    }
    if (p.size() >= 2 && p[1] == ':') p = "\\??\\" + p;
    std::lock_guard<std::mutex> lock(g_links_lock);
    for (int depth = 0; depth < 8; depth++) {
        std::string up = Upper(p);
        bool changed = false;
        for (auto& [k, v] : g_links) {
            if (up.compare(0, k.size(), k) == 0 && (up.size() == k.size() || up[k.size()] == '\\')) {
                p = v + p.substr(k.size());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    std::string up = Upper(p);
    const std::pair<const std::string, fs::path>* best = nullptr;
    for (auto& e : g_devices)
        if (up.compare(0, e.first.size(), e.first) == 0 && (up.size() == e.first.size() || up[e.first.size()] == '\\'))
            if (!best || e.first.size() > best->first.size()) best = &e;
    if (!best) return {};
    std::vector<std::string> parts;
    std::string rest = p.substr(best->first.size());
    size_t s = 0;
    while (s < rest.size()) {
        size_t e = rest.find('\\', s);
        if (e == std::string::npos) e = rest.size();
        if (e > s) parts.push_back(rest.substr(s, e - s));
        s = e + 1;
    }
    // no Xbox name holds ':', and fs::path would take "D:" as a host drive
    for (auto& part : parts)
        if (part.find(':') != std::string::npos) return {};
    if (best->first != kCdRom) return {MatchCase(best->second, parts).string()};
    Target t;
    t.on_cd = true;
    t.cd = CdKey(parts);
    if (auto it = g_mod_files.find(t.cd); it != g_mod_files.end()) {
        t.host = it->second.string();
        t.mod = true;
        return t;
    }
    std::error_code ec;
    bool found;
    if (const Disc* disc = MountedDisc()) {
        t.disc = true;
        t.entry = disc->Find(parts);
        found = t.entry;
    } else {
        t.host = MatchCase(best->second, parts).string();
        found = fs::exists(t.host, ec);
    }
    if (!found && g_mod_dirs.count(t.cd)) t.mod_dir = true;
    return t;
}

// a mod's file, or a folder only mods have: opened as on the disc, read only
uint32_t OpenMod(const Target& t, const std::string& name, uint32_t handle_out, uint32_t iosb, uint32_t disposition,
                 uint32_t options) {
    if (disposition != 1 && disposition != 3) {  // FILE_OPEN, FILE_OPEN_IF
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    auto f = std::make_shared<File>();
    f->xbox = Normalize(name);
    f->readonly = true;
    f->on_cd = true;
    f->cd = t.cd;
    f->dir = t.mod_dir;
    if (f->dir && (options & 0x40)) {  // FILE_NON_DIRECTORY_FILE
        IoStatus(iosb, kFileIsADirectory, 0);
        return kFileIsADirectory;
    }
    if (!f->dir) {
        if (options & 1) {  // FILE_DIRECTORY_FILE
            IoStatus(iosb, kObjectNameNotFound, 5);
            return kObjectNameNotFound;
        }
        f->host = t.host;
        f->fp = std::fopen(t.host.c_str(), "rb");
        if (!f->fp) {
            LOGW("files: cannot open %s", t.host.c_str());
            IoStatus(iosb, kAccessDenied, 0);
            return kAccessDenied;
        }
    }
    f->async = !(options & 0x30);  // FILE_SYNCHRONOUS_IO_ALERT / _NONALERT
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    LOG_FIRST(400, "open %s from %s", name.c_str(), f->dir ? "the mods" : t.host.c_str());
    return kSuccess;
}

// the disc image is read only: nothing on it is created, replaced or written
uint32_t OpenOnDisc(const Disc::Entry* e, const std::string& cd, const std::string& name, uint32_t handle_out, uint32_t iosb,
                    uint32_t disposition, uint32_t options) {
    if (!e && (disposition == 1 || disposition == 4)) {  // FILE_OPEN or FILE_OVERWRITE need an existing file
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return kObjectNameNotFound;
    }
    if (!e || (disposition != 1 && disposition != 3)) {
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    if (e->dir && !(options & 1) && (options & 0x40)) {  // options 1 FILE_DIRECTORY_FILE, 0x40 FILE_NON_DIRECTORY_FILE
        IoStatus(iosb, kFileIsADirectory, 0);
        return kFileIsADirectory;
    }
    auto f = std::make_shared<File>();
    f->xbox = Normalize(name);
    f->entry = e;
    f->dir = e->dir;
    f->on_cd = true;
    f->cd = cd;
    f->async = !(options & 0x30);  // FILE_SYNCHRONOUS_IO_ALERT / _NONALERT
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

}  // namespace

std::string ResolvePath(const std::string& xbox_path, uint32_t root_handle) { return Resolve(xbox_path, root_handle).host; }

bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out) {
    Target t = Resolve(xbox_path, 0);
    if (t.disc) {
        if (!t.entry || t.entry->dir) return false;
        out.resize(t.entry->size);
        return MountedDisc()->Read(*t.entry, 0, out.data(), t.entry->size) == t.entry->size;
    }
    std::ifstream f(t.host, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// exports

KERNEL_EXPORT(190, NtCreateFile, 9) {
    loadtrace::Scope trace(loadtrace::kOpen, loadtrace::kOpenMain);
    uint32_t handle_out = a[0], access = a[1], attrs = a[2], iosb = a[3], disposition = a[7], options = a[8];
    (void)access;
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.mod || t.mod_dir) return OpenMod(t, name, handle_out, iosb, disposition, options);
    if (t.disc) return OpenOnDisc(t.entry, t.cd, name, handle_out, iosb, disposition, options);
    const std::string& host = t.host;
    if (host.empty()) {
        LOGW("NtCreateFile: no mapping for %s", name.c_str());
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    std::error_code ec;
    bool exists = fs::exists(host, ec);
    bool is_dir = exists && fs::is_directory(host, ec);
    bool want_dir = options & 1;
    uint32_t info = 1;  // FILE_OPENED
    if (want_dir || is_dir) {
        if (!exists) {
            if (disposition == 1 || disposition == 4) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            fs::create_directories(host, ec);
            info = 2;
        } else if (disposition == 2) {
            IoStatus(iosb, kObjectNameCollision, 4);
            return kObjectNameCollision;
        }
        if (!want_dir && (options & 0x40)) {
            IoStatus(iosb, kFileIsADirectory, 0);
            return kFileIsADirectory;
        }
        auto f = std::make_shared<File>();
        f->host = host;
        f->xbox = Normalize(name);
        f->dir = true;
        f->on_cd = t.on_cd;
        f->cd = t.cd;
        W32(handle_out, NewHandle(f));
        IoStatus(iosb, kSuccess, info);
        return kSuccess;
    }
    const char* mode = nullptr;
    switch (disposition) {
        case 0:  // supersede
        case 5:  // overwrite if
            mode = "w+b";
            info = exists ? 3 : 2;
            break;
        case 1:  // open
            if (!exists) {
                LOG_FIRST(200, "missing %s", name.c_str());
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "r+b";
            break;
        case 2:  // create
            if (exists) {
                IoStatus(iosb, kObjectNameCollision, 4);
                return kObjectNameCollision;
            }
            mode = "w+b";
            info = 2;
            break;
        case 3:  // open if
            mode = exists ? "r+b" : "w+b";
            info = exists ? 1 : 2;
            break;
        case 4:  // overwrite
            if (!exists) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "w+b";
            info = 3;
            break;
        default:
            return kInvalidParameter;
    }
    // a file in a missing folder fails as on the console: XAPI probes saves with OPEN_ALWAYS
    if (!exists && !fs::is_directory(fs::path(host).parent_path(), ec)) {
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    FILE* fp = std::fopen(host.c_str(), mode);
    if (!fp && std::string(mode) == "r+b") fp = std::fopen(host.c_str(), "rb");  // read only media
    if (!fp) {
        LOGW("NtCreateFile: cannot open %s", host.c_str());
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->fp = fp;
    f->delete_on_close = options & 0x1000;  // FILE_DELETE_ON_CLOSE
    f->async = !(options & 0x30);           // FILE_SYNCHRONOUS_IO_ALERT / _NONALERT
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

KERNEL_EXPORT(202, NtOpenFile, 6) {
    loadtrace::Scope trace(loadtrace::kOpen, loadtrace::kOpenMain);
    // same as create with FILE_OPEN
    uint32_t handle_out = a[0], attrs = a[2], iosb = a[3], options = a[5];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.mod || t.mod_dir) return OpenMod(t, name, handle_out, iosb, 1, options);
    if (t.disc) return OpenOnDisc(t.entry, t.cd, name, handle_out, iosb, 1, options);
    const std::string& host = t.host;
    std::error_code ec;
    if (host.empty() || !fs::exists(host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return host.empty() ? kObjectPathNotFound : kObjectNameNotFound;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->dir = fs::is_directory(host, ec);
    f->on_cd = t.on_cd;
    f->cd = t.cd;
    if (!f->dir) {
        f->fp = std::fopen(host.c_str(), "r+b");
        if (!f->fp) f->fp = std::fopen(host.c_str(), "rb");
        if (!f->fp) return kAccessDenied;
    }
    f->async = !(options & 0x30);  // FILE_SYNCHRONOUS_IO_ALERT / _NONALERT
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    return kSuccess;
}

// I/O completes before the call returns: the status block is written, then the file object, the event and the
// APC are signalled (GetOverlappedResult without an event waits on the file handle)
static uint32_t Finish(File& f, uint32_t status, uint32_t n, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    IoStatus(iosb, status, n);
    SetEvent(f);
    Complete(event, apc, apc_ctx, iosb);
    return status;
}

static uint64_t Offset(File& f, uint32_t off) {
    int64_t o = off ? int64_t(R64(off)) : -1;
    return o >= 0 ? uint64_t(o) : f.pos;
}

// A read on an asynchronous handle returns STATUS_PENDING and completes a moment later, as the console's does. The
// data is already in place; only the status block, the events and the APC wait. Titles poll that status: XACT
// issues a stream's reads and then asks whether any is still in flight, and a read that had already completed made it
// end a short sound before its only packet was played. Completions land after about a millisecond, or as soon as the
// thread that issued them waits, sleeps or yields, so a title that reads and then waits or polls loses nothing.
struct PendingIo {
    std::shared_ptr<File> f;
    uint32_t status, n, event, apc, apc_ctx, iosb;
    ThreadRef issuer;
    std::chrono::steady_clock::time_point due;
};
std::mutex g_io_lock;
std::condition_variable g_io_cv;
std::deque<PendingIo> g_io;

void CompleteIo(const PendingIo& p) {
    IoStatus(p.iosb, p.status, p.n);
    SetEvent(*p.f);
    if (p.event)
        if (ObjectRef e = FromHandle(p.event)) SetEvent(*e);
    if (p.apc && p.issuer) QueueApc(*p.issuer, p.apc, p.apc_ctx, p.iosb, 0);
}

uint32_t Defer(const std::shared_ptr<File>& f, uint32_t status, uint32_t n, uint32_t event, uint32_t apc, uint32_t apc_ctx,
               uint32_t iosb) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            std::unique_lock<std::mutex> lock(g_io_lock);
            for (;;) {
                if (g_io.empty()) {
                    g_io_cv.wait(lock);
                    continue;
                }
                auto due = g_io.front().due;
                if (std::chrono::steady_clock::now() < due) {
                    g_io_cv.wait_until(lock, due);
                    continue;
                }
                PendingIo p = std::move(g_io.front());
                g_io.pop_front();
                lock.unlock();
                CompleteIo(p);
                lock.lock();
            }
        }).detach();
    });
    IoStatus(iosb, kPending, 0);
    std::lock_guard<std::mutex> lock(g_io_lock);
    g_io.push_back({f, status, n, event, apc, apc_ctx, iosb, CurrentRef(),
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(1)});
    g_io_cv.notify_one();
    return kPending;
}

void FlushPendingIo() {
    std::vector<PendingIo> due;
    {
        std::lock_guard<std::mutex> lock(g_io_lock);
        if (g_io.empty()) return;
        Thread* me = Current();
        for (auto it = g_io.begin(); it != g_io.end();) {
            if (it->issuer.get() != me) {
                ++it;
                continue;
            }
            due.push_back(std::move(*it));
            it = g_io.erase(it);
        }
    }
    for (const PendingIo& p : due) CompleteIo(p);
}

void LogPendingIo() {
    std::lock_guard<std::mutex> lock(g_io_lock);
    auto now = std::chrono::steady_clock::now();
    double late = g_io.empty() ? 0 : std::chrono::duration<double>(now - g_io.front().due).count();
    LOGE("files: %zu async reads waiting to complete, the oldest %.2fs past due", g_io.size(), late);
    for (size_t i = 0; i < g_io.size() && i < 8; i++)
        LOGE("files:  %s %X bytes, thread %X", g_io[i].f->xbox.c_str(), g_io[i].n, g_io[i].issuer ? g_io[i].issuer->id : 0);
}

// pages is the FILE_SEGMENT_ELEMENT array for scatter and gather, 0 for one buffer
static uint32_t Transfer(const Args& a, bool write, uint32_t pages) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], buf = a[5], len = a[6], off = a[7];
    loadtrace::Scope trace(write ? loadtrace::kKinds : loadtrace::kRead, loadtrace::kReadMain);
    auto f = FileFromHandle(h);
    if (!f || (!f->fp && !f->entry)) return kInvalidHandle;
    if (write && (f->entry || f->readonly)) return kAccessDenied;
    ResetEvent(*f);
    std::lock_guard<std::mutex> lock(f->io);
    uint64_t pos = Offset(*f, off);
    if (f->fp) Seek(f->fp, pos);
    uint32_t n = 0;
    while (n < len) {
        uint32_t at = pages ? R32(pages + n / 0x1000 * 8) + n % 0x1000 : buf + n;
        uint32_t chunk = pages ? std::min(len - n, 0x1000 - n % 0x1000) : len;
        size_t done = f->entry ? MountedDisc()->Read(*f->entry, pos + n, HostPtr(at), chunk)
                      : write  ? std::fwrite(HostPtr(at), 1, chunk, f->fp)
                               : std::fread(HostPtr(at), 1, chunk, f->fp);
        n += uint32_t(done);
        if (done < chunk) break;
    }
    f->pos = pos + n;
    trace.bytes = n;
    static const bool trace_xwb = std::getenv("XBE_TRACE_DSOUND") != nullptr;
    if (trace_xwb && !write && !pages && f->xbox.size() > 4 && f->xbox.compare(f->xbox.size() - 4, 4, ".xwb") == 0)
        LOGR("xwb read %s %llX %X -> %08X", f->xbox.c_str(), (unsigned long long)pos, n, buf);
    if (write && n < len) return Finish(*f, 0xC000007F, n, event, apc, apc_ctx, iosb);  // disk full
    if (!write && len && !n) return Finish(*f, kEndOfFile, 0, event, apc, apc_ctx, iosb);
    if (!write && f->async) return Defer(f, kSuccess, n, event, apc, apc_ctx, iosb);
    return Finish(*f, kSuccess, n, event, apc, apc_ctx, iosb);
}

KERNEL_EXPORT(219, NtReadFile, 8) { return Transfer(a, false, 0); }
KERNEL_EXPORT(236, NtWriteFile, 8) { return Transfer(a, true, 0); }
// the buffer argument is an array of one page per element
KERNEL_EXPORT(220, NtReadFileScatter, 8) { return Transfer(a, false, a[5]); }
KERNEL_EXPORT(237, NtWriteFileGather, 8) { return Transfer(a, true, a[5]); }

KERNEL_EXPORT(211, NtQueryInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], out = a[2], len = a[3], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    std::error_code ec;
    uint64_t size = f->dir ? 0 : f->entry ? f->entry->size : fs::file_size(f->host, ec);
    if (f->fp) {
        std::fflush(f->fp);
        size = fs::file_size(f->host, ec);
    }
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 4: {  // FileBasicInformation
            uint64_t t = f->entry               ? MountedDisc()->Time()
                         : f->host.empty() ? ModDirTime(f->cd)
                                           : ToFileTime(fs::last_write_time(f->host, ec));
            for (int i = 0; i < 4; i++) W64(out + i * 8, t);
            W32(out + 0x20, f->dir ? 0x10 : 0x80);
            IoStatus(iosb, kSuccess, 0x28);
            return kSuccess;
        }
        case 5:  // FileStandardInformation
            W64(out, (size + 0xFFF) & ~0xFFFull);
            W64(out + 8, size);
            W32(out + 0x10, 1);  // NumberOfLinks
            G<uint8_t>(out + 0x15) = f->dir;  // Directory, after DeletePending
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 14:  // FilePositionInformation
            W64(out, f->pos);
            IoStatus(iosb, kSuccess, 8);
            return kSuccess;
        case 34:  // FileNetworkOpenInformation
            if (f->entry) FillNetworkInfo(out, *f->entry);
            else if (f->host.empty()) FillNetworkInfo(out, ModDirTime(f->cd), 0, true);
            else FillNetworkInfo(out, f->host);
            IoStatus(iosb, kSuccess, 0x38);
            return kSuccess;
        case 22:  // FileModeInformation
        case 16:
            IoStatus(iosb, kSuccess, 4);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(226, NtSetInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], in = a[2], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    if ((f->entry || f->readonly) && cls != 14 && cls != 4) return kAccessDenied;
    std::error_code ec;
    switch (cls) {
        case 14:  // position
            f->pos = R64(in);
            break;
        case 19:  // allocation: only a size below the end of file cuts it
        case 20:  // end of file
            if (f->fp) {
                std::lock_guard<std::mutex> lock(f->io);
                std::fflush(f->fp);
                uint64_t size = fs::file_size(f->host, ec);
                if (cls == 20 || R64(in) < size) fs::resize_file(f->host, R64(in), ec);
            }
            break;
        case 13:  // disposition
            f->delete_on_close = G<uint8_t>(in) != 0;
            break;
        case 10: {  // rename: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING inline)
            uint32_t root = R32(in + 4);
            std::string target = ReadAnsiString(in + 8);
            Target to = Resolve(target, root);
            if (to.mod || to.mod_dir || to.disc) return kAccessDenied;  // a mod's file is never replaced
            const std::string& host = to.host;
            if (host.empty()) return kObjectPathNotFound;
            if (f->fp) std::fflush(f->fp);
            fs::rename(f->host, host, ec);
            if (ec) return kAccessDenied;
            f->host = host;
            break;
        }
        case 4:  // basic, times and attributes are not kept
            break;
        default:
            LOG_FIRST(8, "NtSetInformationFile: class %u", cls);
            return kInvalidParameter;
    }
    IoStatus(iosb, kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(218, NtQueryVolumeInformationFile, 5) {
    uint32_t iosb = a[1], out = a[2], len = a[3], cls = a[4];
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 3:  // FileFsSizeInformation: 16 GiB total, 8 GiB free in 16 KiB units
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W32(out + 0x10, 32);
            W32(out + 0x14, 512);
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 7:  // FileFsFullSizeInformation
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W64(out + 0x10, 0x80000);
            W32(out + 0x18, 32);
            W32(out + 0x1C, 512);
            IoStatus(iosb, kSuccess, 0x20);
            return kSuccess;
        case 1:  // FileFsVolumeInformation
            W32(out + 8, 0x12345678);  // VolumeSerialNumber
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "FATX"
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            std::memcpy(HostPtr(out + 12), "FATX", 4);
            IoStatus(iosb, kSuccess, 16);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryVolumeInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(207, NtQueryDirectoryFile, 10) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], out = a[5], len = a[6], cls = a[7],
             mask = a[8], restart = a[9];
    auto f = FileFromHandle(h);
    if (!f || !f->dir) return kInvalidHandle;
    if (cls != 1) {
        LOG_FIRST(8, "NtQueryDirectoryFile: class %u", cls);
        return kInvalidParameter;
    }
    std::string pat = mask ? ReadAnsiString(mask) : "*";
    if (!f->listed || (restart & 0xFF)) {
        f->listing.clear();
        if (f->entry) {
            for (const Disc::Entry& e : f->entry->children)
                if (WildMatch(pat, e.name)) f->listing.push_back({e.name, MountedDisc()->Time(), e.dir ? 0 : e.size, e.dir});
        } else if (!f->host.empty()) {
            std::vector<fs::path> paths;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(f->host, ec))
                if (WildMatch(pat, e.path().filename().string())) paths.push_back(e.path());
            std::sort(paths.begin(), paths.end());
            for (const fs::path& p : paths) f->listing.push_back(ListHost(p));
        }
        // on D: the mods' files show through: one that stands in for the game's has its size, the ones the game lacks
        // follow
        if (f->on_cd)
            if (auto mods = g_mod_dirs.find(f->cd); mods != g_mod_dirs.end()) {
                std::map<std::string, ModChild> extra = mods->second;
                for (Listed& l : f->listing) {
                    auto it = extra.find(Upper(l.name));
                    if (it == extra.end()) continue;
                    if (!l.dir) l = ListHost(it->second.host), l.name = it->second.name;
                    extra.erase(it);
                }
                for (auto& [up, child] : extra)
                    if (WildMatch(pat, child.name)) {
                        Listed l = ListHost(child.host);
                        l.name = child.name;
                        f->listing.push_back(l);
                    }
            }
        f->next = 0;
        f->listed = true;
    }
    if (f->next >= f->listing.size()) {
        IoStatus(iosb, kNoMoreFiles, 0);
        return f->next == 0 ? kNoSuchFile : kNoMoreFiles;
    }
    const Listed& l = f->listing[f->next++];
    const std::string& fname = l.name;
    uint32_t need = 0x40 + uint32_t(fname.size());
    if (len < need) return kBufferTooSmall;
    std::memset(HostPtr(out), 0, need);
    // times, sizes and attributes sit at the same place
    FillNetworkInfo(out + 8, l.time, l.size, l.dir);
    // FILE_DIRECTORY_INFORMATION has EndOfFile before AllocationSize, the reverse of the network info
    W32(out + 0x38, R32(out + 8 + 0x30));
    uint64_t alloc = R64(out + 8 + 0x20), eof = R64(out + 8 + 0x28);
    W64(out + 0x28, eof);
    W64(out + 0x30, alloc);
    W32(out + 0x3C, uint32_t(fname.size()));
    std::memcpy(HostPtr(out + 0x40), fname.data(), fname.size());
    IoStatus(iosb, kSuccess, need);
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(210, NtQueryFullAttributesFile, 2) {
    std::string name = ReadAnsiString(R32(a[0] + 4));
    Target t = Resolve(name, R32(a[0]));
    std::error_code ec;
    if (t.mod_dir) {
        FillNetworkInfo(a[1], ModDirTime(t.cd), 0, true);
        return kSuccess;
    }
    if (t.disc ? !t.entry : t.host.empty() || !fs::exists(t.host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        return kObjectNameNotFound;
    }
    if (t.disc) FillNetworkInfo(a[1], *t.entry);
    else FillNetworkInfo(a[1], t.host);
    return kSuccess;
}

KERNEL_EXPORT(195, NtDeleteFile, 1) {
    Target t = Resolve(ReadAnsiString(R32(a[0] + 4)), R32(a[0]));
    if (t.mod || t.mod_dir || t.disc) return kAccessDenied;  // D:'s mods and image are read only
    const std::string& host = t.host;
    std::error_code ec;
    if (host.empty() || !fs::remove(host, ec)) return kObjectNameNotFound;
    return kSuccess;
}

KERNEL_EXPORT(198, NtFlushBuffersFile, 2) {
    if (auto f = FileFromHandle(a[0]))
        if (f->fp) std::fflush(f->fp);
    IoStatus(a[1], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(200, NtFsControlFile, 10) {
    IoStatus(a[4], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(196, NtDeviceIoControlFile, 10) {
    uint32_t iosb = a[4], code = a[5], out = a[8], out_len = a[9];
    LOG_FIRST(16, "NtDeviceIoControlFile: code %08X", code);
    if (out && out_len) std::memset(HostPtr(out), 0, out_len);
    if (code == 0x70000 && out_len >= 0x18) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
        W64(out, 1000);
        W32(out + 8, 12);
        W32(out + 12, 255);
        W32(out + 16, 63);
        W32(out + 20, 512);
    }
    IoStatus(iosb, kSuccess, out_len);
    return kSuccess;
}

KERNEL_EXPORT(67, IoCreateSymbolicLink, 2) {
    CreateSymbolicLink(ReadAnsiString(a[0]), ReadAnsiString(a[1]));
    return kSuccess;
}

KERNEL_EXPORT(69, IoDeleteSymbolicLink, 1) {
    return DeleteSymbolicLink(ReadAnsiString(a[0])) ? kSuccess : kObjectNameNotFound;
}

KERNEL_EXPORT(203, NtOpenSymbolicLinkObject, 2) {
    std::string name = ObjectName(a[1]);
    std::string target = QuerySymbolicLink(name);
    if (target.empty()) return kObjectNameNotFound;
    auto l = std::make_shared<Link>();
    l->target = target;
    W32(a[0], NewHandle(l));
    return kSuccess;
}

KERNEL_EXPORT(215, NtQuerySymbolicLinkObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::SymLink) return kInvalidHandle;
    auto& t = static_cast<Link&>(*o).target;
    uint32_t str = a[1];
    uint16_t maxlen = R16(str + 2);
    if (a[2]) W32(a[2], uint32_t(t.size()));
    if (t.size() > maxlen) return kBufferTooSmall;
    std::memcpy(HostPtr(R32(str + 4)), t.data(), t.size());
    W16(str, uint16_t(t.size()));
    return kSuccess;
}

}  // namespace kernel
