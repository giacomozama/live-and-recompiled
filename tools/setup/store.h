#pragma once

// The digital, backwards compatible release's package (store.cpp), unpacked into the files Setup reads.
#include <filesystem>
#include <string>

namespace store {

using Say = void (*)(const char* fmt, ...);

// a folder holding DefaultPackage beside its DefaultPackage.data folder
bool HasPackage(const std::filesystem::path& dir);

// unpacks the package at `given` (DefaultPackage, the folder holding it, or its .data folder) into `out`, telling
// progress through `say`; false with `error` set when it isn't a package of the game or cannot be written
bool Unpack(const std::filesystem::path& given, const std::filesystem::path& out, Say say, std::string& error);

}  // namespace store
