// Speech bubbles drawn again (common/subtitles.h): the game's bubble sprites, stretched with the 4:3 frame on a wider
// display, are replaced by a shape of the runtime's around the bubble's words, at the game's place for it.
//
// 0xE1880 draws a bubble (the object at [esp+4]): its tail, then its body, both sprites through 0x356750 (the body at
// [obj+0x70], [obj+0x74], from 0xE21F8; the tail from 0xE215C, or rotated toward its speaker through 0x356790 from
// 0xE2022 or 0xE212A, along [obj+0xB4], [obj+0xB8]), then its words (0xE2940, eax the object: each line from [obj+0x78]
// less the half width at a height from [obj+0x7C], black, or white for an electronic one). A sprite (eax its record:
// +0xA its alpha, +0xD its flips) is placed by 0x3567E0 / 0x356950, which leave its box on the 640 by 480 frame in
// 0x801740 (left; the middle for a rotated one), 0x80173C (top), 0x801734 (width), 0x801730 (height), and drawn by
// 0x356AA0 / 0x358CA0 (a DrawVerticesUP of 28-byte vertices: x y z, colour, u v), its colour the speaker's
// (0x801758..5A, red green blue) when [0x80174C] is set.
//
// Here the tail and body sprites are drawn see-through (alpha 0), their boxes kept, and right after the body's draw,
// in the state it left, the runtime's bubble is drawn: an ellipse with a pixel's soft edge, its tail one with it, all
// in the display's proportions (the frame's x narrowed by how far the window widens it). It fits the words' box (the
// glyphs' boxes as the frames before drew them, ConkerTextQuad, over the page) no taller than the body, about the
// body's middle, its size eased, and the words are moved to its middle. Thought bubbles (descriptor
// flag 2) stay the game's.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "common/menus.h"
#include "common/subtitles.h"
#include "core/diag.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

using kernel::R32;

float F(uint32_t va) {
    float f;
    std::memcpy(&f, mem::Base() + va, 4);
    return f;
}
void PutF(uint32_t va, float f) { std::memcpy(mem::Base() + va, &f, 4); }
uint8_t B(uint32_t va) { return mem::Base()[va]; }

bool Trace() {
    static const bool on = diag::Env("XBE_BUBBLE_TRACE") != nullptr;
    return on;
}

constexpr uint32_t kThought = 2;
constexpr uint32_t kTailCall = 0xE2161, kTailTurnedCall1 = 0xE2027, kTailTurnedCall2 = 0xE212F, kBodyCall = 0xE21FD;
constexpr uint32_t kBoxLeft = 0x801740, kBoxTop = 0x80173C, kBoxWidth = 0x801734, kBoxHeight = 0x801730;
constexpr uint32_t kColoured = 0x80174C, kRed = 0x801758, kGreen = 0x801759, kBlue = 0x80175A;
constexpr uint32_t kSpriteAlpha = 0xA;
constexpr uint32_t kScale = 0x50, kAnchor = 0x68, kTextX = 0x78, kTextY = 0x7C, kPage = 0x48, kToward = 0xB4;
constexpr uint32_t kSpriteZ = 0x648D1C;  // the sprites' z
constexpr uint32_t kDrawVerticesUP = 0x53A600, kTriangleList = 5;
// texture stage 0's states as the library keeps them (0x356AA0 sets them for each sprite), and their dirty bit
constexpr uint32_t kColorOp = 0x547360, kColorArg1 = 0x547368, kColorArg2 = 0x54736C;
constexpr uint32_t kAlphaOp = 0x547370, kAlphaArg1 = 0x547378, kAlphaArg2 = 0x54737C;
constexpr uint32_t kDirty = 0x5499F0, kDirtyStages = 0x800;
constexpr uint32_t kSelectArg1 = 2, kModulate = 4, kDiffuse = 0, kTexture = 2;

struct Vertex {
    float x, y, z;
    uint32_t colour;
    float u, v, pad;
};
static_assert(sizeof(Vertex) == 0x1C);

struct Box {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool Empty() const { return x1 < x0; }
    void Add(float ax0, float ay0, float ax1, float ay1) {
        x0 = std::min(x0, ax0), y0 = std::min(y0, ay0), x1 = std::max(x1, ax1), y1 = std::max(y1, ay1);
    }
};

