// The APK: the base APK's entries copied as they are (compressed ones as they were compressed, stored ones moved to a
// 4-byte boundary, as Android 11 wants of resources.arsc), libmain.so added stored on a 16 KiB boundary (the app maps
// it from the APK, extractNativeLibs is false, and 16 KiB covers devices with large pages), then signed.
//
// The signature is APK Signature Scheme v2 (the app's minSdk is 28, so no v1 JAR signature): a block between the
// entries and the central directory with SHA-256 digests of the file's three other parts in 1 MiB chunks, the
// player's certificate, and an ECDSA P-256 signature over those (micro-ecc). Layout and digests as
// source.android.com/docs/security/features/apksigning/v2 has them; apksigner verify accepts the result.
#include "apk.h"

#include <miniz.h>
#include <toml++/toml.hpp>
#include <uECC.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace apk {

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// SHA-256, fed in pieces
class Sha256 {
public:
    void Update(const void* data, size_t n) {
        auto* p = static_cast<const uint8_t*>(data);
        total_ += n;
        while (n) {
            size_t take = std::min(n, size_t(64) - used_);
            std::memcpy(buf_ + used_, p, take);
            used_ += take;
            p += take;
            n -= take;
            if (used_ == 64) {
                Block(buf_);
                used_ = 0;
            }
        }
    }
    void Final(uint8_t out[32]) {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        pad = 0;
        while (used_ != 56) Update(&pad, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        Update(len, 8);
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = uint8_t(h_[i] >> (24 - 8 * j));
    }

private:
    void Block(const uint8_t* b) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], c1 = h_[1], c2 = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & c1) ^ (a & c2) ^ (c1 & c2));
            hh = g, g = f, f = e, e = d + t1, d = c2, c2 = c1, c1 = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += c1, h_[2] += c2, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += hh;
    }

    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf_[64] = {};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

Bytes Sha(const Bytes& data) {
    Sha256 s;
    s.Update(data.data(), data.size());
    Bytes out(32);
    s.Final(out.data());
    return out;
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += "0123456789abcdef"[p[i] >> 4];
        s += "0123456789abcdef"[p[i] & 15];
    }
    return s;
}

bool Unhex(const std::string& s, Bytes& out) {
    if (s.size() % 2) return false;
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int h = nib(s[i]), l = nib(s[i + 1]);
        if (h < 0 || l < 0) return false;
        out.push_back(uint8_t(h << 4 | l));
    }
    return true;
}

// the system's random bytes, for the key and the signatures' nonces; micro-ecc's RNG
int Random(uint8_t* dest, unsigned size) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, dest, ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    while (size) {
        ssize_t n = getrandom(dest, size, 0);
        if (n <= 0) return 0;
        dest += n;
        size -= unsigned(n);
    }
    return 1;
#endif
}

// DER: a tag and its contents
Bytes Der(uint8_t tag, const Bytes& body) {
    Bytes out{tag};
    size_t n = body.size();
    if (n < 0x80) {
        out.push_back(uint8_t(n));
    } else {
        Bytes len;
        for (; n; n >>= 8) len.insert(len.begin(), uint8_t(n));
        out.push_back(uint8_t(0x80 | len.size()));
        out.insert(out.end(), len.begin(), len.end());
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
Bytes Cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
Bytes Seq(std::initializer_list<Bytes> parts) { return Der(0x30, Cat(parts)); }
// an unsigned big-endian number as an INTEGER: no leading zeros, one added when the top bit is set
Bytes Integer(const uint8_t* p, size_t n) {
    while (n > 1 && !*p) p++, n--;
    Bytes body;
    if (*p & 0x80) body.push_back(0);
    body.insert(body.end(), p, p + n);
    return Der(0x02, body);
}
Bytes Text(uint8_t tag, const char* s) { return Der(tag, Bytes(s, s + std::strlen(s))); }

const Bytes kEcdsaSha256 = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};  // 1.2.840.10045.4.3.2
const Bytes kEcPublicKey = {0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};        // 1.2.840.10045.2.1
const Bytes kPrime256v1 = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};   // 1.2.840.10045.3.1.7
const Bytes kCommonName = {0x06, 0x03, 0x55, 0x04, 0x03};                                 // 2.5.4.3

