// unpack-store - unpacks the digital, backwards compatible release of the game into default.xbe, dvddata/ and media/.
//
//   unpack-store [package] [outdir]
//
// package is DefaultPackage (or the folder holding it, or its DefaultPackage.data folder); with none, assets/
// beside this program is searched. outdir defaults to assets/ beside this program: the folder Setup reads.
//
// The release is an Xbox 360 "Games on Demand" package: a small header file, DefaultPackage, and DefaultPackage.data/
// with Data0000, Data0001, ... Nothing in it is encrypted. A Data file is a master hash table, then 203 times a
// sub-table of hashes followed by the 204 4 KiB blocks it covers. Taking the hash tables out leaves the disc's file
// system, an XDVDFS volume, in 2 KiB sectors. That volume is the disc's
// game partition with the start cut off: the directory entries still count sectors from the disc's start, so the
// file data sits at a fixed distance before where they say. The distance is found by looking for the root folder
// (the one with default.xbe, its sectors all inside the data) rather than assumed. The hashes aren't checked; the
// game is checked later, by Setup, against its SHA-1.
//
// Read only on the package; writes the files of the volume under outdir. Nothing of the game is kept in this program.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kBlock = 0x1000;
constexpr uint64_t kBlocksPerTable = 204;   // data blocks under one sub-table
constexpr uint64_t kTablesPerFile = 203;    // sub-tables under a file's master table
constexpr uint64_t kFileBlocks = kBlocksPerTable * kTablesPerFile;
constexpr uint64_t kFullFile = (1 + kTablesPerFile * (1 + kBlocksPerTable)) * kBlock;
constexpr uint32_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
constexpr uint32_t kMaxTable = 16u << 20;
constexpr uint8_t kFolder = 0x10;
constexpr int kMaxDepth = 32;

// for messages: UTF-8, whatever the system's code page
std::string Str(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// the volume inside the Data files, as one run of bytes
class Volume {
public:
    explicit Volume(const fs::path& dir) {
        for (int i = 0;; i++) {
            char name[16];
            snprintf(name, sizeof(name), "Data%04d", i);
            std::error_code ec;
            uint64_t size = fs::file_size(dir / name, ec);
            if (ec) break;
            files_.emplace_back(dir / name, std::ios::binary);
            if (!files_.back()) throw Error(std::string("cannot open ") + Str((dir / name)));
            sizes_.push_back(size);
        }
        if (files_.empty()) throw Error("no Data0000 in " + Str(dir));
        uint64_t total = 0;
        for (size_t i = 0; i < sizes_.size(); i++) {
            // all but the last file are full; the last ends inside a sub-table or at its end
            bool last = i + 1 == sizes_.size();
            uint64_t blocks = 0;
            if (sizes_[i] % kBlock == 0 && sizes_[i] >= 2 * kBlock) {
                uint64_t rest = sizes_[i] / kBlock - 1, tables = rest / (kBlocksPerTable + 1), r = rest % (kBlocksPerTable + 1);
                blocks = tables * kBlocksPerTable + (r ? r - 1 : 0);
                if (!last && sizes_[i] != kFullFile) blocks = 0;
            }
            if (!blocks) {
                throw Error(std::string("Data") + (i < 10 ? "000" : "00") + std::to_string(i) +
                            " is the wrong size: the download is incomplete or isn't this kind of package");
            }
            total += blocks;
        }
        size_ = total * kBlock;
    }
    uint64_t Size() const { return size_; }

    // false past the end
    bool Read(uint64_t at, void* out, uint64_t len) {
        if (at > size_ || len > size_ - at) return false;
        auto* p = static_cast<uint8_t*>(out);
        while (len) {
            uint64_t block = at / kBlock, in = at % kBlock;
            uint64_t file = block / kFileBlocks, rem = block % kFileBlocks;
            uint64_t table = rem / kBlocksPerTable, idx = rem % kBlocksPerTable;
            uint64_t take = std::min(len, (kBlocksPerTable - idx) * kBlock - in);  // to the end of the sub-table
            uint64_t phys = (2 + table * (kBlocksPerTable + 1) + idx) * kBlock + in;
            auto& f = files_[file];
            f.clear();
            f.seekg(std::streamoff(phys));
            if (!f.read(reinterpret_cast<char*>(p), std::streamsize(take))) throw Error("read error in the package");
            p += take;
            at += take;
            len -= take;
        }
        return true;
    }

private:
    std::vector<std::ifstream> files_;
    std::vector<uint64_t> sizes_;
    uint64_t size_ = 0;
};

struct File {
    std::string path;  // relative, '/' separated
    uint64_t at;       // byte in the volume
    uint32_t size;
};

bool GoodName(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (unsigned char c : n) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || c >= 0x7F) {
            return false;
        }
    }
    return true;
}

