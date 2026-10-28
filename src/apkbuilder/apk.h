#pragma once
// The APK (apk.cpp): the release's base APK, which has the app and no native code, with the player's libmain.so put
// in, signed with APK Signature Scheme v2 by a key of the player's own.
#include <cstdint>
#include <filesystem>
#include <string>

namespace apk {

// the signing key: a P-256 key and its self-signed certificate, made the first time and kept in path. Android installs
// a new build over the old one only when both are signed by the same key, so it is kept for the next build.
struct Key {
    uint8_t priv[32] = {};
    uint8_t pub[64] = {};  // x and y
    std::string cert;      // the certificate, DER
};
// reads the key at path, or makes one and writes it there; false with error when neither works
bool LoadOrMakeKey(const std::filesystem::path& path, Key& key, std::string& error);

// base with lib/arm64-v8a/libmain.so from so (stored, on a 16 KiB boundary, as Android maps it from the APK), every
// stored entry 4-byte aligned, written to out and signed with key
bool Build(const std::filesystem::path& base, const std::filesystem::path& so, const std::filesystem::path& out,
           const Key& key, std::string& error);

// SHA-256 of a file's contents, hex; empty when it cannot be read
std::string Sha256File(const std::filesystem::path& p);

}  // namespace apk
