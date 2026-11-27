#pragma once
// The shaders and pipelines a run made, kept in app::CacheDir so the next run can make them before the frames that
// need them. shaders.rec holds what each was made from and may be shared: vertex programs are named by hash only.
// Their microcode, the title's own, goes in shader_programs.bin, which stays on the machine that played.
// spirv.bin keeps what glslang made of each source and pipelines.vkc the driver's pipeline cache, so a run after the
// first makes neither again.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/shaders.h"

namespace gpu::shader_record {

// kMotion*: the motion pass's (MotionVertexEntry, MotionPixelEntry, MotionPipelineEntry in gpu/draw.h)
enum Kind : uint8_t { kVertex = 1, kPixel = 2, kPipeline = 3, kMotionVertex = 4, kMotionPixel = 5, kMotionPipeline = 6 };

// a vertex shader's inputs: VertexProgramHlsl's with a program, else FixedVertexHlsl's
struct VertexEntry {
    uint64_t program = 0;  // VertexShader::hash, 0 for the fixed function pipeline
    uint64_t decode = 0;
    FixedKey fixed;        // zero with a program
    uint16_t regs = 0;
    uint8_t pretransformed = 0;
    uint8_t quad = 0;      // QuadHlsl
    uint8_t shading = 0;   // g_program_shading's entry, 1 based, 0 none
    uint8_t pad[3] = {};
};
static_assert(sizeof(VertexEntry) == 48, "no implicit padding: the bytes are hashed and stored");

struct Entry {
    Kind kind;
    uint64_t id;
    std::vector<uint8_t> bytes;  // VertexEntry, PixelKey, or the backend's pipeline record
};
using Programs = std::unordered_map<uint64_t, std::vector<uint32_t>>;

// the warm-up's progress, for the overlay that says it is at work (any thread): making the disc's records (no count:
// another process does it), or compiling `total` shaders and pipelines, `done` of them through
enum class Work : uint32_t { None, Generating, Compiling };
inline std::atomic<Work> g_work{Work::None};
inline std::atomic<uint32_t> g_work_done{0}, g_work_total{0};
inline void SetWork(Work w, uint32_t total = 0) {
    g_work_done.store(0);
    g_work_total.store(total);
    g_work.store(w);
}

// set as the settings menu shuts (any thread): the renderer warms up again if the settings now draw with other shaders
inline std::atomic<bool> g_settings_closed{false};

// the vertex programs on the game's disc (gpu/disc_programs.h) added to programs, each by both names a program goes
// by (ChipProgramHash, HashProgram's), where programs has none yet: disc_programs.bin beside the exe (Setup's), else
// in app::CacheDir, else the game's files scanned and written there. Slow when it scans: off the title's thread
void AddDiscPrograms(Programs& programs);

// A whole records file in shaders.rec's format (gpu/shader_gen.h writes one): Write replaces path with entries,
// Read gives its records, false when it is missing or written with another layout
bool WriteRecords(const std::filesystem::path& path, uint64_t layout, const std::vector<Entry>& entries);
bool ReadRecords(const std::filesystem::path& path, uint64_t layout, std::vector<Entry>& entries);
// the same from a file's bytes in memory (app::GameInfo::base_shaders)
bool ReadRecords(const uint8_t* data, size_t size, uint64_t layout, std::vector<Entry>& entries);

// a record's name, how a pipeline names its shaders
uint64_t Id(Kind kind, const void* p, size_t n);

class Recorder {
public:
    ~Recorder();
    // reads both files from dir; layout names the record layouts, a file written with others starts over.
    // write false reads only (a second instance on this PC)
    void Open(const std::filesystem::path& dir, uint64_t layout, bool write);
    // as read, in the order first seen; Take hands them over once
    std::vector<Entry> TakeEntries() { return std::move(entries_); }
    Programs TakePrograms() { return std::move(programs_); }
    // appends a record not there yet
    void Add(Kind kind, const void* p, size_t n);
    void AddProgram(uint64_t hash, const std::vector<uint32_t>& words);

private:
    std::vector<Entry> entries_;
    Programs programs_;
    std::unordered_set<uint64_t> ids_;
    std::unordered_set<uint64_t> program_ids_;
    FILE* rec_ = nullptr;
    FILE* prog_ = nullptr;
};

// spirv.bin: SPIR-V by SourceHash, for one compiler and SPIR-V target (stamp); a file with another stamp starts over.
// Find and Add from any thread
class SpirvCache {
public:
    ~SpirvCache();
    void Open(const std::filesystem::path& dir, uint64_t stamp, bool write);
    bool Find(uint64_t hash, std::vector<uint32_t>& out);
    void Add(uint64_t hash, const std::vector<uint32_t>& words);
    size_t Size();

private:
    std::mutex lock_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> blobs_;
    FILE* f_ = nullptr;
};

// a whole file, empty when there is none
std::vector<uint8_t> ReadFile(const std::filesystem::path& p);
// the file replaced whole: written beside it, then renamed over it
bool WriteFile(const std::filesystem::path& p, const void* data, size_t n);

}  // namespace gpu::shader_record
