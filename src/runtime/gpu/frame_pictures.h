#pragma once
// Frame generation as the D3D11 and OpenGL renderers do it (gpu/d3d11.cpp, gpu/opengl.cpp): the frame's pictures kept
// as they are drawn, shown on a schedule spread over the frame, and counted for the frame rate. Vulkan has its own.
//
// Self, the backend deriving from this (a friend of it) and from Targets, has hud_, FinishPicture(image, slot),
// CopyPicture(source, destination) and Display: DisplayKept(slot, count), a kept picture, and
// DisplayAlone(image, count), a frame shown by itself (null: the last one presented)
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <utility>

#include "app/app.h"
#include "gpu/draw.h"
#include "gpu/hud_split.h"
#include "gpu/targets.h"

namespace gpu {

template <class Self, class Image>
class FramePictures {
protected:
    std::array<Image, 4> generated_;
    uint32_t taken_ = 0, generated_count_ = 0, next_picture_ = 0;
    int last_picture_ = -1;
    bool replaying_ = false, keeping_pictures_ = false;
    std::chrono::steady_clock::time_point schedule_{};
    std::chrono::steady_clock::duration picture_interval_{};
    uint64_t shown_ = 0;  // the target last presented
    std::chrono::steady_clock::time_point last_{};
    std::chrono::steady_clock::time_point fps_since_{};
    uint32_t fps_pictures_ = 0;
    float fps_ = 0;

    // Backend::Pictures, where there is a window to show them in
    static uint32_t PicturesWanted() {
        const auto& set = app::Get();
        const uint32_t wanted = std::clamp(set.frame_generation, 1u, 4u);
        if (!set.vsync || wanted == 1) return wanted;
        // A 60 Hz display with VSync can present one picture per 60 Hz game frame.
        return std::min(wanted, std::max(1u, (app::MonitorRefresh() + 1) / 60));
    }
    // Backend::Keep: what is left of the frame before shown, and this one's own picture kept as its last
    void KeepFirst(const Surface& front) {
        if (next_picture_ < generated_count_ && last_picture_ >= 0) Backend().DisplayKept(last_picture_, true);
        generated_count_ = std::clamp(app::Get().frame_generation, 2u, 4u);
        if (app::Get().vsync) generated_count_ = std::clamp((app::MonitorRefresh() + 1) / 60, 2u, generated_count_);
        next_picture_ = generated_count_;
        taken_ = 0;
        keeping_pictures_ = true;
        KeepPicture(front, generated_count_ - 1);
    }
    // Backend::Between: a picture drawn again, before the frame's own
    void KeepBetween(const Surface& front, uint32_t pass) {
        if (pass < generated_count_) KeepPicture(front, pass - 1);
    }
    void SetReplaying(bool on) {
        Self& self = Backend();
        if (on && !self.hud_.rolled) {  // the frame's draws again take its own boxes
            self.hud_.Roll(self.guest_frame_.w);
            self.hud_.rolled = true;
        }
        replaying_ = on;
    }
    bool PictureDue(std::chrono::steady_clock::time_point& at) const {
        if (next_picture_ >= generated_count_) return false;
        at = schedule_;
        return true;
    }
    // the kept pictures due by now: the latest of them shown
    void ShowDue() {
        const auto now = std::chrono::steady_clock::now();
        while (next_picture_ < generated_count_ && schedule_ <= now) {
            const uint32_t slot = next_picture_++;
            schedule_ += picture_interval_;
            if ((taken_ & (1u << slot)) && (schedule_ > now || next_picture_ == generated_count_)) {
                Backend().DisplayKept(int(slot), true);
                break;
            }
        }
    }
    // Backend::Present's start: the front buffer's target (null: none), noted as the one shown
    Image* FrontPicture(const Surface& front) {
        Self& self = Backend();
        if (!std::exchange(self.hud_.rolled, false)) self.hud_.Roll(self.guest_frame_.w);
        Image* image = self.Target(front, false);
        if (image) {
            self.CpuWrite(*image);
            shown_ = ResourceKey(front.data, false);
        }
        // what is left of the frame before, if this one kept no pictures
        if (!keeping_pictures_ && next_picture_ < generated_count_ && last_picture_ >= 0)
            self.DisplayKept(last_picture_, true);
        last_picture_ =
            keeping_pictures_ && (taken_ & (1u << (generated_count_ - 1))) ? int(generated_count_ - 1) : -1;
        keeping_pictures_ = false;
        return image;
    }
    // the time between this frame's pictures, from the time the last frame took
    void PacePictures() {
        using Duration = std::chrono::steady_clock::duration;
        const auto now = std::chrono::steady_clock::now();
        const auto pictures = last_picture_ < 0 ? 1 : std::popcount(taken_);
        const auto elapsed = last_ == decltype(last_){} ? Duration(std::chrono::milliseconds(16)) : now - last_;
        picture_interval_ =
            std::clamp(elapsed, Duration(std::chrono::milliseconds(4)), Duration(std::chrono::milliseconds(50))) /
            pictures;
        last_ = now;
    }
    // the frame by itself, or the first of its pictures now and the others on the schedule
    void ShowFrame(Image* image) {
        if (last_picture_ < 0) {
            next_picture_ = generated_count_;
            Backend().DisplayAlone(image, true);
        } else {
            next_picture_ = 0;
            schedule_ = last_;  // PacePictures' now
            ShowDue();
        }
    }
    // Backend::Redisplay: what is on screen again, with this moment's overlay
    void ShowAgain() {
        next_picture_ = generated_count_;
        if (last_picture_ < 0) Backend().DisplayAlone(nullptr, false);
        else Backend().DisplayKept(last_picture_, false);
    }
    // a picture shown, in the frame rate
    void CountPicture() {
        const auto now = std::chrono::steady_clock::now();
        if (fps_since_ == decltype(fps_since_){} || now - fps_since_ > std::chrono::seconds(2)) {
            fps_since_ = now;
            fps_pictures_ = 0;
        } else if (++fps_pictures_; now - fps_since_ >= std::chrono::milliseconds(500)) {
            fps_ = fps_pictures_ / std::chrono::duration<float>(now - fps_since_).count();
            fps_since_ = now;
            fps_pictures_ = 0;
        }
    }

private:
    void KeepPicture(const Surface& front, uint32_t slot) {
        Self& self = Backend();
        Image* picture = self.Target(front, false);
        if (!picture) return;
        self.CpuWrite(*picture);
        self.FinishPicture(*picture, slot);
        auto& copy = generated_[slot];
        if (!self.ScratchColor(copy, picture->w, picture->h)) return;
        self.CopyPicture(*picture, copy);
        copy.fur_frame = picture->fur_frame;
        taken_ |= 1u << slot;
    }
    Self& Backend() { return static_cast<Self&>(*this); }
};

}  // namespace gpu
