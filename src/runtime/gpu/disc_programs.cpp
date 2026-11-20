// disc_programs.bin from the game's files, see disc_programs.h
#include "gpu/disc_programs.h"

#include "kernel/disc.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>

namespace gpu::disc_programs {

namespace fs = std::filesystem;

namespace {

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// A chunk's code (0x54ADxx, the part 0x54AE40 calls first): 256 bytes of 4 bit lengths, symbol 2i in byte i's low
// nibble and 2i+1 in its high one, for 512 symbols (bytes, then 0x100 + matches). Canonical, shorter codes first and
// lower symbols first within a length. The table is indexed by the next 10 bits: a symbol (its number << 4 | its
// length) or, for a longer code, 0x8000 | the first of two entries at 0x400 and up, picked by the next bit, and so on
// down to 15 bits. False when the lengths aren't a whole prefix code
bool BuildTable(const uint8_t* lens, int16_t* table) {
    int count[16] = {};
    for (int i = 0; i < 256; i++) {
        count[lens[i] & 15]++;
        count[lens[i] >> 4]++;
    }
    if (count[0] == 512) return true;  // no symbols: the game leaves its table as it was
    if (count[0] == 511) return false;
    int per_length[16];
    std::memcpy(per_length, count, sizeof(count));
    int left = 0;
    for (int l = 15; l >= 1; l--) {
        left += count[l];
        if (left & 1) return false;
        left >>= 1;
    }
    if (left != 1) return false;
    for (int l = 2; l < 16; l++) count[l] += count[l - 1];
    int n = count[15];  // symbols with a code, sorted by length then number
    int16_t sorted[512];
    for (int sym = 511; sym >= 0; sym--) {
        int len = sym & 1 ? lens[sym >> 1] >> 4 : lens[sym >> 1] & 15;
        if (len) sorted[--count[len]] = int16_t(sym << 4 | len);
    }
    // codes of 11 to 15 bits, longest first, from the top of the table down: each length's leaves below the nodes
    // over the length above's
    int cur = 0x800, top = 0x800;
    for (int l = 15; l > 10; l--) {
        int mark = cur;
        while (top > mark) {
            top -= 2;
            table[--cur] = int16_t(0x8000 | top);
        }
        for (int k = 0; k < per_length[l]; k++) table[--cur] = sorted[--n];
        top = mark;
    }
    // the 10 bit entries: nodes over the 11 bit level at the top, then the shorter codes, each over 0x400 >> length
    int p = 0x400;
    while (top > cur) {
        top -= 2;
        table[--p] = int16_t(0x8000 | top);
    }
    while (n > 0) {
        int16_t e = sorted[--n];
        for (int k = 0x400 >> (e & 15); k; k--) table[--p] = e;
    }
    return true;
}

}  // namespace

bool DecompressChunk(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
    // 0x54AE40's own limits; a chunk stored as it is (in_size == out_size) its caller copies
    if (out_size <= 0x105 || out_size > 0x10000 || in_size < 0x105 || out_size < in_size) return false;
    int16_t table[0x800] = {};
    if (!BuildTable(in, table)) return false;
    // the bits: 16 bit words, the top bit first, in a 32 bit window that keeps at least 16 ahead (16 + spare)
    const uint8_t *p = in + 0x100, *end = in + in_size;
    uint32_t bits = Rd16(p) << 16 | Rd16(p + 2);
    p += 4;
    int spare = 16;
    auto refill = [&] {
        if (spare >= 0) return true;
        if (p + 2 > end) return false;
        bits += Rd16(p) << -spare;
        p += 2;
        spare += 16;
        return true;
    };
    uint8_t *o = out, *oend = out + out_size;
    for (;;) {
        int32_t e = table[bits >> 22];
        if (e >= 0) {
            bits <<= e & 15;
        } else {
            bits <<= 10;
            do {
                e = table[(e & 0x7FFF) + (bits >> 31)];
                bits <<= 1;
            } while (e < 0);
        }
        spare -= e & 15;
        const int sym = e >> 4;
        if (!refill()) return false;
        // full: whole only when 0x100 (a match of nothing) says it ends there
        if (o >= oend) return sym == 0x100;
        if (sym < 0x100) {
            *o++ = uint8_t(sym);
            continue;
        }
        // a match: the distance's bit count in the high nibble (the distance 1 << that, plus that many bits), the
        // length less 3 in the low one, 15 for a byte beside the bits (0xFF for a 16 bit length after it, 0x10E up)
        const int m = sym - 0x100, nb = m >> 4, lc = m & 15;
        const size_t dist = (size_t(1) << nb) | (nb ? bits >> (32 - nb) : 0);
        bits <<= nb;
        spare -= nb;
        size_t n = size_t(lc);
        if (lc == 15) {
            if (p >= end) return false;
            n = size_t(*p++) + 15;
            if (n == 0xFF + 15) {
                if (p + 2 > end) return false;
                n = Rd16(p);
                p += 2;
                if (n < 0x10E) return false;
            }
        }
        n += 3;
        if (dist > size_t(o - out)) return false;  // before the chunk: each chunk stands alone
        if (n > size_t(oend - o)) return false;    // past the end (the game copies up to it and gives up)
        for (const uint8_t* s = o - dist; n; n--) *o++ = *s++;
        if (!refill()) return false;
    }
}

bool UnpackCaff(const std::vector<uint8_t>& file, std::vector<uint8_t>& out) {
    // the retail header: "CAFF", the version, ..., the data's offset at 0x38, the section count at 0x3D and the
    // compressed flag at 0x3E; then 0x28 bytes a section, its size at 0x10 and its compressed size at 0x24. The
    // sections follow one another from the data's offset, each as 8 byte headers (size, stored size) and chunks
    if (file.size() < 0x40 || std::memcmp(file.data(), "CAFF", 4)) return false;
    const uint32_t data = Rd32(&file[0x38]);
    const unsigned sections = file[0x3D];
    const bool compressed = file[0x3E] != 0;
    if (0x40 + size_t(sections) * 0x28 > file.size()) return false;
    out.clear();
    size_t src = data;
    for (unsigned i = 0; i < sections; i++) {
        const uint8_t* info = &file[0x40 + i * 0x28];
        const size_t size = Rd32(info + 0x10), stored = compressed ? Rd32(info + 0x24) : size;
        if (src > file.size() || stored > file.size() - src || size > (size_t(1) << 31)) return false;
        const size_t at = out.size();
        out.resize(at + size);
        if (!compressed) {
            std::memcpy(&out[at], &file[src], size);
        } else {
            size_t s = src, o = at;
            const size_t s_end = src + stored, o_end = at + size;
            while (s + 8 <= s_end) {
                const size_t osz = Rd32(&file[s]), isz = Rd32(&file[s + 4]);
                s += 8;
                if (isz > s_end - s || osz > o_end - o) break;
                // a chunk that won't unpack stays zeros, as the game leaves it
                if (osz == isz) std::memcpy(&out[o], &file[s], osz);
                else DecompressChunk(&file[s], isz, &out[o], osz);
                s += isz;
                o += osz;
            }
        }
        src += stored;
    }
    return true;
}

namespace {

constexpr uint32_t kMaxInstructions = 136;  // the chip's program memory

bool IsFinal(const uint32_t* insn) { return insn[3] & 1; }

// a NV097 header of incrementing SET_TRANSFORM_PROGRAM words (method 0xB00, subchannel 0) for whole instructions
bool ProgramPacket(uint32_t h, uint32_t& count) {
    count = (h >> 18) & 0x7FF;
    return (h & 0xE003FFFF) == 0xB00 && count && count <= 32 && count % 4 == 0;
}

// the run's programs: from its start or a final instruction to the next final one
void SplitAtFinals(const std::vector<uint32_t>& run, std::vector<Program>& out) {
    size_t from = 0;
    for (size_t i = 0; i + 4 <= run.size(); i += 4) {
        if (!IsFinal(&run[i])) continue;
        if ((i + 4 - from) / 4 <= kMaxInstructions) out.emplace_back(run.begin() + from, run.begin() + i + 4);
        from = i + 4;
    }
}

}  // namespace

void FindPrograms(const uint8_t* data, size_t size, std::vector<Program>& out) {
    std::vector<uint32_t> words, run;
    for (size_t align = 0; align < 4 && align < size; align++) {
        const size_t n = (size - align) / 4;
        words.resize(n);
        std::memcpy(words.data(), data + align, n * 4);
        for (size_t i = 0; i < n;) {
            const uint32_t h = words[i];
            uint32_t count;
            if (ProgramPacket(h, count)) {
                // packets one after the other, each starting again at 0xB00 with the load pointer carried on
                run.clear();
                size_t k = i;
                while (k < n && ProgramPacket(words[k], count) && k + 1 + count <= n) {
                    run.insert(run.end(), words.begin() + k + 1, words.begin() + k + 1 + count);
                    k += 1 + count;
                }
                if (k > i) {
                    SplitAtFinals(run, out);
                    i = k;
                    continue;
                }
            } else if ((h & 0xFFFF) == 0x2078) {
                // a D3D vertex shader: its instructions, the last (alone) final
                const uint32_t count = h >> 16;
                if (count && count <= kMaxInstructions && i + 1 + size_t(count) * 4 <= n) {
                    const uint32_t* p = &words[i + 1];
                    bool ok = IsFinal(p + (count - 1) * 4);
                    for (uint32_t j = 0; ok && j + 1 < count; j++) ok = !IsFinal(p + j * 4);
                    if (ok) out.emplace_back(p, p + size_t(count) * 4);
                }
            }
            i++;
        }
    }
}

namespace {

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out, size_t limit = SIZE_MAX) {
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out.clear();
    uint8_t buf[1 << 16];
    for (size_t got; out.size() < limit && (got = std::fread(buf, 1, std::min(sizeof(buf), limit - out.size()), f)) > 0;)
        out.insert(out.end(), buf, buf + got);
    std::fclose(f);
    return true;
}

}  // namespace