// The files under the folder at `sector`, given how far the volume's first byte is from the disc's. Fails with
// false on anything a real folder wouldn't hold, which is how a wrong guess for `shift` shows itself.
bool Walk(Volume& v, int64_t shift, uint32_t sector, uint32_t size, const std::string& prefix, int depth,
          std::set<uint32_t>& seen, std::vector<File>& out) {
    if (depth > kMaxDepth || size > kMaxTable || !seen.insert(sector).second) return false;
    int64_t first = int64_t(sector) - shift;
    if (first < 0) return false;
    std::vector<uint8_t> t(size);
    if (size && !v.Read(uint64_t(first) * kSector, t.data(), size)) return false;
    if (size >= 4 && U32(t.data()) == 0xFFFFFFFF) return true;  // empty
    std::set<uint32_t> visited;
    std::vector<uint32_t> todo{0};
    while (!todo.empty()) {
        uint32_t off = todo.back();
        todo.pop_back();
        if (off + 14 > size || !visited.insert(off).second) return false;
        const uint8_t* e = &t[off];
        uint32_t left = U16(e) * 4u, right = U16(e + 2) * 4u, sct = U32(e + 4), len = U32(e + 8);
        uint8_t attr = e[12], nlen = e[13];
        if (off + 14u + nlen > size) return false;
        std::string name(reinterpret_cast<const char*>(e + 14), nlen);
        if (!GoodName(name)) return false;
        if (left) todo.push_back(left);
        if (right) todo.push_back(right);
        if (attr & kFolder) {
            if (len && !Walk(v, shift, sct, len, prefix + name + "/", depth + 1, seen, out)) return false;
        } else {
            int64_t at = int64_t(sct) - shift;
            if (at < 0 || uint64_t(at) * kSector + len > v.Size()) return false;
            out.push_back({prefix + name, uint64_t(at) * kSector, len});
        }
    }
    return true;
}

// Finds `shift` by looking for a folder holding default.xbe (where the root has it), and checking that the root, read
// at that place, is a whole file system whose default.xbe starts "XBEH".
int64_t FindShift(Volume& v, uint32_t root, uint32_t rootSize, std::vector<File>& files) {
    constexpr char kNeedle[] = "\x0b" "default.xbe";
    constexpr size_t kLen = sizeof(kNeedle) - 1;
    constexpr uint64_t kChunk = 8u << 20;
    std::vector<uint8_t> buf(kChunk + kLen);
    for (uint64_t at = 0; at < v.Size(); at += kChunk) {
        uint64_t n = std::min<uint64_t>(kChunk + kLen, v.Size() - at);
        v.Read(at, buf.data(), n);
        for (uint64_t i = 0; i + kLen <= n; i++) {
            if (buf[i] != 0x0B) continue;
            bool same = true;
            for (size_t k = 1; k < kLen && same; k++) same = std::tolower(buf[i + k]) == kNeedle[k];
            // an entry that starts in this chunk, or straddles into the next one's start
            if (!same || i >= kChunk) continue;
            uint64_t sectorOfTable = (at + i) / kSector;
            int64_t shift = int64_t(root) - int64_t(sectorOfTable);
            std::vector<File> found;
            std::set<uint32_t> seen;
            if (shift < 0 || !Walk(v, shift, root, rootSize, "", 0, seen, found)) continue;
            for (auto& f : found) {
                if (f.path != "default.xbe") continue;
                uint8_t head[4];
                if (v.Read(f.at, head, 4) && !memcmp(head, "XBEH", 4)) {
                    files = std::move(found);
                    return shift;
                }
            }
        }
    }
    throw Error("found no game in the package: it isn't the digital release of the game");
}

fs::path ProgramDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return n ? fs::path(buf).parent_path() : fs::current_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : p.parent_path();
#endif
}

// the folder with Data0000 for whatever the user pointed at
fs::path FindData(const fs::path& given, fs::path& header) {
    std::error_code ec;
    if (fs::is_regular_file(given, ec)) {
        header = given;
        return fs::path(given) += ".data";
    }
    if (!fs::is_directory(given, ec)) throw Error("no such file or folder: " + Str(given));
    std::vector<fs::path> hits;
    for (fs::recursive_directory_iterator it(given, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it.depth() > 4) it.disable_recursion_pending();
        if (it->path().filename() == "Data0000" && it->is_regular_file(ec)) hits.push_back(it->path().parent_path());
    }
    std::sort(hits.begin(), hits.end());
    if (hits.empty()) throw Error("no DefaultPackage.data (Data0000, ...) in " + Str(given));
    if (hits[0].extension() == ".data") {
        fs::path h = hits[0];
        h.replace_extension();
        if (fs::is_regular_file(h, ec)) header = h;
    }
    return hits[0];
}