// what a bubble's last frames left. Its words' box is kept as the game would draw them (less the move they were drawn
// with) and from the body's middle as the game placed it, over the page; the ellipse's size is eased toward the
// frame's
struct Bubble {
    uint32_t page = 0;
    Box words;                   // over the page so far, from the body's middle, unmoved
    float raw_x = 0, raw_y = 0;  // the body's middle as the game placed it this frame
    float x = 0, y = 0;          // the ellipse's middle
    float width = 0, tall = 0;   // its size, eased (the display's pixels)
    float tip_x = 0, tip_y = 0;  // the tail's tip on the frame
    bool tip = false;
    std::chrono::steady_clock::time_point at{};  // when it last drew
    uint32_t seen = 0;                           // the frame it last drew in
};
std::unordered_map<uint32_t, Bubble> g_bubbles;
uint32_t g_frame = 0;

// the bubble 0xE1880 is drawing (0 none), its tail as found, and whether its words are drawing
uint32_t g_obj = 0;
bool g_tail = false, g_tail_turned = false;
float g_tail_x0, g_tail_y0, g_tail_w, g_tail_h;
bool g_words = false;
Box g_words_now;

uint32_t g_vertices = 0;
constexpr uint32_t kCapacity = 4096;  // vertices

float Wider() {
    const float w = app::FrameAspect() * 0.75f;
    return std::isfinite(w) && w > 0.5f ? w : 1.0f;
}

// a point and its outward normal along the outline, in the display's proportions about the body's middle
struct Point {
    float x, y, nx, ny;
    bool tip = false;  // the tail's tip
};

// the ellipse's angle (x = hx cos t, y = hy sin t) of its point nearest (x, y)
float Nearest(float hx, float hy, float x, float y) {
    auto d = [&](float t) { return std::hypot(hx * std::cos(t) - x, hy * std::sin(t) - y); };
    constexpr int kSamples = 256;
    constexpr float kTurn = 6.2831853f;
    float best = 0, at = 1e30f;
    for (int i = 0; i < kSamples; i++) {
        const float t = kTurn * float(i) / kSamples, e = d(t);
        if (e < at) at = e, best = t;
    }
    for (float step = kTurn / kSamples; step > 1e-5f; step *= 0.5f) {  // halved about the best
        if (d(best - step) < at) at = d(best - step), best -= step;
        else if (d(best + step) < at) at = d(best + step), best += step;
    }
    return best;
}

// the ellipse's angle the arc from angle t, going dir (1 or -1), is length long at
float Along(float hx, float hy, float t, int dir, float length) {
    constexpr float kStep = 0.002f, kMost = 1.5f;
    float run = 0, a = 0;
    while (run < length && a < kMost) {
        const float m = t + float(dir) * (a + kStep * 0.5f);
        run += std::hypot(hx * std::sin(m), hy * std::cos(m)) * kStep;
        a += kStep;
    }
    return t + float(dir) * a;
}

// the ellipse's outline, clockwise on the frame (y down), hx by hy its half axes; with a tail (its tip at tx, ty from
// the middle, its base about the ellipse's angle t0, base long along it each side), the arc of its base gives way to
// the tip. gap_from and gap_to: the base's arc's angles, from its end to its start
std::vector<Point> Outline(float hx, float hy, bool tail, float tx, float ty, float t0, float base,
                           float* gap_from = nullptr, float* gap_to = nullptr) {
    constexpr int kSteps = 128;
    constexpr float kTurn = 6.2831853f;
    auto at = [&](float t) {
        const float c = std::cos(t), s = std::sin(t);
        const float nx = c / hx, ny = s / hy, l = std::hypot(nx, ny);
        return Point{hx * c, hy * s, nx / l, ny / l};
    };
    std::vector<Point> p;
    if (tail) {
        const float from = Along(hx, hy, t0, -1, base), to = Along(hx, hy, t0, 1, base);
        if (gap_from) *gap_from = from, *gap_to = to;
        const float span = kTurn - (to - from);
        for (int i = 0; i <= kSteps; i++) p.push_back(at(to + span * float(i) / kSteps));
        const Point edge = at(t0);
        float nx = tx - edge.x, ny = ty - edge.y;
        const float l = std::hypot(nx, ny);
        if (l > 1e-3f) nx /= l, ny /= l;
        p.push_back({tx, ty, nx, ny, true});
        return p;
    }
    for (int i = 0; i < kSteps; i++) p.push_back(at(kTurn * float(i) / kSteps));
    return p;
}