std::vector<File> FolderFiles(const fs::path& dir) {
    std::vector<File> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        fs::path p = it->path();
        files.push_back({p.lexically_relative(dir).generic_string(),
                         [p](std::vector<uint8_t>& out, size_t limit) { return ReadAll(p, out, limit); }});
    }
    return files;
}

std::vector<File> DiscFiles(const kernel::Disc& disc) {
    std::vector<File> files;
    std::function<void(const kernel::Disc::Entry&, const std::string&)> walk = [&](const kernel::Disc::Entry& dir,
                                                                                   const std::string& at) {
        for (const auto& e : dir.children) {
            std::string path = at.empty() ? e.name : at + "/" + e.name;
            if (e.dir) {
                walk(e, path);
                continue;
            }
            const kernel::Disc::Entry* entry = &e;
            files.push_back({path, [&disc, entry](std::vector<uint8_t>& out, size_t limit) {
                                 out.resize(std::min<size_t>(entry->size, limit));
                                 return disc.Read(*entry, 0, out.data(), uint32_t(out.size())) == out.size();
                             }});
        }
    };
    if (const kernel::Disc::Entry* root = disc.Find(std::vector<std::string>{})) walk(*root, "");
    return files;
}

std::vector<Program> Scan(const std::vector<File>& all, const std::function<void(size_t, size_t)>& progress) {
    // default.xbe as it is, and every CAFF file (by its magic) unpacked
    std::vector<const File*> files;
    std::vector<uint8_t> head;
    for (const File& f : all) {
        const bool xbe = f.path == "default.xbe" || (f.path.size() > 12 && f.path.compare(f.path.size() - 12, 12, "/default.xbe") == 0);
        if (xbe || (f.read(head, 4) && head.size() == 4 && !std::memcmp(head.data(), "CAFF", 4))) files.push_back(&f);
    }
    std::sort(files.begin(), files.end(), [](const File* a, const File* b) { return a->path < b->path; });
    std::vector<std::vector<Program>> found(files.size());
    std::atomic<size_t> next{0}, done{0};
    std::mutex progress_lock;
    auto work = [&] {
        std::vector<uint8_t> file, data;
        for (size_t i; (i = next++) < files.size();) {
            if (files[i]->read(file, SIZE_MAX)) {
                if (UnpackCaff(file, data)) FindPrograms(data.data(), data.size(), found[i]);
                else FindPrograms(file.data(), file.size(), found[i]);
            }
            const size_t d = ++done;
            if (progress) {
                std::lock_guard<std::mutex> g(progress_lock);
                progress(d, files.size());
            }
        }
    };
    // each thread holds a package and what it unpacks to (up to some 100 MB): a few are enough for the disc
    const unsigned threads = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    std::vector<Program> programs;
    std::set<Program> seen;
    for (auto& list : found)
        for (auto& prog : list)
            if (seen.insert(prog).second) programs.push_back(std::move(prog));
    return programs;
}

