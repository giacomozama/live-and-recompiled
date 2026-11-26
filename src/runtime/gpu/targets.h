#pragma once
// The render targets the D3D11 and OpenGL renderers keep for the title's surfaces (gpu/d3d11.cpp, gpu/opengl.cpp):
// made at the render resolution, filled from what the CPU wrote, resolved from their multisampled copies, resized and
// copied. Vulkan has its own.
//
// Image has w, h, data, samples, guest, depth, drawn, epoch, signature, frame, surface, ms and newest. Self, the
// backend deriving from this (a friend of it), has:
//   kName, for the log
//   CreateTargetImage(image), HasTexture(image), RenderSize(size), PictureViewport() and ViewportSize(viewport)
//   Fill(image, pixels, pitch), BlitWhole(source, destination, linear) and ResolveColor(source, destination)
//   PreserveDepth(source, destination), SnapshotResource(source)
//   CopyPixels(source, from, destination, to), the rectangles in guest pixels
//   BeforeResize(), before targets are replaced
#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "kernel/memory.h"

namespace gpu {

// XDK surface headers store physical addresses; texture descriptions and locks use virtual addresses. Every resource
// lookup must use the same canonical form.
inline uint64_t ResourceKey(uint32_t address, bool depth) {
    return TargetMapKey(mem::VirtualFromPhysical(address), depth);
}

template <class Self, class Image>
class Targets {
protected:
    std::unordered_map<uint64_t, Image> targets_, feedback_, depth_bytes_;
    Image native_color_;  // GPU transfer at the guest's size for CPU surface locks
    uint64_t frame_ = 0;
    GuestExtent guest_frame_{};
    app::Scale render_scale_{1, 1};
    GuestExtent requested_frame_{};
    uint32_t samples_ = 1, requested_samples_ = 0, supported_samples_ = 1;