// normals again from the outline's edges (the tail's sides), each the mean of its two edges', short at a sharp turn
void Normals(std::vector<Point>& p) {
    const size_t n = p.size();
    std::vector<Point> q = p;
    for (size_t i = 0; i < n; i++) {
        const Point& a = p[(i + n - 1) % n];
        const Point& b = p[i];
        const Point& c = p[(i + 1) % n];
        auto edge = [](const Point& s, const Point& e, float& nx, float& ny) {
            float dx = e.x - s.x, dy = e.y - s.y;
            const float l = std::hypot(dx, dy);
            if (l < 1e-5f) return false;
            nx = dy / l, ny = -dx / l;  // clockwise with y down: outward
            return true;
        };
        float n1x, n1y, n2x, n2y;
        if (!edge(a, b, n1x, n1y) || !edge(b, c, n2x, n2y)) continue;
        float mx = n1x + n2x, my = n1y + n2y;
        const float l = std::hypot(mx, my);
        if (l < 1e-3f) continue;
        mx /= l, my /= l;
        const float cosine = std::max(mx * n1x + my * n1y, 0.35f);  // a miter, no longer than about three times
        q[i].nx = mx / cosine, q[i].ny = my / cosine;
    }
    p.swap(q);
}

uint32_t Colour(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}

