// shaders.rec and shader_programs.bin in app::CacheDir, see shader_record.h
#include "gpu/shader_record.h"

#include <chrono>
#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "gpu/disc_programs.h"
#include "gpu/vertex.h"
#include "kernel/disc.h"

namespace gpu::shader_record {

namespace {

// shaders.rec: magic, version, the layout Open was given, then records of kind (1 byte), size (2) and bytes
constexpr char kRecMagic[4] = {'X', 'S', 'R', 'C'};
// shader_programs.bin: magic, version, then programs of hash (8 bytes), word count (4) and words
constexpr char kProgMagic[4] = {'X', 'S', 'V', 'P'};
constexpr uint32_t kVersion = 1;
constexpr size_t kRecHeader = 16;
constexpr size_t kProgHeader = 8;
constexpr uint32_t kMaxProgramWords = 136 * 4;  // the chip's program memory
// spirv.bin: magic, version, stamp (8 bytes), then blobs of hash (8), word count (4) and words
constexpr char kSpvMagic[4] = {'X', 'S', 'P', 'V'};
constexpr size_t kSpvHeader = 16;
constexpr uint32_t kMaxSpirvWords = 1u << 22;

FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

}  // namespace

std::vector<uint8_t> ReadFile(const std::filesystem::path& p) {
    std::vector<uint8_t> b;
    FILE* f = OpenFile(p, "rb");
    if (!f) return b;
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return b;
}

bool WriteFile(const std::filesystem::path& p, const void* data, size_t n) {
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    FILE* f = OpenFile(tmp, "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, n, f) == n;
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, p, ec);
    if (!ok || ec) std::filesystem::remove(tmp, ec);
    return ok && !ec;
}

namespace {

template <typename T>
T Read(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// the file for appending: as it is when all of it was good, else rewritten as its good part
FILE* Reopen(const std::filesystem::path& p, const std::vector<uint8_t>& old, size_t good, const void* header,
             size_t header_size) {
    if (good && good == old.size()) return OpenFile(p, "ab");
    FILE* f = OpenFile(p, "wb");
    if (!f) return nullptr;
    if (good) std::fwrite(old.data(), 1, good, f);
    else std::fwrite(header, 1, header_size, f);
    std::fflush(f);
    return f;
}

}  // namespace

bool WriteRecords(const std::filesystem::path& path, uint64_t layout, const std::vector<Entry>& entries) {
    std::vector<uint8_t> b(kRecHeader);
    std::memcpy(b.data(), kRecMagic, 4);
    std::memcpy(b.data() + 4, &kVersion, 4);
    std::memcpy(b.data() + 8, &layout, 8);
    for (const Entry& e : entries) {
        if (e.bytes.size() > UINT16_MAX) continue;
        const uint16_t n = uint16_t(e.bytes.size());
        b.push_back(e.kind);
        b.insert(b.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 2);
        b.insert(b.end(), e.bytes.begin(), e.bytes.end());
    }
    return WriteFile(path, b.data(), b.size());
}

bool ReadRecords(const std::filesystem::path& path, uint64_t layout, std::vector<Entry>& entries) {
    const std::vector<uint8_t> b = ReadFile(path);
    return ReadRecords(b.data(), b.size(), layout, entries);
}

bool ReadRecords(const uint8_t* b, size_t size, uint64_t layout, std::vector<Entry>& entries) {
    entries.clear();
    uint8_t header[kRecHeader];
    std::memcpy(header, kRecMagic, 4);
    std::memcpy(header + 4, &kVersion, 4);
    std::memcpy(header + 8, &layout, 8);
    if (!b || size < kRecHeader || std::memcmp(b, header, kRecHeader)) return false;
    for (size_t at = kRecHeader; at + 3 <= size;) {
        const Kind kind = Kind(b[at]);
        const uint16_t n = Read<uint16_t>(&b[at + 1]);
        if (at + 3 + n > size || kind < kVertex || kind > kMotionPipeline) break;
        const uint8_t* p = &b[at + 3];
        entries.push_back({kind, Id(kind, p, n), std::vector<uint8_t>(p, p + n)});
        at += 3 + n;
    }
    return true;
}

void AddDiscPrograms(Programs& programs) {
    namespace dp = disc_programs;
    std::vector<dp::Program> list;
    const std::filesystem::path setup = app::ExeDir() / dp::kFileName, cached = app::CacheDir() / dp::kFileName;
    if (!dp::Read(setup, list) && !dp::Read(cached, list)) {
        auto t = std::chrono::steady_clock::now();
        if (const kernel::Disc* disc = kernel::MountedDisc()) list = dp::Scan(dp::DiscFiles(*disc));
        else if (!app::GameDir().empty()) list = dp::Scan(dp::FolderFiles(app::GameDir()));
        // a second instance on this PC reads only, as with the records
        const bool wrote = !list.empty() && app::Instance() <= 1 && dp::Write(cached, list);
        LOGR("shaders: found %zu vertex programs in the game's files in %.0f ms%s", list.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(),
             wrote ? ", kept in the cache folder" : "");
    }
    size_t added = 0;
    for (dp::Program& p : list) {
        VertexShader vs;
        vs.program = p;
        HashProgram(vs);
        added += programs.emplace(ChipProgramHash(p.data(), p.size()), p).second;
        added += programs.emplace(vs.hash, std::move(p)).second;
    }
    LOGI("shaders: %zu vertex programs from the game's disc, %zu new names", list.size(), added);
}

uint64_t Id(Kind kind, const void* p, size_t n) { return Hash(p, n, 0x5EC0'0000ull | kind); }

Recorder::~Recorder() {
    if (rec_) std::fclose(rec_);
    if (prog_) std::fclose(prog_);
}

void Recorder::Open(const std::filesystem::path& dir, uint64_t layout, bool write) {
    // shaders.rec: what each shader and pipeline was made from
    std::filesystem::path rec_path = dir / "shaders.rec";
    std::vector<uint8_t> b = ReadFile(rec_path);
    uint8_t header[kRecHeader];
    std::memcpy(header, kRecMagic, 4);
    std::memcpy(header + 4, &kVersion, 4);
    std::memcpy(header + 8, &layout, 8);
    size_t good = 0;
    if (b.size() >= kRecHeader && !std::memcmp(b.data(), header, kRecHeader)) {
        good = kRecHeader;
        while (good + 3 <= b.size()) {
            Kind kind = Kind(b[good]);
            uint16_t n = Read<uint16_t>(&b[good + 1]);
            if (good + 3 + n > b.size() || kind < kVertex || kind > kMotionPipeline) break;
            const uint8_t* p = &b[good + 3];
            uint64_t id = Id(kind, p, n);
            if (ids_.insert(id).second) entries_.push_back({kind, id, std::vector<uint8_t>(p, p + n)});
            good += 3 + n;
        }
    } else if (!b.empty()) {
        LOGI("shaders: %s is from another build%s", rec_path.string().c_str(), write ? ", starting it over" : "");
    }
    // shader_programs.bin: the vertex programs those name
    std::filesystem::path prog_path = dir / "shader_programs.bin";
    std::vector<uint8_t> pb = ReadFile(prog_path);
    uint8_t pheader[kProgHeader];
    std::memcpy(pheader, kProgMagic, 4);
    std::memcpy(pheader + 4, &kVersion, 4);
    size_t pgood = 0;
    if (pb.size() >= kProgHeader && !std::memcmp(pb.data(), pheader, kProgHeader)) {
        pgood = kProgHeader;
        while (pgood + 12 <= pb.size()) {
            uint64_t hash = Read<uint64_t>(&pb[pgood]);
            uint32_t n = Read<uint32_t>(&pb[pgood + 8]);
            if (n > kMaxProgramWords || pgood + 12 + size_t(n) * 4 > pb.size()) break;
            std::vector<uint32_t> words(n);
            std::memcpy(words.data(), &pb[pgood + 12], size_t(n) * 4);
            if (program_ids_.insert(hash).second) programs_.emplace(hash, std::move(words));
            pgood += 12 + size_t(n) * 4;
        }
    }
    LOGI("shaders: %zu recorded shaders and pipelines, %zu vertex programs", entries_.size(), programs_.size());
    if (!write) return;
    rec_ = Reopen(rec_path, b, good, header, sizeof(header));
    prog_ = Reopen(prog_path, pb, pgood, pheader, sizeof(pheader));
    if (!rec_ || !prog_) LOGW("shaders: can't write %s", dir.string().c_str());
}

void Recorder::Add(Kind kind, const void* p, size_t n) {
    if (!ids_.insert(Id(kind, p, n)).second || !rec_ || n > UINT16_MAX) return;
    uint16_t size = uint16_t(n);
    std::fwrite(&kind, 1, 1, rec_);
    std::fwrite(&size, 2, 1, rec_);
    std::fwrite(p, 1, n, rec_);
    std::fflush(rec_);
}

void Recorder::AddProgram(uint64_t hash, const std::vector<uint32_t>& words) {
    if (!program_ids_.insert(hash).second || !prog_ || words.size() > kMaxProgramWords) return;
    uint32_t n = uint32_t(words.size());
    std::fwrite(&hash, 8, 1, prog_);
    std::fwrite(&n, 4, 1, prog_);
    std::fwrite(words.data(), 4, n, prog_);
    std::fflush(prog_);
}

SpirvCache::~SpirvCache() {
    if (f_) std::fclose(f_);
}

void SpirvCache::Open(const std::filesystem::path& dir, uint64_t stamp, bool write) {
    std::lock_guard<std::mutex> g(lock_);
    std::filesystem::path path = dir / "spirv.bin";
    std::vector<uint8_t> b = ReadFile(path);
    uint8_t header[kSpvHeader];
    std::memcpy(header, kSpvMagic, 4);
    std::memcpy(header + 4, &kVersion, 4);
    std::memcpy(header + 8, &stamp, 8);
    size_t good = 0;
    if (b.size() >= kSpvHeader && !std::memcmp(b.data(), header, kSpvHeader)) {
        good = kSpvHeader;
        while (good + 12 <= b.size()) {
            uint64_t hash = Read<uint64_t>(&b[good]);
            uint32_t n = Read<uint32_t>(&b[good + 8]);
            if (!n || n > kMaxSpirvWords || good + 12 + size_t(n) * 4 > b.size()) break;
            std::vector<uint32_t> words(n);
            std::memcpy(words.data(), &b[good + 12], size_t(n) * 4);
            blobs_.emplace(hash, std::move(words));
            good += 12 + size_t(n) * 4;
        }
    } else if (!b.empty()) {
        LOGI("shaders: %s is from another compiler or device%s", path.string().c_str(), write ? ", starting it over" : "");
    }
    LOGI("shaders: %zu compiled shaders in spirv.bin", blobs_.size());
    if (!write) return;
    f_ = Reopen(path, b, good, header, sizeof(header));
    if (!f_) LOGW("shaders: can't write %s", path.string().c_str());
}

bool SpirvCache::Find(uint64_t hash, std::vector<uint32_t>& out) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = blobs_.find(hash);
    if (it == blobs_.end()) return false;
    out = it->second;
    return true;
}

void SpirvCache::Add(uint64_t hash, const std::vector<uint32_t>& words) {
    if (words.empty() || words.size() > kMaxSpirvWords) return;
    std::lock_guard<std::mutex> g(lock_);
    if (!blobs_.emplace(hash, words).second || !f_) return;
    uint32_t n = uint32_t(words.size());
    std::fwrite(&hash, 8, 1, f_);
    std::fwrite(&n, 4, 1, f_);
    std::fwrite(words.data(), 4, n, f_);
    std::fflush(f_);
}

size_t SpirvCache::Size() {
    std::lock_guard<std::mutex> g(lock_);
    return blobs_.size();
}

}  // namespace gpu::shader_record