// the public key as X.509 has it, and as the v2 signer's public key field
Bytes PublicKeyInfo(const Key& k) {
    Bytes point{0x00, 0x04};  // a BIT STRING with no unused bits, an uncompressed point
    point.insert(point.end(), k.pub, k.pub + 64);
    return Seq({Seq({kEcPublicKey, kPrime256v1}), Der(0x03, point)});
}

// ECDSA P-256 over SHA-256 of data, DER as X.509 and APK signatures have it
bool Sign(const Key& k, const Bytes& data, Bytes& out) {
    Bytes digest = Sha(data);
    uint8_t rs[64];
    if (!uECC_sign(k.priv, digest.data(), unsigned(digest.size()), rs, uECC_secp256r1())) return false;
    out = Seq({Integer(rs, 32), Integer(rs + 32, 32)});
    return true;
}

// a self-signed certificate for the key; Android reads only the key in it, the names and dates are for people
bool MakeCertificate(Key& k) {
    uint8_t serial[8];
    if (!Random(serial, sizeof(serial))) return false;
    serial[0] &= 0x7F;
    Bytes name = Seq({Der(0x31, Seq({kCommonName, Text(0x0C, "Live & Recompiled (this PC's build)")}))});
    Bytes tbs = Seq({
        Der(0xA0, Integer((const uint8_t*)"\x02", 1)),  // v3
        Integer(serial, sizeof(serial)),
        Seq({kEcdsaSha256}),
        name,
        Seq({Text(0x17, "260101000000Z"), Text(0x18, "20760101000000Z")}),
        name,
        PublicKeyInfo(k),
    });
    Bytes sig;
    if (!Sign(k, tbs, sig)) return false;
    sig.insert(sig.begin(), 0x00);  // a BIT STRING
    Bytes cert = Seq({tbs, Seq({kEcdsaSha256}), Der(0x03, sig)});
    k.cert.assign(cert.begin(), cert.end());
    return true;
}

// little-endian numbers and length-prefixed blocks, as the signing block has them
void U32(Bytes& b, uint64_t v) {
    for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
}
void U64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i)));
}
void U16(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
Bytes Prefixed(const Bytes& body) {
    Bytes out;
    U32(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// one entry of the new zip, for its central directory record
struct Entry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0;
    uint64_t offset = 0;  // of its local header
};

constexpr uint16_t kDosTime = 0, kDosDate = (1 << 9) | (1 << 5) | 1;  // 1981-01-01, as Android's build tools write
constexpr uint32_t kSignatureV2 = 0x7109871a, kEcdsaWithSha256 = 0x0201;
constexpr size_t kChunk = 1 << 20;

// a local header for e at the writer's position, padded so the data starts at a multiple of align: the padding is
// zipalign's extra field (0xD935: the alignment, then zeros)
Bytes LocalHeader(const Entry& e, uint64_t at, uint32_t align) {
    size_t fixed = 30 + e.name.size();
    size_t extra = 0;
    if (align > 1) {
        extra = (align - (at + fixed + 6) % align) % align + 6;
    }
    Bytes h;
    U32(h, 0x04034b50);
    U16(h, 20);
    U16(h, 0);
    U16(h, e.method);
    U16(h, kDosTime);
    U16(h, kDosDate);
    U32(h, e.crc);
    U32(h, e.csize);
    U32(h, e.usize);
    U16(h, uint32_t(e.name.size()));
    U16(h, uint32_t(extra));
    h.insert(h.end(), e.name.begin(), e.name.end());
    if (extra) {
        U16(h, 0xD935);
        U16(h, uint32_t(extra - 4));
        U16(h, align);
        h.resize(h.size() + extra - 6, 0);
    }
    return h;
}

Bytes CentralRecord(const Entry& e) {
    Bytes c;
    U32(c, 0x02014b50);
    U16(c, 20);
    U16(c, 20);
    U16(c, 0);
    U16(c, e.method);
    U16(c, kDosTime);
    U16(c, kDosDate);
    U32(c, e.crc);
    U32(c, e.csize);
    U32(c, e.usize);
    U16(c, uint32_t(e.name.size()));
    U16(c, 0);
    U16(c, 0);
    U16(c, 0);
    U16(c, 0);
    U32(c, 0);
    U32(c, e.offset);
    c.insert(c.end(), e.name.begin(), e.name.end());
    return c;
}

Bytes EndRecord(size_t entries, uint64_t cd_size, uint64_t cd_offset) {
    Bytes e;
    U32(e, 0x06054b50);
    U16(e, 0);
    U16(e, 0);
    U16(e, uint32_t(entries));
    U16(e, uint32_t(entries));
    U32(e, cd_size);
    U32(e, cd_offset);
    U16(e, 0);
    return e;
}

// chunk digests of n bytes read by read(at, buf, len), for v2's top-level digest
void DigestChunks(uint64_t n, const std::function<bool(uint64_t, uint8_t*, size_t)>& read, std::vector<Bytes>& out,
                  Bytes& buf) {
    for (uint64_t at = 0; at < n; at += kChunk) {
        size_t len = size_t(std::min<uint64_t>(kChunk, n - at));
        read(at, buf.data(), len);
        Sha256 s;
        uint8_t head[5] = {0xa5, uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16), uint8_t(len >> 24)};
        s.Update(head, 5);
        s.Update(buf.data(), len);
        Bytes d(32);
        s.Final(d.data());
        out.push_back(std::move(d));
    }
}

// the signature files of an older JAR signature, which the new signature replaces
bool OldSignature(const std::string& name) {
    if (name.rfind("META-INF/", 0) != 0) return false;
    std::string ext = name.substr(name.find_last_of('.') == std::string::npos ? name.size() : name.find_last_of('.'));
    return name == "META-INF/MANIFEST.MF" || ext == ".SF" || ext == ".RSA" || ext == ".DSA" || ext == ".EC";
}

}  // namespace