// the title the package header names, as a hint that it is the right package
std::string Title(const fs::path& header) {
    std::ifstream f(header, std::ios::binary);
    uint8_t h[0x1000];
    if (!f.read(reinterpret_cast<char*>(h), sizeof(h))) throw Error(Str(header) + " is not a package header");
    if (memcmp(h, "PIRS", 4) && memcmp(h, "LIVE", 4) && memcmp(h, "CON ", 4)) {
        throw Error(Str(header) + " is not a package header");
    }
    std::string title;
    for (int i = 0; i < 0x40; i++) {
        unsigned c = h[0x411 + i * 2] << 8 | h[0x411 + i * 2 + 1];
        if (!c) break;
        title += c < 0x80 ? char(c) : '?';
    }
    return title;
}

void Pause() {
#ifdef _WIN32
    // started by double click or drag and drop: the window would close before it can be read
    DWORD pid;
    if (GetConsoleProcessList(&pid, 1) == 1) {
        printf("\nPress Enter to close.\n");
        getchar();
    }
#endif
}

int Run(int argc, const std::vector<fs::path>& argv) {
    fs::path here = ProgramDir();
    fs::path assets = here / "assets";
    fs::path given = argc > 1 ? argv[1] : assets;
    fs::path out = argc > 2 ? argv[2] : assets;
    if (argc > 3 || (argc > 1 && (argv[1] == fs::path("-h") || argv[1] == fs::path("--help")))) {
        printf("usage: unpack-store [DefaultPackage] [outdir]\n"
               "  DefaultPackage  the package file, or the folder holding it (default: assets/ beside this program)\n"
               "  outdir          where default.xbe, dvddata/ and media/ go (default: assets/ beside this program)\n");
        return argc > 3 ? 2 : 0;
    }

    fs::path header;
    fs::path data = FindData(given, header);
    if (!header.empty() && fs::exists(header)) {
        std::string title = Title(header);
        printf("package: %s%s%s\n", Str(header).c_str(), title.empty() ? "" : "  ", title.c_str());
    }
    printf("data:    %s\n", Str(data).c_str());
    Volume v(data);

    uint8_t vd[kSector];
    if (!v.Read(0, vd, kSector) || memcmp(vd, kMagic, kMagicSize) || memcmp(vd + 0x7EC, kMagic, kMagicSize)) {
        throw Error("the package's data isn't a disc file system (is it a different kind of package?)");
    }
    uint32_t root = U32(vd + 0x14), rootSize = U32(vd + 0x18);

    printf("finding the game's files...\n");
    std::vector<File> files;
    int64_t shift = FindShift(v, root, rootSize, files);
    uint64_t total = 0;
    for (auto& f : files) total += f.size;
    printf("%zu files, %.2f GB, volume starts at disc sector %lld\n", files.size(), double(total) / 1e9,
           (long long)shift);

    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) throw Error("cannot create " + Str(out) + ": " + ec.message());
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.at < b.at; });  // one pass
    std::vector<uint8_t> buf(4u << 20);
    uint64_t written = 0;
    int shown = -1;
    for (auto& f : files) {
        fs::path dest = out / fs::path(f.path);
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream o(dest, std::ios::binary | std::ios::trunc);
        if (!o) throw Error("cannot write " + Str(dest));
        for (uint64_t pos = 0; pos < f.size;) {
            uint64_t n = std::min<uint64_t>(buf.size(), f.size - pos);
            if (!v.Read(f.at + pos, buf.data(), n)) throw Error("package ends inside " + f.path);
            if (!o.write(reinterpret_cast<char*>(buf.data()), std::streamsize(n))) {
                throw Error("cannot write " + Str(dest) + " (disk full?)");
            }
            pos += n;
            written += n;
            int pct = total ? int(written * 100 / total) : 100;
            if (pct != shown) {
                shown = pct;
                printf("\r%3d%%", pct);
                fflush(stdout);
            }
        }
    }
    printf("\r100%%\nunpacked into %s\n", Str(out).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // paths as the system holds them: wide on Windows, where argv is in the code page
    std::vector<fs::path> args;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int wide = 0;
    wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &wide);
    for (int i = 0; w && i < wide; i++) args.emplace_back(w[i]);
    if (w) LocalFree(w);
#else
    for (int i = 0; i < argc; i++) args.emplace_back(argv[i]);
#endif
    int code = 1;
    try {
        code = Run(int(args.size()), args);
    } catch (const std::exception& e) {
        fprintf(stderr, "\nunpack-store: %s\n", e.what());
    }
    Pause();
    return code;
}
