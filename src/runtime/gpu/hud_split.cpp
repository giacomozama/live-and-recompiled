// g_hud_split for the renderers, see hud_split.h
#include "gpu/hud_split.h"

#include <cmath>

#include "app/app.h"
#include "gpu/interp.h"
#include "gpu/vertex.h"

namespace gpu {

HudAnchor HudEdge(const HudBox& b, float gw) {
    const float left = b.x0, right = gw - b.x1, middle = std::fabs(b.Mid() - gw * 0.5f);
    if (middle <= left && middle <= right) return kHudMiddle;
    return left <= right ? kHudLeft : kHudRight;
}

bool HudMeasure(const State& s, const Assembled& a, uint32_t start, uint32_t count, const uint8_t* indices, int pos_w,
                float guest_w, HudBox& b) {
    if (!count || count > kHudVertices || !ScreenSpace(s, a, pos_w)) return false;
    float v[kHudVertices][4];
    if (!ReadInput(s, 0, start, indices, count, v)) return false;
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    int rows = -1;  // a program's position rows, from c[rows]
    if (prog) {
        if (pos_w == kPosWInput) prog = false;
        else if (pos_w >= 3 && size_t(pos_w) < s.vsc.size()) rows = pos_w - 3;
        else return false;
    }
    float m[16] = {};
    const bool as_given = a.pretransformed || ((s.vs_handle & 1) && !prog);  // on the screen already
    if (!as_given && !prog) {
        auto mul = [](const float* l, const float* r, float* o) {
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    o[i * 4 + j] = l[i * 4] * r[j] + l[i * 4 + 1] * r[4 + j] + l[i * 4 + 2] * r[8 + j] + l[i * 4 + 3] * r[12 + j];
        };
        constexpr size_t kView = 2, kProjection = 3, kWorld = 256;  // D3DTRANSFORMSTATETYPE
        float wv[16];
        mul(s.transforms[kWorld].data(), s.transforms[kView].data(), wv);
        mul(wv, s.transforms[kProjection].data(), m);
    }
    b = {};
    for (uint32_t i = 0; i < count; i++) {
        const float* p = v[i];
        float x = p[0], y = p[1];
        if (prog) {
            auto dot = [&](size_t r) {
                const auto& c = s.vsc[r];
                return p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] * c[3];
            };
            const float w = dot(size_t(rows) + 3);
            if (!(std::fabs(w) > 1e-6f)) return false;
            x = dot(size_t(rows)) / w * s.vsc[58][0] + s.vsc[59][0];
            y = dot(size_t(rows) + 1) / w * s.vsc[58][1] + s.vsc[59][1];
        } else if (!as_given) {
            const float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
            const float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
            const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
            if (!(std::fabs(w) > 1e-6f)) return false;
            x = (cx / w + 1) * 0.5f * float(s.viewport.w) + float(s.viewport.x);
            y = (1 - cy / w) * 0.5f * float(s.viewport.h) + float(s.viewport.y);
        }
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        b.Add({x, y, x, y});
    }
    const float gw = guest_w;
    return !(b.x1 < -gw || b.x0 > gw * 2);  // nowhere near the frame: not what it seems
}

bool HudSplit::Place(const State& s, const Assembled& a, uint32_t start, uint32_t count, const uint8_t* indices,
                     int pos_w, uint32_t target_gw, uint32_t target_gh, uint32_t guest_w, uint32_t guest_h, bool replay) {
    const interp::Tag& tag = interp::g_run_tag;
    const uint8_t mode = tag.hud_mode;
    if (mode != kHudCentred && !g_hud_split.load(std::memory_order_relaxed)) return false;
    if (mode == kHudNone || !tag.hud[0]) return false;
    if (target_gw != guest_w || target_gh != guest_h) return false;
    const float gw = float(guest_w), gh = float(guest_h);
    const float wider = app::FrameAspect() * gh / gw;  // the frame's width as the window shows it, over its own
    if (wider < 1.01f) return false;
    took = 'S';
    box = {};
    if (mode == kHudStretched) return true;
    if (mode == kHudCentred) {
        scale = 1 / wider;
        shift = gw * 0.5f * (1 - scale);
        took = 'C';
        return true;
    }
    HudBox b;
    const bool measured = HudMeasure(s, a, start, count, indices, pos_w, gw, b);
    HudElement& e = elements[tag.hud[0]];
    if (measured && !replay) {
        if (e.first_now.Empty()) e.first_now = b;
        e.now.Add(b);
    }
    float mid = -1;  // the middle the frame's shape is taken about
    HudAnchor anchor = kHudNoEdge;
    if (mode == kHudInPlaceEach) {
        if (measured) mid = b.Mid();
    } else if (mode == kHudInPlace || mode == kHudInPlaceBars) {
        // during the frame's drawing again, its own boxes are last already (Roll)
        const HudBox& first = replay ? e.first_last : e.first_now;
        if (!e.last.Empty() && !first.Empty() && !e.first_last.Empty())
            mid = e.last.Mid() + (replay ? 0 : first.Mid() - e.first_last.Mid());
        else if (!first.Empty()) mid = first.Mid();  // the first draw's, the same for each of the frame's draws
        else if (measured) mid = b.Mid();
    } else {
        anchor = e.anchor;
        if (anchor == kHudNoEdge) {  // new: the third of its box so far
            const HudBox& so_far = !e.now.Empty() ? e.now : e.last;
            if (!so_far.Empty()) anchor = HudEdge(so_far, gw);
        }
    }
    const float sc = 1 / wider;
    if (anchor != kHudNoEdge) {
        scale = sc;
        shift = anchor == kHudRight ? gw * (1 - sc) : anchor == kHudMiddle ? gw * 0.5f * (1 - sc) : 0;
        took = anchor == kHudLeft ? 'L' : anchor == kHudRight ? 'R' : 'M';
    } else if (mid >= 0) {
        scale = sc;
        shift = mid * (1 - sc);
        took = 'P';
        bars = mode == kHudInPlaceBars && measured && b.x0 <= 1 && b.x1 >= gw - 1;
    }
    if (measured) box = b;
    return true;
}

void HudSplit::Roll(uint32_t guest_w) {
    const float gw = float(guest_w);
    for (auto it = elements.begin(); it != elements.end();) {
        HudElement& e = it->second;
        if (e.now.Empty()) {
            if (++e.unseen > kHudForget) {
                it = elements.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        e.unseen = 0;
        const HudAnchor own = HudEdge(e.now, gw);
        if (e.anchor == kHudNoEdge || own == e.anchor) {
            e.anchor = own;
            e.against = 0;
        } else if (++e.against >= kHudTurnFrames) {
            e.anchor = own;
            e.against = 0;
        }
        e.last = e.now;
        e.first_last = e.first_now;
        e.now = e.first_now = {};
        ++it;
    }
}

}  // namespace gpu
