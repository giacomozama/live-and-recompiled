#pragma once
// The vertex programs on the game's disc, so the shaders shaders.rec names can be made before this machine has run
// them: Setup scans the player's copy once and writes disc_programs.bin beside the game, the game reads it (and makes
// it in the cache folder itself when it is missing, for a build Setup didn't make). Its words are the title's own,
// so the file is only ever made from the player's copy, never shipped.
//
// Programs reach the chip two ways, and both are kept: uploads in the push buffers of the asset packages
// (SET_TRANSFORM_PROGRAM packets of up to 8 instructions, one after the other, the last instruction marked final),
// and D3D vertex shader objects ((count << 16) | 0x2078, then the instructions) in packages and the xbe. The
// packages are Rare's CAFF files (aid\zpackage\...\default.rbm), their sections compressed in chunks of up to 64 KB;
// Decompress is the game's own routine (0x54AE40, its chunk loop 0x416170).
//
// Setup compiles this file too: it uses nothing of the runtime but kernel::Disc.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kernel {
class Disc;
}

namespace gpu::disc_programs {

using Program = std::vector<uint32_t>;  // 4 words an instruction, the last one final

// One chunk (the game's 0x54AE40): out_size bytes from in_size at in. False when the data is bad
bool DecompressChunk(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);
// A CAFF file's sections, decompressed and one after the other (the header left out); false when it isn't one
bool UnpackCaff(const std::vector<uint8_t>& file, std::vector<uint8_t>& out);
// Every program in data, in the order found (the same one may come more than once)
void FindPrograms(const uint8_t* data, size_t size, std::vector<Program>& out);

// a file of the game's: its path below the game's root, and its first `limit` bytes read (from any thread)
struct File {
    std::string path;
    std::function<bool(std::vector<uint8_t>& out, size_t limit)> read;
};
std::vector<File> FolderFiles(const std::filesystem::path& dir);  // the extracted files
std::vector<File> DiscFiles(const kernel::Disc& disc);             // a disc image's, while disc stays open
// default.xbe and every CAFF package among files, scanned on threads, each program once in the order of the files'
// paths; progress (files done, files) from any of them
std::vector<Program> Scan(const std::vector<File>& files, const std::function<void(size_t, size_t)>& progress = {});

constexpr const char* kFileName = "disc_programs.bin";
// the list, whole, written beside it then renamed over it
bool Write(const std::filesystem::path& path, const std::vector<Program>& programs);
// false when there is no file or it isn't one of these
bool Read(const std::filesystem::path& path, std::vector<Program>& programs);

}  // namespace gpu::disc_programs