namespace {

// magic, version, program count, then each program's word count and words
constexpr char kMagic[4] = {'X', 'D', 'P', 'G'};
constexpr uint32_t kVersion = 1;

}  // namespace

bool Write(const fs::path& path, const std::vector<Program>& programs) {
    std::vector<uint8_t> b(kMagic, kMagic + 4);
    auto put = [&](uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
    put(kVersion);
    put(uint32_t(programs.size()));
    for (const Program& p : programs) {
        put(uint32_t(p.size()));
        b.insert(b.end(), reinterpret_cast<const uint8_t*>(p.data()), reinterpret_cast<const uint8_t*>(p.data() + p.size()));
    }
    fs::path tmp = path;
    tmp += ".tmp";
#ifdef _WIN32
    FILE* f = _wfopen(tmp.c_str(), L"wb");
#else
    FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) fs::rename(tmp, path, ec);
    if (!ok || ec) fs::remove(tmp, ec);
    return ok && !ec;
}

bool Read(const fs::path& path, std::vector<Program>& programs) {
    std::vector<uint8_t> b;
    if (!ReadAll(path, b) || b.size() < 12 || std::memcmp(b.data(), kMagic, 4) || Rd32(&b[4]) != kVersion) return false;
    const uint32_t count = Rd32(&b[8]);
    size_t at = 12;
    programs.clear();
    for (uint32_t i = 0; i < count; i++) {
        if (at + 4 > b.size()) return false;
        const uint32_t n = Rd32(&b[at]);
        at += 4;
        if (n % 4 || n / 4 > kMaxInstructions || n * size_t(4) > b.size() - at) return false;
        Program p(n);
        std::memcpy(p.data(), &b[at], size_t(n) * 4);
        at += size_t(n) * 4;
        programs.push_back(std::move(p));
    }
    return true;
}

}  // namespace gpu::disc_programs
