#pragma once
// The shaders and pipelines a run made, kept beside the exe so the next run can make them before the frames that
// need them. shaders.rec holds what each was made from and may be shared: vertex programs are named by hash only.
// Their microcode, the title's own, goes in shader_programs.bin, which stays on the machine that played.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/shaders.h"

namespace gpu::shader_record {

enum Kind : uint8_t { kVertex = 1, kPixel = 2, kPipeline = 3 };

// a vertex shader's inputs: VertexProgramHlsl's with a program, else FixedVertexHlsl's
struct VertexEntry {
    uint64_t program = 0;  // VertexShader::hash, 0 for the fixed function pipeline
    uint64_t decode = 0;
    FixedKey fixed;        // zero with a program
    uint16_t regs = 0;
    uint8_t pretransformed = 0;
    uint8_t quad = 0;      // QuadHlsl
    uint32_t pad = 0;
};
static_assert(sizeof(VertexEntry) == 48, "no implicit padding: the bytes are hashed and stored");

struct Entry {
    Kind kind;
    uint64_t id;
    std::vector<uint8_t> bytes;  // VertexEntry, PixelKey, or the backend's pipeline record
};
using Programs = std::unordered_map<uint64_t, std::vector<uint32_t>>;

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

}  // namespace gpu::shader_record
