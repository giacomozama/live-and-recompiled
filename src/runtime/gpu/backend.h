#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu/state.h"

namespace gpu {

// a draw's vertex constants and fixed function transforms in the picture before, where they differ from its own
struct Before {
    std::vector<std::pair<uint32_t, std::array<float, 4>>> vsc;
    std::vector<std::pair<uint32_t, std::array<float, 16>>> xf;
};

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
    // a backend made before the title had a device (g_early): its back buffer size once it has, before anything is drawn
    virtual void SetGuestSize(uint32_t /*width*/, uint32_t /*height*/) {}
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the last picture Present showed, shown again with the overlay over it: the title is held (kernel::Hold) and
    // draws nothing, the menu still has to; blurred the longer it is held, unless blur is false (a load's fade). A
    // backend without a window does nothing
    virtual void Redisplay(bool /*blur*/ = true) {}
    // the freeze (g_freeze): the last frame drawn again from its kept commands, with the settings as they are now,
    // and presented; false when no frame was kept whole (nothing is shown then)
    virtual bool Refresh() { return false; }
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
    // drawing again: Skips, the next Draw is one a run drawn again took with it, so blending it is wasted; Pass, a draw
    // left out of the picture, counted all the same so the draws after it keep the places the frame gave them
    virtual bool Skips() const { return false; }
    virtual void Pass() {}
    // drawing again, before a Draw: true, the draw is the frame's own but for the constants blended for the picture;
    // false, its vertices or other state were changed for it too
    virtual void AsDrawn(bool /*same*/) {}
    // motion vectors, for a temporal upscaler: Motion says the backend wants them for each of the frame's pictures.
    // Between MotionPass(true, slot) and MotionPass(false, slot) the frame's draws come again through DrawMotion, at
    // the picture's own t, each with what its constants were in the picture shown before it (Before). slot numbers
    // the frame's pictures as Keep and Between do: Pictures() - 1 its own, i - 1 Between's i
    virtual bool Motion() { return false; }
    virtual void MotionPass(bool /*on*/, uint32_t /*slot*/) {}
    virtual void DrawMotion(const State& /*s*/, const Before& /*before*/, uint32_t /*prim*/, uint32_t /*start*/,
                            uint32_t /*count*/, const uint8_t* /*indices*/) {}
    // asked before each draw of a motion pass: true, the backend leaves it out (it counted it), so its constants for
    // the picture before and this one needn't be worked out
    virtual bool MotionSkips() { return false; }
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
// the GPUs the backend can draw with, named, as it found them at start (empty before); the settings' gpu names one
std::vector<std::string>& Adapters();
// the swap chain's surface offers an HDR10 format (set as the swap chain is made, false before)
bool HdrAvailable();
void SetHdrAvailable(bool);
std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height);
std::unique_ptr<Backend> CreateBackend(uint32_t width, uint32_t height, bool& ok);  // ok: Init succeeded
// a backend made before boot (app::RunWelcome draws the welcome screen with it), which the title's device takes over
// instead of making its own (d3d8.cpp)
inline std::unique_ptr<Backend> g_early;
std::unique_ptr<Backend> CreateVulkanBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
// how much of the picture reaches the window, 0 black to 1 whole: a load fades through black (d3d8.cpp
// LoadingScreenWanted); read by the backend as it shows each picture, the overlay drawn over it unfaded
inline std::atomic<float> g_fade{1.0f};
// the welcome screen's backdrop (app::RunWelcome), 0 black to 1 whole: the game fades it in and out with the screen
inline std::atomic<float> g_welcome_fade{0.0f};
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause
// XBE_PERF: the title thread's draw recording in TSC cycles: state compared, headers copied, command, uses, handover
inline std::atomic<uint64_t> g_perf_rec_cycles[5];

// DLSS as the renderer found it, for the menu: 0 not there (no NVIDIA RTX GPU or driver for it), 1 there, 2 there
// but failed to start, 3 an NVIDIA GPU but no DLSS module beside the game (the log says why)
inline std::atomic<int> g_dlss_state{0};

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};
// F4: save the pictures XBE_INTERP_RECORD keeps (gpu/interp.h) under diag::CaptureDir
inline std::atomic<bool> g_record_dump{false};
// F7: the game frozen on its last frame, drawn again each refresh with the settings as they are now (d3d8.cpp's Swap
// waits, Backend::Refresh draws); F8 lets one more frame through, steps, freezing first if it isn't
inline std::atomic<bool> g_freeze{false};
// Debug: a colour vision deficiency simulated over the picture put on screen (the settings menu left out): 0 none,
// 1 protanopia, 2 deuteranopia, 3 tritanopia, 4 achromatopsia (cvd_hlsl.h). Not saved
inline std::atomic<uint32_t> g_cvd_sim{0};
// the settings menu's display calibration: a test pattern in place of the picture, the overlay over it (vulkan.cpp
// kCalibHlsl): 0 none, 1 black level, 2 gamma, 3 paper white, 4 HDR peak. Not saved
inline std::atomic<uint32_t> g_calibrate{0};
inline std::atomic<uint32_t> g_freeze_steps{0};
inline void ToggleFreeze() {
    g_freeze_steps = 0;
    g_freeze = !g_freeze.load();
}
inline void StepFrozen() {
    if (g_freeze.exchange(true)) g_freeze_steps++;
}

}  // namespace gpu