    bool CreateMultisample(Image& image, uint32_t samples) {
        if (samples == 1 || image.guest.w != guest_frame_.w || image.guest.h != guest_frame_.h) return true;
        auto ms = std::make_unique<Image>();
        ms->w = image.w;
        ms->h = image.h;
        ms->guest = image.guest;
        ms->depth = image.depth;
        ms->data = image.data;
        ms->samples = samples;
        if (!Backend().CreateTargetImage(*ms)) return false;
        image.ms = std::move(ms);
        image.newest = Image::Newest::Single;
        return true;
    }
    // the surface's target, made at the render resolution on its first use
    Image* Target(const Surface& surface, bool depth) {
        if (!surface.header || !surface.data) return nullptr;
        GuestExtent guest{};
        SurfaceSize(surface, guest.w, guest.h);
        if (!guest.w || !guest.h || guest.w > 8192 || guest.h > 8192) return nullptr;
        const auto key = ResourceKey(surface.data, depth);
        auto it = targets_.find(key);
        if (it != targets_.end() && it->second.guest.w == guest.w && it->second.guest.h == guest.h) {
            it->second.frame = frame_;
            return &it->second;
        }
        Self& self = Backend();
        Image im;
        im.guest = guest;
        im.w = self.RenderSize(guest.w * render_scale_.x);
        im.h = self.RenderSize(guest.h * render_scale_.y);
        im.data = mem::VirtualFromPhysical(surface.data);
        im.depth = depth;
        im.surface = surface;
        im.frame = frame_;
        if (!self.CreateTargetImage(im)) return nullptr;
        if (!im.depth) UploadTarget(im);
        CreateMultisample(im, samples_);
        im.epoch = DirtyEpoch(im.data);
        return &targets_.insert_or_assign(key, std::move(im)).first->second;
    }
    Image* Snapshot(Image& source) { return Resolve(source) ? Backend().SnapshotResource(source) : nullptr; }
    // the single sampled image brought up to date from the multisampled one
    bool Resolve(Image& image) {
        if (!image.ms || image.newest != Image::Newest::Multisampled) return true;
        if (image.depth) {
            if (!Backend().PreserveDepth(*image.ms, image)) return false;
        } else {
            Backend().ResolveColor(*image.ms, image);
        }
        image.newest = Image::Newest::Both;
        return true;
    }
    // what a draw writes: the multisampled copy, brought up to date, or the image itself
    Image* Attachment(Image* image, bool multisampled) {
        if (!image) return nullptr;
        if (!multisampled || !image->ms) return Resolve(*image) ? image : nullptr;
        if (image->newest == Image::Newest::Single) {
            const bool copied = image->depth ? Backend().PreserveDepth(*image, *image->ms)
                                             : Backend().BlitWhole(*image, *image->ms, false);
            if (!copied) return nullptr;
            image->newest = Image::Newest::Both;
        }
        return image->ms.get();
    }
    void MarkWritten(Image& image, bool multisampled = false) {
        image.drawn = true;
        image.frame = frame_;
        if (image.ms) image.newest = multisampled ? Image::Newest::Multisampled : Image::Newest::Single;
    }
    // a color image of this size for a pass's output, made anew only when the size changes
    bool ScratchColor(Image& image, uint32_t width, uint32_t height) {
        if (Self::HasTexture(image) && image.w == width && image.h == height) return true;
        Image next;
        next.w = width;
        next.h = height;
        if (!Backend().CreateTargetImage(next)) return false;
        image = std::move(next);
        return true;
    }
    // the render resolution and MSAA the settings ask for, the targets remade when they change
    void ApplyRenderScale() {
        Self& self = Backend();
        const auto [picture_w, picture_h] = Self::ViewportSize(self.PictureViewport());
        const auto& set = app::Get();
        const float percent = set.render_percent / 100.0f;
        const GuestExtent requested{self.RenderSize(picture_w * percent), self.RenderSize(picture_h * percent)};
        const uint32_t wanted = set.smaa ? 1 : set.msaa >= 8 ? 8 : set.msaa >= 4 ? 4 : set.msaa >= 2 ? 2 : 1;
        if (requested.w == requested_frame_.w && requested.h == requested_frame_.h && wanted == requested_samples_)
            return;
        requested_frame_ = requested;
        requested_samples_ = wanted;
        uint32_t samples = wanted;
        while (!(supported_samples_ & samples)) samples >>= 1;
        if (wanted != samples) LOGR("%s: MSAA %ux unsupported, using %ux", Self::kName, wanted, samples);
        const app::Scale next{float(requested.w) / guest_frame_.w, float(requested.h) / guest_frame_.h};
        const auto log = [&] {
            LOGR("%s: render resolution %ux%u at %u%%, %ux MSAA", Self::kName, requested.w, requested.h,
                 set.render_percent, samples);
        };
        if (next.x == render_scale_.x && next.y == render_scale_.y && samples == samples_) {
            log();
            return;
        }

        // The title can draw a texture once and sample it much later. Keep that
        // picture, the last presented frame, and the depth/stencil at the new size.
        std::unordered_map<uint64_t, Image> resized;
        for (auto& [key, old] : targets_) {
            const uint32_t w = self.RenderSize(old.guest.w * next.x);
            const uint32_t h = self.RenderSize(old.guest.h * next.y);
            if (w == old.w && h == old.h && samples == samples_) continue;
            if (!Resolve(old)) return;
            Image im;
            im.w = w;
            im.h = h;
            im.data = old.data;
            im.guest = old.guest;
            im.epoch = old.epoch;
            im.signature = old.signature;
            im.frame = old.frame;
            im.depth = old.depth;
            im.drawn = old.drawn;
            im.surface = old.surface;
            if (!self.CreateTargetImage(im)) return;
            const bool copied =
                im.depth ? (!im.drawn || self.PreserveDepth(old, im)) : self.BlitWhole(old, im, true);
            if (!copied || !CreateMultisample(im, samples)) return;
            resized.emplace(key, std::move(im));
        }
        self.BeforeResize();
        for (auto& [key, image] : resized) targets_.at(key) = std::move(image);
        feedback_.clear();
        depth_bytes_.clear();
        render_scale_ = next;
        samples_ = samples;
        log();
    }
    bool NativeColor(GuestExtent size) {
        if (Self::HasTexture(native_color_) && native_color_.w == size.w && native_color_.h == size.h) return true;
        Image im;
        im.w = size.w;
        im.h = size.h;
        if (!Backend().CreateTargetImage(im)) return false;
        native_color_ = std::move(im);
        return true;
    }
    // the surface's pixels as guest memory has them, into its target
    bool UploadTarget(Image& im) {
        TextureDesc d;
        if (im.depth || !DescribeSurface(im.surface, d) || Format(d.format).host != Host::BGRA8) return false;
        std::vector<uint8_t> px;
        uint32_t pitch = 0;
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
        if (!pitch || px.empty() || px.size() < size_t(pitch) * d.height) return false;
        Self& self = Backend();
        if (im.w == d.width && im.h == d.height) {
            self.Fill(im, px, pitch);
        } else {
            if (!NativeColor({d.width, d.height})) return false;
            self.Fill(native_color_, px, pitch);
            if (!self.BlitWhole(native_color_, im, false)) return false;
        }
        im.signature = MemSig(d.data, TotalSize(d));
        if (im.ms) im.newest = Image::Newest::Single;
        return true;
    }
    // a color target the CPU has written since, uploaded again
    void CpuWrite(Image& im) {
        const uint64_t e = DirtyEpoch(im.data);
        if (!im.depth && e > im.epoch) {
            TextureDesc d;
            if (!DescribeSurface(im.surface, d) || MemSig(d.data, TotalSize(d)) == im.signature || UploadTarget(im))
                im.epoch = e;
        }
    }
    // clipped in guest space, so the source and destination move together
    bool CopyRect(const Image& source, Image& destination, GuestRect rect, GuestPoint point) {
        const int64_t dx = int64_t(point.x) - rect.left;
        const int64_t dy = int64_t(point.y) - rect.top;
        const int64_t left = std::max({int64_t(rect.left), int64_t(0), -dx});
        const int64_t top = std::max({int64_t(rect.top), int64_t(0), -dy});
        const int64_t right =
            std::min({int64_t(rect.right), int64_t(source.guest.w), int64_t(destination.guest.w) - dx});
        const int64_t bottom =
            std::min({int64_t(rect.bottom), int64_t(source.guest.h), int64_t(destination.guest.h) - dy});
        if (left >= right || top >= bottom) return false;
        return Backend().CopyPixels(
            source, {int32_t(left), int32_t(top), int32_t(right), int32_t(bottom)}, destination,
            {int32_t(left + dx), int32_t(top + dy), int32_t(right + dx), int32_t(bottom + dy)});
    }
    // Backend::Copy: on the GPU from a target drawn, otherwise in guest memory
    void CopySurfaces(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) {
        auto si = targets_.find(ResourceKey(src.data, false));
        GuestExtent guest{};
        SurfaceSize(src, guest.w, guest.h);
        if (si == targets_.end() || !si->second.drawn || si->second.guest.w != guest.w ||
            si->second.guest.h != guest.h) {
            if (CopySurfaceMemory(src, dst, count, rects, points)) {
                auto di = targets_.find(ResourceKey(dst.data, false));
                if (di != targets_.end()) CpuWrite(di->second);
            }
            return;
        }
        Image& in = si->second;
        if (!Resolve(in)) return;
        if (ResourceKey(dst.data, false) == si->first) return;
        Image* out = Target(dst, false);
        if (!out) return;
        CpuWrite(*out);
        if (!Resolve(*out)) return;
        const bool listed = count && rects;
        bool copied = false;
        for (uint32_t i = 0; i < (listed ? count : 1); ++i) {
            const GuestRect rect =
                listed ? ReadGuestRect(rects, i) : GuestRect{0, 0, int32_t(in.guest.w), int32_t(in.guest.h)};
            const GuestPoint point = points ? ReadGuestPoint(points, i) : GuestPoint{rect.left, rect.top};
            copied |= CopyRect(in, *out, rect, point);
        }
        if (copied) MarkWritten(*out);
    }

private:
    Self& Backend() { return static_cast<Self&>(*this); }
};

}  // namespace gpu