std::string Sha256File(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    Sha256 s;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        s.Update(buf.data(), size_t(in.gcount()));
    }
    uint8_t d[32];
    s.Final(d);
    return Hex(d, 32);
}

bool LoadOrMakeKey(const fs::path& path, Key& key, std::string& error) {
    uECC_set_rng(Random);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        try {
            toml::table t = toml::parse_file(path.string());
            Bytes priv, cert;
            if (!Unhex(t["private_key"].value_or(std::string()), priv) || priv.size() != 32 ||
                !Unhex(t["certificate"].value_or(std::string()), cert) || cert.empty()) {
                error = path.string() + " is not a signing key this program wrote";
                return false;
            }
            std::memcpy(key.priv, priv.data(), 32);
            if (!uECC_compute_public_key(key.priv, key.pub, uECC_secp256r1())) {
                error = path.string() + " holds no valid key";
                return false;
            }
            key.cert.assign(cert.begin(), cert.end());
            return true;
        } catch (const toml::parse_error& e) {
            error = path.string() + ": " + std::string(e.description());
            return false;
        }
    }
    if (!uECC_make_key(key.pub, key.priv, uECC_secp256r1()) || !MakeCertificate(key)) {
        error = "could not make a signing key";
        return false;
    }
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    f << "# The key this PC signs the Android app with (Live & Recompiled's APK builder). Android installs a new build\n"
         "# over the installed one only when the same key signed both: keep this file, and keep it to yourself.\n"
         "private_key = \"" << Hex(key.priv, 32) << "\"\n"
         "certificate = \"" << Hex(reinterpret_cast<const uint8_t*>(key.cert.data()), key.cert.size()) << "\"\n";
    if (!f.flush()) {
        error = "could not write the signing key to " + path.string();
        return false;
    }
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, ec);
#endif
    return true;
}

