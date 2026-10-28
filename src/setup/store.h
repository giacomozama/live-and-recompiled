#pragma once

// The digital, backwards compatible release's package (store.cpp), unpacked into the files Setup reads.
#include <filesystem>
#include <string>
#include <vector>

namespace store {

using Say = void (*)(const char* fmt, ...);

// a folder holding DefaultPackage beside its DefaultPackage.data folder
bool HasPackage(const std::filesystem::path& dir);

// unpacks the package at `given` (DefaultPackage, the folder holding it, or its .data folder) into `out`, telling
// progress through `say`; false with `error` set when it isn't a package of the game or cannot be written
bool Unpack(const std::filesystem::path& given, const std::filesystem::path& out, Say say, std::string& error);

// one file of the package (a path in it, '/' separated, any case) into out, without unpacking the rest; false with
// error set when the package or the file isn't there
bool LoadFile(const std::filesystem::path& given, const std::string& name, std::vector<uint8_t>& out,
              std::string& error);

#ifndef _WIN32
// the same from the package's Data0000, Data0001, ... as open file descriptors, in order (Android's document picker)
bool UnpackFds(const std::vector<int>& data, const std::filesystem::path& out, Say say, std::string& error);
#endif

}  // namespace store