// the runtime's bubble, in the state the body's sprite draw left
void DrawBubble(X86Context& ctx, uint32_t rec) {
    Bubble& bub = g_bubbles[g_obj];
    const float wider = Wider();
    const float bx = F(kBoxLeft), by = F(kBoxTop), bw = F(kBoxWidth), bh = F(kBoxHeight);
    if (!(bw > 1 && bh > 1) || !std::isfinite(bx + by + bw + bh)) return;
    bub.raw_x = bx + bw * 0.5f, bub.raw_y = by + bh * 0.5f;
    // the game's scale of the bubble [obj+0x50]: from 0 as each line's opens (0xE0CD0) to 1, and back as it closes.
    // The body's box is scaled by it; the ellipse is sized at full scale, eased, then scaled
    const float scale = std::clamp(F(g_obj + kScale), 0.0f, 1.0f);
    if (!(scale > 0.01f)) return;
    const float full = bh / scale;  // the body's height at full scale
    // the ellipse about the words' box and a margin, its half axes kSnug times the box's half sides (the least ellipse
    // around a box has them root 2 times: a little less, as a page's last line is short), no taller than the body;
    // before the words have drawn, the body narrowed
    constexpr float kMargin = 8.0f, kSnug = 1.35f;
    float width = bw / scale, height = full;  // the display's pixels (the frame's x times wider)
    if (!bub.words.Empty()) {
        width = 2 * kSnug * ((bub.words.x1 - bub.words.x0) * wider * 0.5f + kMargin);
        height = std::min(2 * kSnug * ((bub.words.y1 - bub.words.y0) * 0.5f + kMargin), full);
    }
    width = std::max(width, height);
    // the tail's tip: toward its speaker from the turned tail's middle, as far as half its sprite's height, or the
    // bubble's anchor (the game sets its body off from it at an angle, its tail's corner on it)
    float tip_x = 0, tip_y = 0;
    if (g_tail && g_tail_turned) {
        float dx = float(int32_t(R32(g_obj + kToward))), dy = float(int32_t(R32(g_obj + kToward + 4)));
        const float l = std::hypot(dx, dy);
        if (l > 1e-3f) dx /= l, dy /= l;
        else dx = 0, dy = 1;
        tip_x = g_tail_x0 + dx * g_tail_h * 0.5f / wider, tip_y = g_tail_y0 + dy * g_tail_h * 0.5f;
    } else if (g_tail) {
        tip_x = F(g_obj + kAnchor), tip_y = F(g_obj + kAnchor + 4);
    }
    // its place and tip as the game's are, which jump where the game's do (a camera cut in a cutscene); its size eased
    // by the time gone, most of the way in a tenth of a second (a page's words of another width), but a bubble just
    // opening (as each line's does) or not drawn for a while starts at its size
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - bub.at).count();
    const bool fresh = bub.at == std::chrono::steady_clock::time_point{} || dt > 0.5f || scale < 0.15f;
    const float k = fresh ? 1.0f : 1.0f - std::exp(-std::max(dt, 0.0f) / 0.05f);
    bub.at = now;
    auto ease = [&](float& was, float to) { was += (to - was) * k; };
    ease(bub.width, width), ease(bub.tall, height);
    bub.x = bub.raw_x, bub.y = bub.raw_y;
    bub.tip_x = tip_x, bub.tip_y = tip_y;
    bub.tip = g_tail;
    const float cx = bub.x, cy = bub.y;
    const float hx = std::max(bub.width * scale * 0.5f, 1.0f), hy = std::max(bub.tall * scale * 0.5f, 1.0f);
    // the soft edge: a pixel as drawn (the frame's pixels times the render scale down), half in and half out
    const app::Scale render = app::RenderScale(640, 480);
    const float soft = render.y > 0.1f && std::isfinite(render.y) ? 1.0f / render.y : 1.0f;
    // the tail leaves the ellipse square, from its point nearest the tip, its base kTailBase long each side of that
    // along the ellipse and no more than kTailLong long (it stops short of a far anchor), both in the frame's pixels
    // (the display's proportions) whatever the ellipse's size. It comes out as the ellipse finishes opening and goes
    // back in before it closes: over the game's scale's last kTailOut its tip is drawn in toward the ellipse's edge
    constexpr float kTailBase = 9.0f, kTailLong = 36.0f, kTailOut = 0.3f;
    const float out = std::clamp((scale - (1 - kTailOut)) / kTailOut, 0.0f, 1.0f);
    const float ejected = out * out * (3 - 2 * out);
    float tx = (bub.tip_x - cx) * wider, ty = bub.tip_y - cy;
    float t0 = 0;
    bool tail = g_tail && (tx / hx) * (tx / hx) + (ty / hy) * (ty / hy) > 1;  // outside the ellipse
    if (tail) {
        // the farther the tip than kTailLong, the more the base slides from the nearest point toward where the line
        // from the middle to the tip leaves the ellipse, the tail aimed at the tip from there: cut short, it still
        // points the way
        const float nearest = Nearest(hx, hy, tx, ty), toward = std::atan2(ty / hy, tx / hx);
        const float far = std::hypot(tx - hx * std::cos(nearest), ty - hy * std::sin(nearest));
        const float slide = far > kTailLong ? 1 - kTailLong / far : 0;
        const float turn = std::remainder(toward - nearest, 6.2831853f);  // the shorter way round
        t0 = nearest + turn * slide;
        const float ex = hx * std::cos(t0), ey = hy * std::sin(t0);
        float dx = tx - ex, dy = ty - ey;
        const float l = std::hypot(dx, dy);
        const float length = std::min(l, kTailLong) * ejected;
        tail = length >= soft * 2 && l > 1e-3f;
        if (tail) {
            // aimed at the tip, but no more than kSlant off square to the ellipse there
            constexpr float kSlant = 0.35f;  // radians, some 20 degrees
            const float normal = std::atan2(std::sin(t0) / hy, std::cos(t0) / hx);
            const float off = std::remainder(std::atan2(dy, dx) - normal, 6.2831853f);
            const float aim = normal + std::clamp(off, -kSlant, kSlant);
            tx = ex + std::cos(aim) * length, ty = ey + std::sin(aim) * length;
        }
    }
    float gap_from = 0, gap_to = 0;
    std::vector<Point> p = Outline(hx, hy, tail, tx, ty, t0, kTailBase, &gap_from, &gap_to);
    Normals(p);

    // colours: the speaker's (white without) and the sprite's alpha
    const bool coloured = B(kColoured) != 0;
    const float fr = coloured ? B(kRed) / 255.0f : 1, fg = coloured ? B(kGreen) / 255.0f : 1,
                fb = coloured ? B(kBlue) / 255.0f : 1;
    const float alpha = B(rec + kSpriteAlpha) / 255.0f;
    // the fill shaded toward the tail: darker on its side of the ellipse, the most at its tip, a little lighter across
    float sx = 0, sy = 0;  // toward the tail, of the half axes
    if (p.back().tip) {
        const Point& tip = p.back();
        const float l = std::hypot(tip.x / hx, tip.y / hy);
        if (l > 1e-3f) sx = tip.x / hx / l, sy = tip.y / hy / l;
    }
    constexpr float kShade = 0.22f, kLight = 0.06f;
    auto fill = [&](float x, float y) {
        const float g = std::clamp((x / hx) * sx + (y / hy) * sy, -1.0f, 1.0f);  // 1 at the ellipse's edge by the tail
        const float dark = 1 - kShade * std::max(g, 0.0f), up = kLight * std::max(-g, 0.0f);
        return Colour((fr + (1 - fr) * up) * dark, (fg + (1 - fg) * up) * dark, (fb + (1 - fb) * up) * dark, alpha);
    };
    // a soft shadow below, outside the shape only (the shape is see-through): from its edge out by kBlur of its half
    // height, moved down by kDrop of it, kShadow dark, falling off fast
    constexpr float kBlur = 0.35f, kDrop = 0.12f, kShadow = 0.4f;
    const float blur = hy * kBlur, drop = hy * kDrop;
    const uint32_t shade0 = Colour(0, 0, 0, alpha * kShadow), shade1 = Colour(0, 0, 0, alpha * kShadow * 0.3f),
                   shade2 = Colour(0, 0, 0, 0);
    const float z = F(kSpriteZ);
    // a point of the display's back on the frame
    auto at = [&](float x, float y, uint32_t c) { return Vertex{cx + x / wider, cy + y, z, c, 0.5f, 0.5f, 0}; };
    std::vector<Vertex> v;
    auto band = [&](float ax0, float ay0, float bx0, float by0, uint32_t ca0, uint32_t cb0, float ax1, float ay1,
                    float bx1, float by1, uint32_t ca1, uint32_t cb1) {
        v.push_back(at(ax0, ay0, ca0)), v.push_back(at(ax1, ay1, ca1)), v.push_back(at(bx0, by0, cb0));
        v.push_back(at(bx0, by0, cb0)), v.push_back(at(ax1, ay1, ca1)), v.push_back(at(bx1, by1, cb1));
    };
    const size_t n = p.size();
    auto off = [](const Point& q, float d, float& x, float& y) { x = q.x + q.nx * d, y = q.y + q.ny * d; };
    for (size_t i = 0; i < n; i++) {  // the shadow first, under the bubble's soft edge
        const Point& a = p[i];
        const Point& b = p[(i + 1) % n];
        float ax0, ay0, bx0, by0, axm, aym, bxm, bym, ax1, ay1, bx1, by1;
        off(a, 0, ax0, ay0), off(b, 0, bx0, by0);
        off(a, blur * 0.35f, axm, aym), off(b, blur * 0.35f, bxm, bym);
        off(a, blur, ax1, ay1), off(b, blur, bx1, by1);
        aym += drop * 0.5f, bym += drop * 0.5f, ay1 += drop, by1 += drop;
        band(ax0, ay0, bx0, by0, shade0, shade0, axm, aym, bxm, bym, shade1, shade1);
        band(axm, aym, bxm, bym, shade1, shade1, ax1, ay1, bx1, by1, shade2, shade2);
    }
    // the fill: the whole ellipse fanned from its middle, and the tail fanned from its tip over the ellipse's arc
    // under its base, so no triangle reaches from the middle to the tip (its shading streaked across the bubble). Their
    // edges a half pixel in, where the soft edge starts, but where the two meet
    const size_t arc = tail ? n - 1 : n;  // the ellipse's points in the outline
    std::vector<float> fx, fy;           // the fill's edge on the outline
    for (size_t i = 0; i < n; i++) {
        float x, y;
        off(p[i], -soft * 0.5f, x, y);
        fx.push_back(x), fy.push_back(y);
    }
    std::vector<Point> gap;  // the base's arc, from the outline's last ellipse point to its first
    if (tail) {
        constexpr int kGap = 12;
        for (int i = 1; i < kGap; i++) {
            const float t = gap_from + (gap_to - gap_from) * float(i) / kGap;
            gap.push_back({hx * std::cos(t), hy * std::sin(t), 0, 0});
        }
    }
    // the sprite's draw leaves culling on (0x901): each triangle clockwise on the frame. Seen from the tail's tip,
    // outside the ellipse, its arc runs the other way: those go in backwards (flip)
    auto fan = [&](float ox, float oy, uint32_t oc, const std::vector<std::pair<float, float>>& ring, bool closed,
                   bool flip) {
        for (size_t i = 0; i + 1 < ring.size() + (closed ? 1 : 0); i++) {
            const auto& [ax, ay] = ring[i];
            const auto& [bx, by] = ring[(i + 1) % ring.size()];
            const Vertex o = at(ox, oy, oc), va = at(ax, ay, fill(ax, ay)), vb = at(bx, by, fill(bx, by));
            v.push_back(o), v.push_back(flip ? vb : va), v.push_back(flip ? va : vb);
        }
    };
    std::vector<std::pair<float, float>> ellipse;
    for (size_t i = 0; i < arc; i++) ellipse.push_back({fx[i], fy[i]});
    for (const Point& g : gap) ellipse.push_back({g.x, g.y});
    fan(0, 0, fill(0, 0), ellipse, true, false);
    if (tail) {
        std::vector<std::pair<float, float>> under{{fx[arc - 1], fy[arc - 1]}};
        for (const Point& g : gap) under.push_back({g.x, g.y});
        under.push_back({fx[0], fy[0]});
        fan(fx[n - 1], fy[n - 1], fill(fx[n - 1], fy[n - 1]), under, false, true);
    }
    // the soft edge all round the outline, the tail's sides too
    for (size_t i = 0; i < n; i++) {
        const Point& a = p[i];
        const Point& b = p[(i + 1) % n];
        float ax1, ay1, bx1, by1;
        off(a, soft * 0.5f, ax1, ay1), off(b, soft * 0.5f, bx1, by1);
        const uint32_t fa = fill(a.x, a.y), fb2 = fill(b.x, b.y);
        const uint32_t ca = fa & 0x00FFFFFFu, cb = fb2 & 0x00FFFFFFu;  // the same, clear
        band(fx[i], fy[i], fx[(i + 1) % n], fy[(i + 1) % n], fa, fb2, ax1, ay1, bx1, by1, ca, cb);
    }
    if (v.size() > kCapacity) return;
    if (!g_vertices) g_vertices = mem::AllocSystem(kCapacity * sizeof(Vertex));
    if (!g_vertices) return;
    std::memcpy(mem::Base() + g_vertices, v.data(), v.size() * sizeof(Vertex));
    // colour and alpha from the vertices, times the texture's middle (the sprite's fill)
    kernel::W32(kColorOp, kModulate), kernel::W32(kColorArg1, kDiffuse), kernel::W32(kColorArg2, kTexture);
    kernel::W32(kAlphaOp, kModulate), kernel::W32(kAlphaArg1, kDiffuse), kernel::W32(kAlphaArg2, kTexture);
    kernel::W32(kDirty, R32(kDirty) | kDirtyStages);
    const X86Context saved = ctx;
    kernel::CallGuest(ctx, kDrawVerticesUP, {kTriangleList, uint32_t(v.size()), g_vertices, uint32_t(sizeof(Vertex))});
    ctx = saved;
    (void)kSelectArg1;
    if (Trace()) {
        static uint32_t traced = 0;
        if (g_frame - traced >= 30) {
            traced = g_frame;
            LOGR("bubbles: %08X body %.1f %.1f %.1f x %.1f, words %.1f..%.1f %.1f..%.1f, "
                 "ellipse at %.1f %.1f %.1f x %.1f, tip %.1f %.1f, tail %d%s %.1f %.1f %.1f x %.1f, "
                 "colour %d %02X%02X%02X alpha %02X, wider %.3f",
                 g_obj, bx, by, bw, bh, bub.words.x0, bub.words.x1, bub.words.y0, bub.words.y1, cx, cy, bub.width,
                 bub.tall, bub.tip_x, bub.tip_y, int(g_tail), g_tail_turned ? " turned" : "", g_tail_x0, g_tail_y0,
                 g_tail_w, g_tail_h, int(coloured), B(kRed), B(kGreen), B(kBlue), B(rec + kSpriteAlpha),
                 double(wider));
        }
    }
}

