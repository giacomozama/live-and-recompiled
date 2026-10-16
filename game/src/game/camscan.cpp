// XBE_CAMSCAN=1: finds the camera's turn angles, for a mouse look that turns the camera directly.
//
// The right stick's path into the camera is not mapped (the stick lands in the controller records at 0x806AD8 and
// is copied into each player's pad by 0x4E0D0; past that every reader goes through player objects). So this watches
// the result instead: a background thread passes over every committed page of guest memory, and each float that
// changed since the last pass gets a vote: +1 when it moved the way the right stick points (x for yaw, y for
// pitch), -1 the other way, and a mark against it when it changed with the stick at rest. An angle the stick turns
// collects votes on every pass the stick is held; a position or a sine flips sign with the quadrant and cancels out.
//
// Use: stand still in a level, F6, then hold the right stick (or the mouse with mouse look on) left, right, up and
// down for a few seconds each, resting between. F7 logs the best candidates, each with the image globals that point
// a little below it (an object holding it), as "camscan:" lines. F6 again starts over (and takes in pages committed
// since).
#include "common/camscan.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"

namespace camscan {

namespace {

// a stick this far over counts as held (of 32767)
constexpr int kHeld = 12000;
// the image's .data through .data1, where an object's owner would be found
constexpr uint32_t kDataLow = 0x64F980, kDataHigh = 0x883280;

std::atomic<int> g_rx{0}, g_ry{0};
std::atomic<bool> g_reset{true}, g_dump{false};

struct Slot {
    uint32_t bits;
    int16_t x, y;      // votes with the stick's x and y
    uint16_t still;    // changes with the stick at rest
};
std::vector<uint32_t> g_pages;  // guest page addresses
std::vector<Slot> g_slots;      // 1024 per page
uint32_t g_passes_x = 0, g_passes_y = 0, g_passes_still = 0;

uint32_t Load(uint32_t va) { return __atomic_load_n(reinterpret_cast<uint32_t*>(mem::Base() + va), __ATOMIC_RELAXED); }

bool Usable(float f) { return std::isfinite(f) && std::fabs(f) < 1e5f; }

void AddRange(uint32_t lo, uint32_t hi) {
    for (uint64_t p = lo; p < hi; p += mem::kPage)
        if (mem::IsCommitted(uint32_t(p))) g_pages.push_back(uint32_t(p));
}

void Reset() {
    g_pages.clear();
    AddRange(mem::kUserLow, mem::kUserHigh);
    AddRange(mem::kContiguousBase + mem::kKernelDataSize, mem::kContiguousBase + mem::kPhysicalSize);
    AddRange(mem::kSystemBase, mem::kSystemBase + mem::kSystemSize);
    g_slots.assign(g_pages.size() * 1024, {});
    for (size_t i = 0; i < g_pages.size(); i++)
        for (uint32_t k = 0; k < 1024; k++) g_slots[i * 1024 + k].bits = Load(g_pages[i] + k * 4);
    g_passes_x = g_passes_y = g_passes_still = 0;
    LOGR("camscan: watching %zu pages (%zu MB); hold the right stick each way, then F7", g_pages.size(),
         g_pages.size() * mem::kPage >> 20);
}

void Pass() {
    const int rx = g_rx.load(), ry = g_ry.load();
    const int sx = rx >= kHeld ? 1 : rx <= -kHeld ? -1 : 0;
    const int sy = ry >= kHeld ? 1 : ry <= -kHeld ? -1 : 0;
    const bool rest = std::abs(rx) < 2000 && std::abs(ry) < 2000;
    if (sx) g_passes_x++;
    if (sy) g_passes_y++;
    if (rest) g_passes_still++;
    for (size_t i = 0; i < g_pages.size(); i++) {
        if (!mem::IsCommitted(g_pages[i])) continue;  // let go of since the reset
        Slot* s = &g_slots[i * 1024];
        for (uint32_t k = 0; k < 1024; k++, s++) {
            const uint32_t bits = Load(g_pages[i] + k * 4);
            if (bits == s->bits) continue;
            float now, was;
            std::memcpy(&now, &bits, 4);
            std::memcpy(&was, &s->bits, 4);
            s->bits = bits;
            if (!Usable(now) || !Usable(was)) continue;
            const int d = now > was ? 1 : -1;
            if (rest) s->still = uint16_t(std::min(s->still + 1, 65535));
            if (sx) s->x = int16_t(std::clamp(s->x + d * sx, -32767, 32767));
            if (sy) s->y = int16_t(std::clamp(s->y + d * sy, -32767, 32767));
        }
    }
}

void Owners(uint32_t va) {
    int n = 0;
    for (uint32_t g = kDataLow; g < kDataHigh && n < 4; g += 4) {
        const uint32_t p = Load(g);
        if (p <= va && va - p < 0x4000) {
            LOGR("camscan:     [0x%06X] = %08X, +0x%X", g, p, va - p);
            n++;
        }
    }
}

void Dump() {
    LOGR("camscan: passes with the stick x held %u, y held %u, at rest %u", g_passes_x, g_passes_y, g_passes_still);
    for (int axis = 0; axis < 2; axis++) {
        // most votes, least moved at rest
        std::vector<std::pair<int, size_t>> best;
        for (size_t i = 0; i < g_slots.size(); i++) {
            const Slot& s = g_slots[i];
            const int v = std::abs(axis ? s.y : s.x) - 2 * s.still;
            if (v > 4) best.push_back({v, i});
        }
        const size_t n = std::min<size_t>(best.size(), 16);
        std::partial_sort(best.begin(), best.begin() + n, best.end(), std::greater<>());
        LOGR("camscan: %s candidates (%zu scored)", axis ? "pitch (stick y)" : "yaw (stick x)", best.size());
        for (size_t j = 0; j < n; j++) {
            const Slot& s = g_slots[best[j].second];
            const uint32_t va = g_pages[best[j].second / 1024] + uint32_t(best[j].second % 1024) * 4;
            float f;
            std::memcpy(&f, &s.bits, 4);
            LOGR("camscan:   %08X = %10.4f  x %+d  y %+d  at rest %u", va, f, s.x, s.y, s.still);
            if (j < 8) Owners(va);
        }
    }
}

void Run() {
    for (;;) {
        if (g_reset.exchange(false)) Reset();
        if (g_dump.exchange(false)) Dump();
        Pass();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool Pressed(int vk, bool& was) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !was;
    was = down;
    return edge;
}

}  // namespace

void Tick(const input::Pad& pad) {
    static const bool enabled = [] {
        const char* e = std::getenv("XBE_CAMSCAN");
        return e && *e && *e != '0';
    }();
    if (!enabled) return;
    static std::once_flag started;
    std::call_once(started, [] {
        LOGR("camscan: on; F6 starts over, F7 logs the candidates");
        std::thread(Run).detach();
    });
    g_rx = pad.rx;
    g_ry = pad.ry;
    static bool f6 = false, f7 = false;
    if (Pressed(VK_F6, f6)) g_reset = true;
    if (Pressed(VK_F7, f7)) g_dump = true;
}

}  // namespace camscan

#else  // no scan on the headless harness

namespace camscan {

void Tick(const input::Pad&) {}

}  // namespace camscan

#endif