bool Build(const fs::path& base, const fs::path& so, const fs::path& out, const Key& key, std::string& error) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, base.string().c_str(), 0)) {
        error = "cannot read the base APK " + base.string();
        return false;
    }
    fs::path tmp = out;
    tmp += ".part";
    std::fstream f(tmp, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!f) {
        mz_zip_reader_end(&z);
        error = "cannot write " + tmp.string();
        return false;
    }
    uint64_t at = 0;
    auto write = [&](const void* p, size_t n) {
        f.write(static_cast<const char*>(p), std::streamsize(n));
        at += n;
    };
    std::vector<Entry> entries;
    bool ok = true;
    // the base's entries, compressed ones as they are
    for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&z); ok && i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) {
            ok = false;
            break;
        }
        std::string name = st.m_filename;
        if (st.m_is_directory || OldSignature(name) || name == "lib/arm64-v8a/libmain.so") continue;
        size_t n = 0;
        void* raw = mz_zip_reader_extract_to_heap(&z, i, &n, MZ_ZIP_FLAG_COMPRESSED_DATA);
        if (!raw && st.m_comp_size) {
            ok = false;
            break;
        }
        Entry e{name, uint16_t(st.m_method), st.m_crc32, st.m_comp_size, st.m_uncomp_size, at};
        Bytes h = LocalHeader(e, at, e.method == 0 ? 4 : 1);
        write(h.data(), h.size());
        write(raw, n);
        mz_free(raw);
        entries.push_back(e);
    }
    mz_zip_reader_end(&z);
    if (!ok) {
        error = "cannot read the base APK's entries";
        return false;
    }
    // the game, stored
    {
        std::ifstream in(so, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
        if (data.empty()) {
            error = "cannot read " + so.string();
            return false;
        }
        Entry e{"lib/arm64-v8a/libmain.so", 0, uint32_t(mz_crc32(MZ_CRC32_INIT, data.data(), data.size())), data.size(),
                data.size(), at};
        Bytes h = LocalHeader(e, at, 16384);
        write(h.data(), h.size());
        write(data.data(), data.size());
        entries.push_back(e);
    }
    const uint64_t entries_end = at;
    Bytes cd;
    for (const Entry& e : entries) {
        Bytes c = CentralRecord(e);
        cd.insert(cd.end(), c.begin(), c.end());
    }
    if (!f.flush()) {
        error = "cannot write " + tmp.string() + " (is the disk full?)";
        return false;
    }

    // v2's digest: the entries, the central directory, and the end record with the directory's offset where the
    // signing block starts
    std::vector<Bytes> chunks;
    Bytes buf(kChunk);
    DigestChunks(entries_end, [&](uint64_t pos, uint8_t* p, size_t n) {
        f.seekg(std::streamoff(pos));
        return bool(f.read(reinterpret_cast<char*>(p), std::streamsize(n)));
    }, chunks, buf);
    Bytes eocd = EndRecord(entries.size(), cd.size(), entries_end);
    for (const Bytes* part : {&cd, &eocd})
        DigestChunks(part->size(), [&](uint64_t pos, uint8_t* p, size_t n) {
            std::memcpy(p, part->data() + pos, n);
            return true;
        }, chunks, buf);
    Sha256 top;
    uint8_t head[5] = {0x5a, uint8_t(chunks.size()), uint8_t(chunks.size() >> 8), uint8_t(chunks.size() >> 16),
                       uint8_t(chunks.size() >> 24)};
    top.Update(head, 5);
    for (const Bytes& c : chunks) top.Update(c.data(), c.size());
    Bytes digest(32);
    top.Final(digest.data());

    // the signer: its signed data (the digest, the certificate, no attributes), the signature over it, its key
    Bytes alg;
    U32(alg, kEcdsaWithSha256);
    Bytes cert(key.cert.begin(), key.cert.end());
    Bytes signed_data = Cat({Prefixed(Prefixed(Cat({alg, Prefixed(digest)}))), Prefixed(Prefixed(cert)), Prefixed({})});
    Bytes signature;
    if (!Sign(key, signed_data, signature)) {
        error = "could not sign the APK";
        return false;
    }
    Bytes signer = Cat({Prefixed(signed_data), Prefixed(Prefixed(Cat({alg, Prefixed(signature)}))),
                        Prefixed(PublicKeyInfo(key))});
    Bytes value = Prefixed(Prefixed(signer));
    Bytes pair;
    U64(pair, value.size() + 4);
    U32(pair, kSignatureV2);
    pair.insert(pair.end(), value.begin(), value.end());
    Bytes block;
    uint64_t block_size = pair.size() + 8 + 16;  // the pairs, the size again and the magic
    U64(block, block_size);
    block.insert(block.end(), pair.begin(), pair.end());
    U64(block, block_size);
    const char magic[] = "APK Sig Block 42";
    block.insert(block.end(), magic, magic + 16);

    f.clear();
    f.seekp(std::streamoff(entries_end));
    f.write(reinterpret_cast<const char*>(block.data()), std::streamsize(block.size()));
    f.write(reinterpret_cast<const char*>(cd.data()), std::streamsize(cd.size()));
    Bytes end = EndRecord(entries.size(), cd.size(), entries_end + block.size());
    f.write(reinterpret_cast<const char*>(end.data()), std::streamsize(end.size()));
    if (!f.flush()) {
        error = "cannot write " + tmp.string() + " (is the disk full?)";
        return false;
    }
    f.close();
    std::error_code ec;
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) {
        error = "cannot replace " + out.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace apk