// a tail or body sprite drawn see-through: its box kept, the draw's state left for the bubble's
template <void (*Imp)(X86Context&, uint8_t*)>
void SeeThrough(X86Context& ctx, uint8_t* base) {
    const uint32_t rec = ctx.eax;
    const uint8_t alpha = base[rec + kSpriteAlpha];
    base[rec + kSpriteAlpha] = 0;
    Imp(ctx, base);
    base[rec + kSpriteAlpha] = alpha;
}

}  // namespace

// [[hook]] 0xE1880 (subtitles.cpp) calls this around the bubble's drawing: obj the bubble, 0 at the end
void subtitles::BubbleDrawing(uint32_t obj) {
    if (obj) {
        g_obj = 0;
        g_tail = false;
        const uint32_t desc = R32(obj + 0x30);
        if (!desc || (R32(desc) & kThought)) return;
        g_obj = obj;
        Bubble& b = g_bubbles[obj];
        const uint32_t page = R32(obj + kPage);
        if (b.page != page) {
            b.page = page;
            b.words = {};
        }
        b.seen = ++g_frame;
        return;
    }
    g_obj = 0;
    g_tail = false;
    // the bubbles not drawn for a while go
    if (g_bubbles.size() > 16)
        for (auto it = g_bubbles.begin(); it != g_bubbles.end();)
            it = g_frame - it->second.seen > 120 ? g_bubbles.erase(it) : std::next(it);
}

