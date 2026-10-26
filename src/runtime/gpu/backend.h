#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the last picture Present showed, shown again with the overlay over it: the title is held (kernel::Hold) and
    // draws nothing, the menu still has to. A backend without a window does nothing
    virtual void Redisplay() {}
    // frame generation: Present shows the first of a frame's pictures and holds the rest for later; Due says when the
    // next held one is to be shown (false when none is held), Show shows those due by now. Present itself shows any
    // still held from the frame before first
    virtual bool Due(std::chrono::steady_clock::time_point& /*at*/) { return false; }
    virtual void Show() {}
    // frame generation by drawing the frame again (render_thread.cpp): Pictures is how many the frame about to be
    // presented is shown as, 1 for itself alone. Keep takes its own picture from the front buffer before it is drawn
    // again, Between each one drawn again, i from 1 to Pictures - 1; Present then shows them in turn, its own last.
    // Replaying brackets the draws done again, which count no visibility test and feed no shading watch
    virtual uint32_t Pictures() { return 1; }
    virtual void Keep(const Surface& /*front*/) {}
    virtual void Between(const Surface& /*front*/, uint32_t /*i*/) {}
    virtual void Replaying(bool /*on*/) {}
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}
    // a line for the frame capture, in order with the draws (state the draws alone don't show, e.g. who set it)
    virtual void Note(const char* /*text*/) {}
    // before a Draw: whether anything changed since the Draw before it besides vertex sources (streams, indices, the
    // raw vertex array and constant upload methods) and vertex program constants. Never called: every draw is new
    virtual void Changed(bool /*any*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}

private:
    uint32_t fence_ = 0;
};

// the renderer the settings name, initialised; one that failed Init draws nothing. A backend is a Backend subclass in
// its own file with a Create<Name>Backend here, and a line in CreateBackend (backend.cpp)
std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height);
std::unique_ptr<Backend> CreateVulkanBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause
// XBE_PERF: the title thread's draw recording in TSC cycles: state compared, headers copied, command, uses, handover
inline std::atomic<uint64_t> g_perf_rec_cycles[5];

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};
// F4: save the pictures XBE_INTERP_RECORD keeps (gpu/interp.h) under diag::CaptureDir
inline std::atomic<bool> g_record_dump{false};

}  // namespace gpu
