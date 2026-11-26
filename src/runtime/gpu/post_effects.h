#pragma once
// The post effects the D3D11 and OpenGL renderers share (gpu/d3d11.cpp, gpu/opengl.cpp): which target and view they
// are for, their order and their pass constants. Vulkan has its own.
//
// Self, the backend deriving from it (a friend of this) and from Targets (gpu/targets.h), has Targets' and:
//   PictureViewport and RenderSize
//   FullScreenPass(source, entry, destination, viewport, {Input...}, constants), into an Image or the screen
//   In and StencilIn: an image's color or depth, and its stencil, as a pass reads them
//   ViewportOf(width, height) and ViewportSize(viewport)
//   SmaaLookups(area, search), ClearColor(image), StencilSnapshot(depth) and OutlineSource()
//   HdrHighlights and KeepHdrWorld
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "app/app.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/fsr_hlsl.h"
#include "gpu/post_hlsl.h"
#include "gpu/targets.h"

namespace gpu {

template <class Self, class Image, class Input>
class PostEffects {
protected:
    Image post_edges_, post_weights_, post_blend_, fsr_easu_;
    Image occlusion_, occlusion_blur_, motion_blur_, outline_;
    std::unordered_map<uint64_t, int> position_w_;
    uint64_t post_target_ = 0;
    uint64_t post_depth_ = 0;
    bool post_done_ = false, occlusion_done_ = false, view_ok_ = false, many_views_ = false;
    float post_zscale_ = 1;
    std::array<float, 4> view_viewport_[2], view_projection_[4];
    std::array<float, 4> camera_[3], previous_camera_[3];
    uint32_t camera_frame_ = 0, previous_camera_frame_ = 0;
    bool camera_seen_ = false, previous_camera_seen_ = false;