// from ConkerTextQuad: a glyph's quad; one of a bubble's words into its box
void subtitles::BubbleGlyph() {
    if (!g_words) return;
    float sx[4], sy[4];
    if (!menus::GlyphScreen(sx, sy)) return;
    g_words_now.Add(std::min({sx[0], sx[1], sx[2], sx[3]}), std::min({sy[0], sy[1], sy[2], sy[3]}),
                    std::max({sx[0], sx[1], sx[2], sx[3]}), std::max({sy[0], sy[1], sy[2], sy[3]}));
}

X86_EXTERN_FUNC(__imp__sub_00356750);
X86_FUNC(sub_00356750) {
    const uint32_t from = R32(ctx.esp);
    if (!g_obj || (from != kTailCall && from != kBodyCall)) return __imp__sub_00356750(ctx, base);
    const uint32_t rec = ctx.eax;
    SeeThrough<__imp__sub_00356750>(ctx, base);
    if (from == kTailCall) {
        g_tail = true, g_tail_turned = false;
        g_tail_x0 = F(kBoxLeft), g_tail_y0 = F(kBoxTop), g_tail_w = F(kBoxWidth), g_tail_h = F(kBoxHeight);
        return;
    }
    DrawBubble(ctx, rec);
}

X86_EXTERN_FUNC(__imp__sub_00356790);
X86_FUNC(sub_00356790) {
    const uint32_t from = R32(ctx.esp);
    if (!g_obj || (from != kTailTurnedCall1 && from != kTailTurnedCall2)) return __imp__sub_00356790(ctx, base);
    SeeThrough<__imp__sub_00356790>(ctx, base);
    // 0x356950 keeps its middle where the box's left would be
    g_tail = true, g_tail_turned = true;
    g_tail_x0 = F(kBoxLeft), g_tail_y0 = F(kBoxTop), g_tail_w = F(kBoxWidth), g_tail_h = F(kBoxHeight);
}

