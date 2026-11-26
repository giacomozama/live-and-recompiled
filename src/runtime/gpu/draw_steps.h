#pragma once
// The parts of a draw the D3D11 and OpenGL renderers share (gpu/d3d11.cpp, gpu/opengl.cpp): what it draws into and
// samples, its extra depth pass, constants, scissor and fur shells. Each backend's Draw calls them between its own
// shaders, buffers, state and binding.
//
// Self, the backend deriving from this (a friend of it), from Targets and from PostEffects, has Texture(state, stage,
// info, target, sampled), In(image), HudBars(color, target), VertexConstants(data, size), IssueGeometry(assembled),
// sprite_, hud_, replaying_ and last_vsc_valid_
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "app/app.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/hud_split.h"
#include "gpu/interp.h"
#include "gpu/targets.h"

namespace gpu {

template <class Self, class Image>
class DrawSteps {
protected:
    // a draw's four texture stages: what each reads, and for a render target how its size differs from the title's
    struct StageTextures {
        TexInfo info[4]{};
        bool is_rt[4]{}, exact[4]{};
        float scale[4] = {1, 1, 1, 1}, wide[4]{};
        Image* images[4]{};
    };

    // the state a draw is made with: its own, or a fixed sprite's with the stage it reads set up
    const State& SpriteState(const State& original, uint32_t prim) {
        if (!FixedSprite(original, prim)) return original;
        Self& self = Backend();
        self.sprite_ = original;
        SpriteStage(self.sprite_);
        return self.sprite_;
    }
    // an element aligned to min(4, its size), as D3D11 and Vulkan want: Xbox SHORT3 streams can have six byte offsets
    // and strides; those repacked into storage. bytes_of: a format's size, 0 for none (false: the draw dropped)
    template <class BytesOf>
    static bool AlignStreams(Assembled& a, std::array<std::vector<uint8_t>, 16>& storage, BytesOf bytes_of) {
        for (uint32_t slot = 0; slot < a.buffer_count; slot++) {
            auto& b = a.buffers[slot];
            auto& packed = storage[slot];
            bool repack = false;
            for (int r = 0; r < 16; r++)
                if ((a.regs >> r) & 1) {
                    const auto& e = a.elements[r];
                    if (e.buffer != slot) continue;
                    const uint32_t align = std::min<uint32_t>(4, bytes_of(e.format));
                    if (align && (e.offset % align || b.stride % align)) repack = true;
                }
            if (!repack || !b.stride) continue;
            uint32_t offsets[16]{}, stride = 0;
            for (int r = 0; r < 16; r++)
                if (((a.regs >> r) & 1) && a.elements[r].buffer == slot) {
                    const uint32_t size = bytes_of(a.elements[r].format), align = std::min<uint32_t>(4, size);
                    if (!align) return false;
                    stride = (stride + align - 1) & ~(align - 1);
                    offsets[r] = stride;
                    stride += size;
                }
            stride = (stride + 3) & ~3u;
            const uint32_t vertices = b.bytes / b.stride;
            packed.assign(size_t(vertices) * stride, 0);
            for (int r = 0; r < 16; r++)
                if (((a.regs >> r) & 1) && a.elements[r].buffer == slot) {
                    auto& e = a.elements[r];
                    const uint32_t size = bytes_of(e.format);
                    for (uint32_t v = 0; v < vertices; v++) {
                        const size_t source = size_t(v) * b.stride + e.offset;
                        if (source < b.bytes)
                            std::memcpy(packed.data() + size_t(v) * stride + offsets[r], b.src + source,
                                        std::min(size_t(size), size_t(b.bytes) - source));
                    }
                    e.offset = uint16_t(offsets[r]);
                }
            b.src = packed.data();
            b.bytes = uint32_t(packed.size());
            b.stride = stride;
        }
        return true;
    }
    // the color and depth targets drawn into (false: neither), and whether g_hud_split narrows the draw: a HUD draw
    // onto the frame, to its place
    bool DrawTargets(const State& s, const Assembled& a, uint32_t start, uint32_t count, const uint8_t* indices,
                     Image*& rt, Image*& ds, bool& hud) {
        Self& self = Backend();
        rt = self.Target(s.rt, false);
        ds = (s.rs[kRsZEnable] || s.rs[kRsStencilEnable]) ? self.Target(s.zs, true) : nullptr;
        if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
        if (!rt && !ds) return false;
        if (rt) self.CpuWrite(*rt);
        hud = rt && self.hud_.Place(s, a, start, count, indices, self.PosW(s), rt->guest.w, rt->guest.h,
                                    self.guest_frame_.w, self.guest_frame_.h, self.replaying_);
        return true;
    }
    static bool ShellsAbove(const State& s, const ProgramShading& shading) {
        return shading.top < 0 || s.vsc[size_t(shading.top)][0] > 0;
    }
    // fur's shells (depth_alpha's, top's x above 0) start the occlusion if nothing has yet: their depth, in only where
    // a strand is opaque enough, read by it speckled the fur with shadow
    void StartFurOcclusion(const State& s, const ProgramShading* shading, Image* rt) {
        Self& self = Backend();
        if (rt && shading && shading->depth_alpha > 0 && !self.occlusion_done_ && !self.post_done_ &&
            app::Get().ambient_occlusion && self.post_target_ == ResourceKey(rt->data, false) && ShellsAbove(s, *shading))
            self.ApplyOcclusion(*rt);
    }
    // each stage's texture, a target being drawn into read from a copy; views: what the shaders bind
    template <class View>
    void ReadStageTextures(const State& s, const Image* rt, const Image* ds, const Image& target, StageTextures& t,
                           View* views) {
        Self& self = Backend();
        const uint32_t sampled = SampledStages(s);
        for (int i = 0; i < 4; i++) {
            Image* tx = self.Texture(s, i, t.info[i], t.is_rt[i], ((sampled >> i) & 1) != 0);
            if (tx && (tx == rt || tx == ds)) tx = self.Snapshot(*tx);
            t.images[i] = tx;
            if (tx) views[i] = Self::In(*tx);
            t.exact[i] = (t.info[i].flags & kTexDepthBytes) != 0;
            if (tx && t.is_rt[i]) {
                t.scale[i] = float(tx->w) / tx->guest.w;
                const float sy = float(tx->h) / tx->guest.h;
                // Keep scaled downsample passes from skipping bright console texels.
                if (!tx->depth && !t.exact[i] && std::max(t.scale[i], sy) > 1.1f &&
                    (tx->guest.w > target.guest.w || tx->guest.h > target.guest.h)) {
                    t.wide[i] = std::max(t.scale[i], sy);
                    t.info[i].flags |= kTexWide;
                }
            }
        }
    }
    // the alpha above which a blended draw puts its depth in with a pass of its own after the color, 0 for none
    float DepthPassAlpha(const State& s, uint32_t start, uint32_t count, const uint8_t* indices, const Image* ds,
                         const ProgramShading* shading) {
        if (!ds || !s.rs[kRsZEnable] || !SimpleState(s, kMBlendEnable)) return 0;
        const bool depth_write = SimpleState(s, kMZWriteEnable) != 0;
        if (depth_write && shading && shading->depth_alpha > 0 && ShellsAbove(s, *shading)) return shading->depth_alpha;
        if (!depth_write && SurfaceDepth(s, start, count, indices)) return g_surface_depth.alpha;
        return 0;
    }
    void FillDrawConstants(const State& s, const Assembled& a, const ShaderChoice& sh, const PixelKey& pk,
                           StageTextures& t, const Image& target, bool sprite, DrawConstants& c) {
        const GuestExtent extent = target.guest;
        FillSurfaceConstants(s, extent, c);
        if (app::Get().fog != app::Fog::Disabled) FillFogConstants(s, sh.prog, c);
        FillCombinerConstants(s, c);
        FillTextureConstants(s, t.is_rt, t.exact, t.scale, t.wide, c);
        if (sh.shading) FillShadingConstants(s, sh.shading, c);
        if (!sh.shading && SimpleState(s, kMBlendEnable) && (pk.tex[0].flags & kTexDepthBytes)) {
            PsoKey blend_key{};
            KeyBlend(s, blend_key);
            NoteDepthOfField(pk, c.pc, blend_key.dst);
        }
        for (int i = 0; i < 4; ++i) {
            const Image* tx = t.images[i];
            if (!tx || !t.is_rt[i] || t.exact[i]) continue;
            c.texsize[i][3] = -0.5f / tx->h;
            if (t.wide[i] > 0) c.spread[i][1] = c.spread[i][0] * tx->w / tx->h;
        }
        if (sh.fixed_transform) {
            FillTransformConstants(s, extent, c);
            FillLightConstants(s, sh.fk, c);
        }
        if (a.topology == Topology::Points) {
            c.surf[3] = 1;
            c.ptpx[0] = float(target.w) / extent.w;
            c.ptpx[1] = float(target.h) / extent.h;
            FillPointConstants(s, sh.prog, sprite, c);
        }
    }
    // the title's viewport and scissor, in its own pixels (the shaders apply the viewport themselves)
    static GuestRect DrawClip(const State& s) {
        GuestRect clip{int32_t(s.viewport.x), int32_t(s.viewport.y), int32_t(s.viewport.x + s.viewport.w),
                       int32_t(s.viewport.y + s.viewport.h)};
        if (s.scissor_count) {
            clip.left = std::max(clip.left, int32_t(s.scissors[0]));
            clip.top = std::max(clip.top, int32_t(s.scissors[1]));
            clip.right = std::min(clip.right, int32_t(s.scissors[2]));
            clip.bottom = std::min(clip.bottom, int32_t(s.scissors[3]));
        }
        return clip;
    }
    // the scissor, in the target's pixels, narrowed with a HUD draw (g_hud_split) and never inside out
    template <class Rect>
    void FinishScissor(Rect& rect, const Image& target, bool hud) {
        const HudSplit& split = Backend().hud_;
        if (hud && (split.scale != 1 || split.shift != 0)) {
            using Edge = decltype(rect.left);
            const float vx = split.shift * (float(target.w) / float(std::max<uint32_t>(target.guest.w, 1)));
            rect.left = Edge(std::clamp(std::floor(float(rect.left) * split.scale + vx), 0.0f, float(target.w)));
            rect.right = Edge(std::clamp(std::ceil(float(rect.right) * split.scale + vx), 0.0f, float(target.w)));
        }
        rect.right = std::max(rect.right, rect.left);
        rect.bottom = std::max(rect.bottom, rect.top);
    }
    // fur's shells drawn again between the title's last two (beyond: past the top one)
    void DrawFurShells(const State& s, const Assembled& a, uint32_t shading, bool beyond) {
        if (!shading) return;
        const auto& fur = g_program_shading[shading - 1];
        if (fur.top < 0 || fur.top >= int(s.vsc.size())) return;
        const auto& top = s.vsc[size_t(fur.top)];
        const float* reg = fur.fill_register >= 0 && fur.fill_register < int(s.vsc.size())
                               ? s.vsc[size_t(fur.fill_register)].data()
                               : nullptr;
        int count;
        float from, to;
        if (beyond) {
            if (!fur.beyond_of || !fur.top_layers || top[0] <= 0 || int(top[3]) != fur.top_layers - 1) return;
            float reach = 0;
            count = fur.beyond_of(reg, top.data(), &reach);
            from = top[0];
            to = from * (1 + reach);
        } else {
            count = fur.between_live ? int(*fur.between_live) : fur.between;
            if (fur.between_of) {
                const int picked = fur.between_of(reg, top.data());
                if (picked >= 0) count = picked;
            }
            from = ShadingBefore(shading);
            to = top[0];
        }
        if (count <= 0 || from < 0 || to <= from) return;
        Self& self = Backend();
        auto vertices = s.vsc.v;
        if (fur.between_mark >= 0 && fur.between_mark < int(s.vsc.size()))
            vertices[size_t(fur.between_mark)][1] = -1;
        for (int shell = 1; shell <= count; ++shell) {
            vertices[size_t(fur.top)][0] = from + (to - from) * float(shell) / float(count + !beyond);
            if (!self.VertexConstants(vertices.data(), sizeof(vertices))) break;
            self.IssueGeometry(a);
        }
        self.last_vsc_valid_ = self.VertexConstants(s.vsc.data(), sizeof(s.vsc.v));
    }
    // what a draw leaves: the HUD's bars, and its targets written
    void FinishDraw(const State& s, Image* rt, Image* ds, Image* color_target, const Image& target, bool fur_mask,
                    bool multisampled, bool depth_pass) {
        Self& self = Backend();
        if (self.hud_.bars && color_target) self.HudBars(*color_target, target);
        if (rt) {
            if (fur_mask) rt->fur_frame = self.frame_;
            self.MarkWritten(*rt, multisampled);
        }
        if (ds && (SimpleState(s, kMZWriteEnable) || depth_pass || s.rs[kRsStencilEnable]))
            self.MarkWritten(*ds, multisampled);
    }

private:
    Self& Backend() { return static_cast<Self&>(*this); }
};

}  // namespace gpu