    bool Smaa(Image& source, Image& destination) {
        Self& self = Backend();
        Input area{}, search{};
        if (!self.SmaaLookups(area, search) || !self.ScratchColor(post_edges_, source.w, source.h) ||
            !self.ScratchColor(post_weights_, source.w, source.h))
            return false;
        const float params[8] = {1.0f / source.w, 1.0f / source.h, float(source.w), float(source.h)};
        const auto viewport = self.ViewportOf(source.w, source.h);
        self.ClearColor(post_edges_);
        return self.FullScreenPass(SmaaHlsl(0), "edges", post_edges_, viewport, {self.In(source)}, params) &&
               self.FullScreenPass(SmaaHlsl(1), "weights", post_weights_, viewport,
                                   {self.In(post_edges_), area, search}, params) &&
               self.FullScreenPass(SmaaHlsl(2), "blend", destination, viewport,
                                   {self.In(source), self.In(post_weights_)}, params);
    }
    bool PostWanted() const {
        const auto& set = app::Get();
        return set.smaa || set.film_grain || set.chromatic_aberration ||
               (set.ambient_occlusion && g_view_constants.viewport >= 0) ||
               (set.motion_blur && g_view_constants.camera >= 0) || g_outlines.load(std::memory_order_relaxed);
    }
    void TrackView(const State& s, const Image* depth) {
        const auto& set = app::Get();
        const auto& vc = g_view_constants;
        if ((!set.ambient_occlusion && !set.motion_blur && !g_outlines.load(std::memory_order_relaxed)) || !depth ||
            vc.viewport < 0 || vc.projection < 0 || !(s.vs_handle & 1) || !s.rs[kRsZEnable] || !s.zscale)
            return;
        const std::array<float, 4> vp[] = {s.vsc[size_t(vc.viewport)], s.vsc[size_t(vc.viewport + 1)]};
        std::array<float, 4> pr[4];
        for (int i = 0; i < 4; ++i) pr[i] = s.vsc[size_t(vc.projection + i)];
        // the off-diagonal zeros too, as Vulkan's TrackView: a program keeping other things in the first row (Frankie's
        // line draw in BarnInside) leaves the other three rows a projection's
        const bool perspective = pr[0][0] && !pr[0][1] && !pr[1][0] && pr[1][1] && !pr[2][0] && !pr[2][1] &&
                                 pr[2][3] && pr[3][0] == 0 && pr[3][1] == 0 && pr[3][2] == -1 && pr[3][3] == 0 &&
                                 vp[0][0] && vp[0][1] && vp[0][2];
        if (!perspective) return;
        if (view_ok_ && (vp[0] != view_viewport_[0] || vp[1] != view_viewport_[1])) many_views_ = true;
        std::copy(std::begin(vp), std::end(vp), view_viewport_);
        std::copy(std::begin(pr), std::end(pr), view_projection_);
        post_depth_ = ResourceKey(depth->data, true);
        post_zscale_ = s.zscale;
        view_ok_ = true;

        if (vc.camera < 0) return;
        const auto& tag = s.vsc[size_t(vc.camera + 3)];
        if (tag[3] != 1 || tag[1] != 0 || tag[2] != 0) return;
        const uint32_t frame = uint32_t(tag[0]);
        if (!camera_seen_ || frame != camera_frame_) {
            std::copy(std::begin(camera_), std::end(camera_), previous_camera_);
            previous_camera_frame_ = camera_frame_;
            previous_camera_seen_ = camera_seen_;
        }
        for (int i = 0; i < 3; ++i) camera_[i] = s.vsc[size_t(vc.camera + i)];
        camera_frame_ = frame;
        camera_seen_ = true;
    }
    Image* PostDepth(const Image& target) {
        if (!view_ok_ || many_views_) return nullptr;
        Self& self = Backend();
        auto it = self.targets_.find(post_depth_);
        if (it == self.targets_.end() || it->second.w != target.w || it->second.h != target.h) return nullptr;
        return self.Snapshot(it->second);
    }
    std::array<float, 20> ViewParams(const Image& target, float strength, float reach) const {
        const auto& s = view_viewport_[0];
        const auto& o = view_viewport_[1];
        const auto& x = view_projection_[0];
        const auto& y = view_projection_[1];
        const auto& z = view_projection_[2];
        return {1.0f / target.w,
                1.0f / target.h,
                strength,
                reach,
                float(target.guest.w),
                float(target.guest.h),
                (o[2] - s[2] * z[2]) / post_zscale_,
                s[2] * z[3] / post_zscale_,
                s[0],
                s[1],
                o[0],
                o[1],
                x[0],
                x[2],
                x[3],
                0,
                y[1],
                y[2],
                y[3],
                0};
    }
    void ApplyOcclusion(Image& target) {
        occlusion_done_ = true;
        const auto& set = app::Get();
        if (!set.ambient_occlusion) return;
        Self& self = Backend();
        Image* depth = PostDepth(target);
        if (!depth) return;
        Image* source = self.Snapshot(target);
        if (!source) return;
        const uint32_t w = (target.w + 1) / 2, h = (target.h + 1) / 2;
        if (!self.ScratchColor(occlusion_, w, h) || !self.ScratchColor(occlusion_blur_, w, h)) return;
        const auto params = ViewParams(target, float(set.ambient_occlusion_radius), set.ambient_occlusion / 100.0f);
        const auto small = self.ViewportOf(w, h);
        const auto full = self.ViewportOf(target.w, target.h);
        const Input z = self.In(*depth), ao = self.In(occlusion_);
        if (self.FullScreenPass(kOcclusionHlsl, "occlusion", occlusion_, small, {z, z}, params) &&
            self.FullScreenPass(kOcclusionHlsl, "blurx", occlusion_blur_, small, {ao, z}, params) &&
            self.FullScreenPass(kOcclusionHlsl, "blury", occlusion_, small, {self.In(occlusion_blur_), z}, params) &&
            self.FullScreenPass(kOcclusionHlsl, "apply", target, full, {self.In(*source), ao}, params))
            self.MarkWritten(target);
    }
    bool ApplyMotion(Image& source, Image& destination, const Image& target) {
        const auto& set = app::Get();
        if (!set.motion_blur || !camera_seen_ || !previous_camera_seen_ ||
            ((camera_frame_ - previous_camera_frame_) & 0xFFFFFF) != 1)
            return false;
        float path[12];
        if (!CameraMotion(camera_, previous_camera_, path)) return false;
        Image* depth = PostDepth(target);
        if (!depth) return false;
        const auto view = ViewParams(target, set.motion_blur / 100.0f, 0.06f);
        float params[32];
        std::copy(view.begin(), view.end(), params);
        std::copy(std::begin(path), std::end(path), params + view.size());
        Self& self = Backend();
        return self.FullScreenPass(kMotionBlurHlsl, "blur", destination, self.ViewportOf(target.w, target.h),
                                   {self.In(source), self.In(*depth)}, params);
    }
    // g_outlines: the stencil holding the marks (d3d::OutlineMark), the frame's depth buffer (TrackView) at the
    // picture's size, as Vulkan's ApplyPost finds it
    Image* OutlineMarks(const Image& target) {
        if (!g_outlines.load(std::memory_order_relaxed) || !post_depth_) return nullptr;
        Self& self = Backend();
        auto it = self.targets_.find(post_depth_);
        if (it == self.targets_.end() || it->second.w != target.w || it->second.h != target.h) return nullptr;
        return self.StencilSnapshot(it->second);
    }
    // the outlines round the marked players (kOutlineHlsl): the line one of the console's pixels wide, at least one
    // of the picture's, thinner past 600 units (6 m) where the frame's view is known
    bool ApplyOutline(Image& source, Image& destination, Image& marks, const Image& target) {
        const float width = std::max(1.0f, float(target.w) / float(std::max<uint32_t>(target.guest.w, 1)));
        const auto &vs = view_viewport_[0], &vo = view_viewport_[1], &vz = view_projection_[2];
        const bool known = view_ok_ && post_zscale_ != 0;
        const float params[] = {1.0f / target.w,
                                1.0f / target.h,
                                width,
                                600.0f,
                                known ? (vo[2] - vs[2] * vz[2]) / post_zscale_ : 0.0f,
                                known ? vs[2] * vz[3] / post_zscale_ : 0.0f,
                                known ? 1.0f : 0.0f,
                                float(g_team_colors.load(std::memory_order_relaxed))};
        Self& self = Backend();
        const Input stencil = self.StencilIn(marks);
        return self.FullScreenPass(Self::OutlineSource(), "outline", destination,
                                   self.ViewportOf(target.w, target.h), {self.In(source), stencil, self.In(marks)},
                                   params);
    }
    // the draw's program's PositionWSource, kPosWUnknown without one
    int PosW(const State& s) {
        if (!(s.vs_handle & 1) || !s.vs || s.vs->program.empty()) return kPosWUnknown;
        EnsureProgramHash(s);
        auto [it, inserted] = position_w_.try_emplace(s.vs->hash, kPosWUnknown);
        if (inserted) it->second = PositionWSource(s.vs->program);
        return it->second;
    }
    void ApplyPost(Image& target) {
        post_done_ = true;
        Self& self = Backend();
        const auto& set = app::Get();
        const bool grade = set.film_grain || set.chromatic_aberration;
        if (!occlusion_done_) ApplyOcclusion(target);
        Image* marks = OutlineMarks(target);
        if (!set.smaa && !grade && !set.motion_blur && !marks) return;
        Image* source = self.Snapshot(target);
        if (!source) return;
        // each effect reads what the one before wrote, the last writes the target
        if (set.motion_blur) {
            if (!set.smaa && !grade && !marks) {
                if (ApplyMotion(*source, target, target)) self.MarkWritten(target);
                return;
            }
            if (self.ScratchColor(motion_blur_, target.w, target.h) && ApplyMotion(*source, motion_blur_, target))
                source = &motion_blur_;
        }
        if (marks) {
            if (!set.smaa && !grade) {
                if (ApplyOutline(*source, target, *marks, target)) self.MarkWritten(target);
                return;
            }
            if (self.ScratchColor(outline_, target.w, target.h) && ApplyOutline(*source, outline_, *marks, target))
                source = &outline_;
        }
        if (set.smaa) {
            if (!grade) {
                if (Smaa(*source, target)) self.MarkWritten(target);
                return;
            }
            if (self.ScratchColor(post_blend_, target.w, target.h) && Smaa(*source, post_blend_))
                source = &post_blend_;
        }
        if (!grade) return;
        const float params[] = {1.0f / target.w,
                                1.0f / target.h,
                                float(target.w),
                                float(target.h),
                                set.chromatic_aberration / 100.0f,
                                set.film_grain / 100.0f,
                                float(self.frame_ % 1024),
                                0.0f};
        if (self.FullScreenPass(kPostHlsl, "grade", target, self.ViewportOf(target.w, target.h), {self.In(*source)},
                                params))
            self.MarkWritten(target);
    }
    // a picture about to be shown: the effects run over it, if no 2D drawn over the frame had them run
    void FinishPost(Image& image) {
        if (post_done_ || post_target_ != ResourceKey(image.data, false)) return;
        if (PostWanted()) ApplyPost(image);
        Backend().KeepHdrWorld(image);
    }
    // the next picture's view and effects start over
    void ResetView() {
        post_target_ = 0;
        post_done_ = occlusion_done_ = view_ok_ = many_views_ = false;
    }
    // each draw: the 3D's target and view noted, the effects applied before the first 2D drawn over it
    void TrackPost(const State& state, const Assembled& assembled, bool program, Image* target, Image* depth) {
        Self& self = Backend();
        if (!target || post_done_ || (!PostWanted() && !self.HdrHighlights())) return;
        const int w_source = program ? PosW(state) : kPosWUnknown;
        const auto key = ResourceKey(target->data, false);
        const bool flat = ScreenSpace(state, assembled, w_source);
        if (flat && !occlusion_done_ && post_target_ && app::Get().ambient_occlusion) {
            bool uses_frame = key == post_target_;
            for (uint32_t header : state.textures) {
                if (uses_frame) break;
                TextureDesc texture;
                uses_frame = header && Describe(header, texture) && ResourceKey(texture.data, false) == post_target_;
            }
            // not the shrink into the screen grab (g_grab_texture): 3D is drawn after it
            if (uses_frame && key != post_target_) {
                TextureDesc grab;
                const uint32_t tex = g_grab_texture.load(std::memory_order_relaxed);
                if (tex && Describe(tex, grab) && ResourceKey(grab.data, false) == key) uses_frame = false;
            }
            auto it = self.targets_.find(post_target_);
            if (uses_frame && it != self.targets_.end()) ApplyOcclusion(it->second);
        }
        if (target->guest.w != self.guest_frame_.w || target->guest.h != self.guest_frame_.h) return;
        if (!flat) {
            post_target_ = key;
            TrackView(state, depth);
            return;
        }
        if (post_target_ != key) return;
        TextureDesc texture;
        if (state.textures[0] && Describe(state.textures[0], texture) &&
            self.targets_.count(ResourceKey(texture.data, Format(texture.format).depth)))
            return;
        ApplyPost(*target);
        self.KeepHdrWorld(*target);
    }
    // FSR 1: the picture upscaled to the output (EASU) where it is smaller, then sharpened onto the screen (RCAS)
    template <class Screen, class Viewport>
    bool DrawFsr(Image& picture, Screen&& screen, const Viewport& viewport) {
        Self& self = Backend();
        const auto& set = app::Get();
        const auto [screen_w, screen_h] = Self::ViewportSize(viewport);
        const uint32_t width = self.RenderSize(screen_w), height = self.RenderSize(screen_h);
        const float fur_sharpening = picture.fur_frame != UINT64_MAX ? set.fur_sharpness / 100.0f : 1.0f;
        Image* source = &picture;
        if (picture.w < width || picture.h < height) {
            const float ratio_x = float(picture.w) / width, ratio_y = float(picture.h) / height;
            const float inv_w = 1.0f / picture.w, inv_h = 1.0f / picture.h;
            const float constants[] = {ratio_x,       ratio_y,        0.5f * ratio_x - 0.5f, 0.5f * ratio_y - 0.5f,
                                       inv_w,         inv_h,          inv_w,                 -inv_h,
                                       -inv_w,        2 * inv_h,      inv_w,                 2 * inv_h,
                                       0.0f,          4 * inv_h,      float(width),          float(height),
                                       fur_sharpening, 0.0f,          0.0f,                  0.0f};
            if (!self.ScratchColor(fsr_easu_, width, height) ||
                !self.FullScreenPass(kFsrHlsl, "easu", fsr_easu_, self.ViewportOf(width, height), {self.In(picture)},
                                     constants))
                return false;
            source = &fsr_easu_;
        }
        // con1.w the colour vision simulation (none here); con2.x no separate fur mark, con2.y the fade
        const float constants[] = {1.0f / width,
                                   1.0f / height,
                                   set.fsr_sharpness / 100.0f,
                                   0.0f,
                                   fur_sharpening,
                                   float(set.gamma) / 220.0f,
                                   float(set.brightness) / 100.0f,
                                   0.0f,
                                   0.0f,
                                   g_fade.load(),
                                   0.0f,
                                   0.0f};
        static const std::string rcas_source = std::string("#define FSR_PRESENTATION_FADE\n") + kFsrHlsl;
        return self.FullScreenPass(rcas_source.c_str(), "rcas", screen, viewport, {self.In(*source)}, constants);
    }

private:
    Self& Backend() { return static_cast<Self&>(*this); }
};

}  // namespace gpu
