// shaders.rec and shader_programs.bin beside the exe, see shader_record.h
#include "gpu/shader_record.h"

#include <cstring>

#include "core/log.h"

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

FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
    std::vector<uint8_t> b;
    FILE* f = OpenFile(p, "rb");
    if (!f) return b;
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return b;
}

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

uint64_t Id(Kind kind, const void* p, size_t n) { return Hash(p, n, 0x5EC0'0000ull | kind); }

Recorder::~Recorder() {
    if (rec_) std::fclose(rec_);
    if (prog_) std::fclose(prog_);
}

void Recorder::Open(const std::filesystem::path& dir, uint64_t layout, bool write) {
    // shaders.rec: what each shader and pipeline was made from
    std::filesystem::path rec_path = dir / "shaders.rec";
    std::vector<uint8_t> b = ReadAll(rec_path);
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
            if (good + 3 + n > b.size() || kind < kVertex || kind > kPipeline) break;
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
    std::vector<uint8_t> pb = ReadAll(prog_path);
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

}  // namespace gpu::shader_record