// 0xE2940, a bubble's words (eax the bubble): moved to the ellipse's middle, their box measured. Each line starts from
// [obj+0x78] (0xE2940 takes [obj+0x58] off again) less the half width, at a height from [obj+0x7C]
X86_EXTERN_FUNC(__imp__sub_000E2940);
X86_FUNC(sub_000E2940) {
    const uint32_t obj = ctx.eax;
    if (!g_obj || obj != g_obj) return __imp__sub_000E2940(ctx, base);
    Bubble& b = g_bubbles[obj];
    // the words' middle as the game would draw them, from the body's, to the ellipse's
    const float mx = b.words.Empty() ? 0.0f : b.x - (b.raw_x + (b.words.x0 + b.words.x1) * 0.5f);
    const float my = b.y - b.raw_y;
    const float x = F(obj + kTextX), y = F(obj + kTextY);
    PutF(obj + kTextX, x + mx), PutF(obj + kTextY, y + my);
    g_words = true;
    g_words_now = {};
    __imp__sub_000E2940(ctx, base);
    g_words = false;
    PutF(obj + kTextX, x), PutF(obj + kTextY, y);
    if (!g_words_now.Empty())
        b.words.Add(g_words_now.x0 - mx - b.raw_x, g_words_now.y0 - my - b.raw_y, g_words_now.x1 - mx - b.raw_x,
                    g_words_now.y1 - my - b.raw_y);
}
