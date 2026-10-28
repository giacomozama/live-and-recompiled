// The Android app's own calls into libmain.so (org.liveandrecompiled.app.Native, android/): before the game runs, the
// app's first screen puts the player's copy of the game in its assets folder. The player picks it with Android's
// document picker, which hands out file descriptors: a disc image (.iso), or the digital release's Data files, are
// unpacked from those here; extracted files the app copies itself and has checked here.
//
// Progress goes to the app through its Progress.report(done, total, text), on the calling thread; each call returns
// the empty string, or why it failed.
#include <jni.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "kernel/crypto.h"
#include "kernel/disc.h"
#include "setup/store.h"

extern const char kImageSha1[];             // the xbe recompiled into this library, written by CMake
extern const char* const kImageAlsoSha1[];  // releases with its code and a different header, null-ended

namespace {

namespace fs = std::filesystem;

constexpr const char* kXbe = "default.xbe";

// the app's Progress object, for the calls below while they run
struct Reporter {
    JNIEnv* env = nullptr;
    jobject progress = nullptr;
    jmethodID report = nullptr;

    void operator()(long long done, long long total, const std::string& text) const {
        if (!progress) return;
        jstring s = env->NewStringUTF(text.c_str());
        env->CallVoidMethod(progress, report, jlong(done), jlong(total), s);
        env->DeleteLocalRef(s);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
};
thread_local Reporter t_report;

Reporter MakeReporter(JNIEnv* env, jobject progress) {
    Reporter r{env, progress, nullptr};
    jclass c = env->GetObjectClass(progress);
    r.report = env->GetMethodID(c, "report", "(JJLjava/lang/String;)V");
    env->DeleteLocalRef(c);
    if (!r.report) {
        env->ExceptionClear();
        r.progress = nullptr;
    }
    return r;
}

// store::Unpack's messages: its "\r 42%" lines are the progress, the rest its text
void Say(const char* fmt, ...) {
    char text[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(text, sizeof(text), fmt, a);
    va_end(a);
    int pct = 0;
    if (text[0] == '\r' && std::sscanf(text + 1, "%d", &pct) == 1) t_report(pct, 100, "");
    else if (text[0] && text[0] != '\n') t_report(-1, -1, text);
}

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool Supported(const std::string& sha) {
    if (sha == kImageSha1) return true;
    for (const char* const* also = kImageAlsoSha1; *also; also++)
        if (sha == *also) return true;
    return false;
}

std::string Path(JNIEnv* env, jstring s) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

jstring Result(JNIEnv* env, const std::string& error) { return env->NewStringUTF(error.c_str()); }

// a disc's folder into dest, every file; written counts the bytes so far of total
bool Extract(const kernel::Disc& disc, const kernel::Disc::Entry& dir, const fs::path& dest, uint64_t& written,
             uint64_t total, std::vector<uint8_t>& buf, std::string& error) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    for (const auto& e : dir.children) {
        fs::path to = dest / e.name;
        if (e.name.empty() || e.name == "." || e.name == ".." || e.name.find('/') != std::string::npos) continue;
        if (e.dir) {
            if (!Extract(disc, e, to, written, total, buf, error)) return false;
            continue;
        }
        std::ofstream o(to, std::ios::binary | std::ios::trunc);
        if (!o) {
            error = "cannot write " + to.string();
            return false;
        }
        for (uint64_t pos = 0; pos < e.size;) {
            uint32_t n = disc.Read(e, pos, buf.data(), uint32_t(buf.size()));
            if (!n) {
                error = "the disc image ends inside " + e.name;
                return false;
            }
            if (!o.write(reinterpret_cast<const char*>(buf.data()), n)) {
                error = "cannot write " + to.string() + " (is the device full?)";
                return false;
            }
            pos += n;
            written += n;
            t_report((long long)written, (long long)total, "");
        }
    }
    return true;
}

uint64_t TotalSize(const kernel::Disc::Entry& dir) {
    uint64_t n = 0;
    for (const auto& e : dir.children) n += e.dir ? TotalSize(e) : e.size;
    return n;
}

}  // namespace

extern "C" {

// the release this build accepts, for the app to show
JNIEXPORT jstring JNICALL Java_org_liveandrecompiled_app_Native_expectedSha1(JNIEnv* env, jclass) {
    return env->NewStringUTF(kImageSha1);
}

// "" when dir holds the game's files with the default.xbe this build was made from
JNIEXPORT jstring JNICALL Java_org_liveandrecompiled_app_Native_verify(JNIEnv* env, jclass, jstring dir) {
    fs::path xbe = fs::path(Path(env, dir)) / kXbe;
    std::ifstream f(xbe, std::ios::binary);
    if (!f) return Result(env, "There is no default.xbe in the game's files.");
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    std::string sha = Sha1(data);
    if (!Supported(sha))
        return Result(env, "This is a different version of the game than this app was built for (default.xbe SHA-1 " +
                               sha + ", expected " + kImageSha1 + ").");
    return Result(env, "");
}

// a disc image, open as fd, into the folder out
JNIEXPORT jstring JNICALL Java_org_liveandrecompiled_app_Native_unpackIso(JNIEnv* env, jclass, jint fd, jstring out,
                                                                          jobject progress) {
    t_report = MakeReporter(env, progress);
    kernel::Disc disc;
    std::string error;
    if (!disc.OpenFd(fd, error)) return Result(env, "This file is not an Xbox disc image (" + error + ").");
    std::vector<uint8_t> xbe;
    if (!disc.Load(kXbe, xbe)) return Result(env, "This disc image has no default.xbe: it is not the game.");
    std::string sha = Sha1(xbe);
    if (!Supported(sha))
        return Result(env, "This disc image is a different version of the game than this app was built for (SHA-1 " +
                               sha + ").");
    const kernel::Disc::Entry* root = disc.Find(std::vector<std::string>{});
    uint64_t written = 0, total = TotalSize(*root);
    std::vector<uint8_t> buf(4u << 20);
    t_report(-1, -1, "Unpacking the disc image");
    bool ok = Extract(disc, *root, fs::path(Path(env, out)), written, total, buf, error);
    t_report = {};
    return Result(env, ok ? "" : error);
}

// the digital release's Data0000, Data0001, ..., open as fds in order, into the folder out
JNIEXPORT jstring JNICALL Java_org_liveandrecompiled_app_Native_unpackPackage(JNIEnv* env, jclass, jintArray fds,
                                                                              jstring out, jobject progress) {
    t_report = MakeReporter(env, progress);
    std::vector<int> data(size_t(env->GetArrayLength(fds)));
    env->GetIntArrayRegion(fds, 0, jsize(data.size()), reinterpret_cast<jint*>(data.data()));
    std::string error;
    bool ok = store::UnpackFds(data, fs::path(Path(env, out)), Say, error);
    t_report = {};
    return Result(env, ok ? "" : error);
}

}  // extern "C"
