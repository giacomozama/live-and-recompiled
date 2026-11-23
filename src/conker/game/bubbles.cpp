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
// body's middle, its size eased, and the words are moved to its middle. A thought's bubble (descriptor flag 2, its
// circles 0x356750's from 0xE1C9D, 0xE1E4F and 0xE215C) is a cloud, a trail of circles in place of the tail.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
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
// a thought's circles: 0x356750 from 0xE1C9D and 0xE1E4F, and from 0xE215C as a speech bubble's tail
constexpr uint32_t kThoughtCall1 = 0xE1CA2, kThoughtCall2 = 0xE1E54;
constexpr uint32_t kTailCall = 0xE2161, kTailTurnedCall1 = 0xE2027, kTailTurnedCall2 = 0xE212F, kBodyCall = 0xE21FD;
constexpr uint32_t kBoxLeft = 0x801740, kBoxTop = 0x80173C, kBoxWidth = 0x801734, kBoxHeight = 0x801730;
constexpr uint32_t kColoured = 0x80174C, kRed = 0x801758, kGreen = 0x801759, kBlue = 0x80175A;
constexpr uint32_t kSpriteAlpha = 0xA, kSpriteFlips = 0xD;
constexpr uint32_t kState = 0x35, kShowing = 0x20, kWords = 0xE2940;
constexpr uint32_t kLineCount = 0x8472E0;  // the page's lines, as 0xE2940 last counted them
constexpr uint32_t kScale = 0x50, kAnchor = 0x68, kTextX = 0x78, kTextY = 0x7C, kPage = 0x48, kToward = 0xB4;
constexpr uint32_t kSpriteZ = 0x648D1C;  // the sprites' z
constexpr uint32_t kDrawVerticesUP = 0x53A600, kTriangleList = 5;
// texture stage 0's states as the library keeps them (0x356AA0 sets them for each sprite), and their dirty bit
constexpr uint32_t kColorOp = 0x547360, kColorArg1 = 0x547368, kColorArg2 = 0x54736C;
constexpr uint32_t kAlphaOp = 0x547370, kAlphaArg1 = 0x547378, kAlphaArg2 = 0x54737C;
constexpr uint32_t kDirty = 0x5499F0, kDirtyStages = 0x800;
constexpr uint32_t kSpeechTexture = 0x69E490, kSetTexture = 0x539960;  // D3DDevice_SetTexture (eax the stage)
constexpr uint32_t kModulate = 4, kDiffuse = 0, kTexture = 2;

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
    Box unseen;                  // as MeasureWords last measured them, for their size alone
    // a scrolling page's (more than four lines) all lines as MeasureWords measured them stepping through its scroll:
    // their widest (x0..x1) and the tallest of the steps (y1 - y0), for their size alone; swept once a page
    Box whole;
    bool swept = false;
    float raw_x = 0, raw_y = 0;  // the body's middle as the game placed it this frame
    float x = 0, y = 0;          // the ellipse's middle
    float width = 0, tall = 0;   // its size, eased (the display's pixels)
    float tip_x = 0, tip_y = 0;  // the tail's tip on the frame
    bool tip = false;
    float scale = 0;        // the scale drawn (DrawBubble)
    float game_last = 0;    // the game's the frame before
    int turn = 0;           // 1 opening, -1 closing, 0 neither
    std::chrono::steady_clock::time_point turn_at{}, seen_at{};
    std::chrono::steady_clock::time_point at{};  // when it last drew
    uint32_t seen = 0;                           // the frame it last drew in
};
std::unordered_map<uint32_t, Bubble> g_bubbles;
uint32_t g_frame = 0;

// the bubble 0xE1880 is drawing (0 none), its tail as found, and whether its words are drawing
uint32_t g_obj = 0;
bool g_tail = false, g_tail_turned = false;
bool g_thought = false;  // the bubble is a thought's (descriptor flag 2): a cloud and a trail of circles, no tail
float g_tail_x0, g_tail_y0, g_tail_w, g_tail_h;
uint8_t g_tail_flips = 0;  // the tail sprite's flips (+0xD): 1 mirrored across, 2 upside down
bool g_words = false;
Box g_words_now;
// the words are drawing unseen (MeasureWords): drawn as the game places them while it opens, which is not where it
// shows them (a box of both came out twice as wide), so only their size is kept
bool g_unseen = false;
// the words are drawing unseen with their alpha (MeasureWords' sweep): the lines' draws (0x1F31B0, 0x1F3340) get alpha 0
bool g_sweeping = false;

// the bubble is being placed alone (BubblePlacing): its sprites and words drawn not at all, the body's box at full
// scale kept (on the 640 by 480 frame)
bool g_place = false;
bool g_placed = false;
float g_placed_x, g_placed_y, g_placed_w, g_placed_h;

uint32_t g_vertices = 0;
constexpr uint32_t kCapacity = 65536;  // vertices (a thought's cloud, shadow, rim and lobed trail some 25000)

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

// a thought's cloud's lobes' radius on average, and how far it stands out of its ellipse, about (Cloud): the frame's
// pixels
float CloudLobe(float hy) { return std::clamp(hy * 0.74f, 11.0f, 70.0f); }
float CloudOut(float hy) { return CloudLobe(hy) * 0.35f; }
constexpr int kCloudFewest = 6;  // lobes, the least a cloud has
// each lobe's radius moves back and forth by this share over time, at this rate (rad/s)
constexpr float kWobble = 0.04f, kWobbleRate = 3.1f;
// points along each lobe's arc
constexpr int kArcSteps = 32;

// A thought's cloud: round lobes about the ellipse, its edge their circles' outer arcs. The circles' middles go
// round an ellipse inside it, evenly along it (by its length: by its angle they bunched at its narrow ends and left
// gaps along its long sides), each of lobe's radius on average (at least fewest of them; kWobble back and forth over
// time, by clock), its edge out of the ellipse by out, of sizes and spaced a little unevenly (from seed, the same
// while the page shows), close enough that each meets the next. Each lobe's arc runs outside from where it meets the
// one before to where it meets the next, with its circle's normals (each lobe's soft edge its own: the edge's
// normals, sharp at the dips, overlapped there in streaks); smooth gets them averaged over a lobe each side, for the
// shadow
std::vector<Point> Cloud(float hx, float hy, float lobe, float out, int fewest, uint32_t seed, float clock,
                         std::vector<Point>& smooth) {
    constexpr float kTurn = 6.2831853f;
    auto random = [&](uint32_t i) {  // 0..1
        uint32_t x = seed * 0x9E3779B1u ^ (i + 1) * 0x85EBCA77u;
        x ^= x >> 15, x *= 0x2C1B3C6Du, x ^= x >> 12, x *= 0x297A2D39u, x ^= x >> 15;
        return float(x >> 8) / float(1 << 24);
    };
    const float in = lobe - out;  // its middles inside the ellipse by lobe less out, its edge out by that
    const float ax = std::max(hx - in, hx * 0.3f), ay = std::max(hy - in, hy * 0.3f);
    // the inner ellipse's length at each of kTable angles, to space the middles by it
    constexpr int kTable = 512;
    std::vector<float> run(kTable + 1, 0.0f);
    for (int i = 0; i < kTable; i++) {
        const float m = kTurn * (float(i) + 0.5f) / kTable;
        run[i + 1] = run[i] + std::hypot(ax * std::sin(m), ay * std::cos(m)) * kTurn / kTable;
    }
    const float perimeter = run[kTable];
    const int lobes = std::max(fewest, int(std::ceil(perimeter / (lobe * 1.3f))));
    struct Circle {
        float x, y, r;
    };
    std::vector<Circle> circles;
    const float start = random(1000) * perimeter;
    for (int i = 0; i < lobes; i++) {
        float at = start + (float(i) + (random(uint32_t(2 * i)) - 0.5f) * 0.2f) * perimeter / float(lobes);
        at = std::fmod(at, perimeter);
        const int k = int(std::upper_bound(run.begin(), run.end(), at) - run.begin()) - 1;
        const float t = kTurn * (float(std::clamp(k, 0, kTable - 1)) + 0.5f) / kTable;
        const float wobble = 1 + kWobble * std::sin(clock * kWobbleRate + kTurn * random(uint32_t(3 * i + 7)));
        const float r = lobe * (0.8f + 0.35f * random(uint32_t(2 * i + 1))) * wobble;
        circles.push_back({ax * std::cos(t), ay * std::sin(t), r});
    }
    // where each circle meets the next: of their two crossings, the one farther out
    const size_t n = circles.size();
    std::vector<float> mx(n), my(n);
    std::vector<bool> meets(n);
    for (size_t i = 0; i < n; i++) {
        const Circle& a = circles[i];
        const Circle& b = circles[(i + 1) % n];
        const float dx = b.x - a.x, dy = b.y - a.y, d = std::hypot(dx, dy);
        meets[i] = d > 1e-3f && d < a.r + b.r && d > std::fabs(a.r - b.r);
        if (!meets[i]) {
            mx[i] = (a.x + b.x) * 0.5f, my[i] = (a.y + b.y) * 0.5f;
            continue;
        }
        const float l = (a.r * a.r - b.r * b.r + d * d) / (2 * d), hgt = std::sqrt(std::max(a.r * a.r - l * l, 0.0f));
        const float px = a.x + dx * l / d, py = a.y + dy * l / d;
        const float x1 = px - dy * hgt / d, y1 = py + dx * hgt / d, x2 = px + dy * hgt / d, y2 = py - dx * hgt / d;
        if (x1 * x1 + y1 * y1 >= x2 * x2 + y2 * y2) mx[i] = x1, my[i] = y1;
        else mx[i] = x2, my[i] = y2;
    }
    std::vector<Point> p;
    auto add = [&](float x, float y, float nx, float ny) { p.push_back({x, y, nx, ny}); };
    for (size_t i = 0; i < n; i++) {
        const Circle& c = circles[i];
        const size_t before = (i + n - 1) % n;
        // its arc from where it meets the one before to where it meets the next, the way round (increasing angle)
        float a0 = std::atan2(my[before] - c.y, mx[before] - c.x), a1 = std::atan2(my[i] - c.y, mx[i] - c.x);
        while (a1 <= a0) a1 += kTurn;
        if (a1 - a0 > kTurn * 0.75f || !meets[before] || !meets[i]) {  // swallowed, or apart: its point out alone
            const float t = std::atan2(c.y, c.x);
            add(c.x + std::cos(t) * c.r, c.y + std::sin(t) * c.r, std::cos(t), std::sin(t));
            continue;
        }
        for (int k = 0; k < kArcSteps; k++) {
            const float a = a0 + (a1 - a0) * float(k) / kArcSteps;
            add(c.x + std::cos(a) * c.r, c.y + std::sin(a) * c.r, std::cos(a), std::sin(a));
        }
    }
    // the shadow's normals: the lobes' over about one lobe each side, which don't cross (the ellipse's did, by a wide
    // cloud's ends)
    smooth = p;
    const size_t m = p.size();
    for (size_t i = 0; i < m; i++) {
        float sx = 0, sy = 0;
        for (int k = -kArcSteps; k <= kArcSteps; k++) {
            const Point& q = p[(i + m + size_t(k + int(m))) % m];
            sx += q.nx, sy += q.ny;
        }
        const float l = std::hypot(sx, sy);
        if (l > 1e-6f) smooth[i].nx = sx / l, smooth[i].ny = sy / l;
    }
    return p;
}

// seconds, for the clouds' lobes moving back and forth: in floats, over a period long enough
float Clock(std::chrono::steady_clock::time_point at) {
    return float(std::fmod(std::chrono::duration<double>(at.time_since_epoch()).count(), 6283.1853));
}

// a simple polygon (clockwise on the frame, y down) cut into triangles, each clockwise, by their corners; none if it
// can't be (it crosses itself)
std::vector<uint32_t> EarClip(const std::vector<float>& x, const std::vector<float>& y) {
    const size_t n = x.size();
    std::vector<uint32_t> out;
    if (n < 3) return out;
    std::vector<uint32_t> ring(n);
    for (size_t i = 0; i < n; i++) ring[i] = uint32_t(i);
    // clockwise on the frame with y down is a positive sum of x0 y1 - x1 y0
    auto cross = [&](uint32_t o, uint32_t a, uint32_t b) {
        return (x[a] - x[o]) * (y[b] - y[o]) - (y[a] - y[o]) * (x[b] - x[o]);
    };
    double area = 0;
    for (size_t i = 0; i < n; i++) area += double(x[i]) * y[(i + 1) % n] - double(x[(i + 1) % n]) * y[i];
    const float turn = area > 0 ? 1.0f : -1.0f;
    size_t k = 0, missed = 0;  // corners tried in a row that weren't ears
    while (ring.size() > 3) {
        const size_t m = ring.size();
        if (missed >= m) return {};  // went round with no ear
        const uint32_t a = ring[(k + m - 1) % m], b = ring[k % m], c = ring[(k + 1) % m];
        bool ear = cross(a, b, c) * turn > 0;
        for (size_t j = 0; ear && j < m; j++) {
            const uint32_t q = ring[j];
            if (q == a || q == b || q == c) continue;
            if (cross(a, b, q) * turn >= 0 && cross(b, c, q) * turn >= 0 && cross(c, a, q) * turn >= 0) ear = false;
        }
        if (!ear) {
            k = (k + 1) % m, missed++;
            continue;
        }
        missed = 0;
        if (turn > 0) out.insert(out.end(), {a, b, c});
        else out.insert(out.end(), {a, c, b});
        ring.erase(ring.begin() + std::ptrdiff_t(k % m));
        k = k % ring.size();
    }
    if (ring.size() != 3) return {};
    if (turn > 0) out.insert(out.end(), {ring[0], ring[1], ring[2]});
    else out.insert(out.end(), {ring[0], ring[2], ring[1]});
    return out;
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
    const float game_scale = std::clamp(F(g_obj + kScale), 0.0f, 1.0f);
    if (!(game_scale > 0.01f)) return;
    const float full = bh / game_scale;  // the body's height at full scale
    // The scale drawn: the game's moves a third a tick ([obj+0x54], 0xE0CD0), in steps, so it runs a curve of its own
    // (smooth at both ends) over kTurnTime, about the time the game's takes, from when that starts to grow or fall
    constexpr float kTurnTime = 0.1f, kTurnClose = 0.09f;  // closing a little sooner: the game stops drawing it at 0
    {
        const auto at = std::chrono::steady_clock::now();
        const bool start = bub.turn_at == std::chrono::steady_clock::time_point{} ||
                           std::chrono::duration<float>(at - bub.seen_at).count() > 0.5f;
        if (start) {
            bub.turn = game_scale >= 0.999f ? 0 : 1, bub.turn_at = at;  // 0 open, 1 opening, -1 closing
        } else if (game_scale > bub.game_last + 1e-4f && bub.turn != 1) {
            bub.turn = 1, bub.turn_at = at;
        } else if (game_scale < bub.game_last - 1e-4f && bub.turn != -1) {
            bub.turn = -1, bub.turn_at = at;
        }
        bub.game_last = game_scale, bub.seen_at = at;
        const float t = std::chrono::duration<float>(at - bub.turn_at).count();
        auto curve = [](float u) {
            u = std::clamp(u, 0.0f, 1.0f);
            return u * u * (3 - 2 * u);
        };
        bub.scale = bub.turn == 1 ? curve(t / kTurnTime) : bub.turn == -1 ? 1 - curve(t / kTurnClose) : 1.0f;
    }
    if (!(bub.scale > 0.01f)) return;
    const float scale = bub.scale;
    // the bubble fades in and out with the scale drawn, always at its full size, its tail or trail out throughout
    const auto now = std::chrono::steady_clock::now();
    const float shown = scale;
    // the ellipse about the words' box and a margin, its half axes kSnug times the box's half sides (the least ellipse
    // around a box has them root 2 times: a little less, as a page's last line is short), no taller than the body;
    // before the words have drawn, their size as measured unseen (MeasureWords), else the body narrowed
    constexpr float kMargin = 8.0f, kSnug = 1.35f;
    float width = bw / scale, height = full;  // the display's pixels (the frame's x times wider)
    // a scrolling page at least as big as all its lines (a wide line or a tall icon scrolling in grew it midway)
    const Box& words = !bub.words.Empty() ? bub.words : bub.unseen;
    if (!words.Empty() || !bub.whole.Empty()) {
        float ww = words.Empty() ? 0.0f : words.x1 - words.x0, wh = words.Empty() ? 0.0f : words.y1 - words.y0;
        if (!bub.whole.Empty()) ww = std::max(ww, bub.whole.x1 - bub.whole.x0), wh = std::max(wh, bub.whole.y1);
        width = 2 * kSnug * (ww * wider * 0.5f + kMargin);
        height = std::min(2 * kSnug * (wh * 0.5f + kMargin), full);
    }
    width = std::max(width, height);
    // the tail's tip: toward its speaker from the turned tail's middle, as far as half its sprite's height, or the
    // bubble's anchor (the game sets its body off from it at an angle, its tail's corner on it)
    float tip_x = 0, tip_y = 0;
    // The game's tail sprite is a slanted triangle: its base along the box's top, its tip at the bottom corner on the
    // right, on the left mirrored (flip 1), upside down with flip 2. 0xE1880 mirrors it toward the side its speaker is
    // on, and a turned one (0x356790) it turns about its middle by -atan(dx / dy) of the way to its speaker
    // ([obj+0xB4], [obj+0xB8]), which takes the sprite's down to that way: its tip leans past the way, as the game's
    // shows. Its base's middle and its tip on the frame, which the tail takes after
    bool game_tail = false;
    float game_base_x = 0, game_base_y = 0;
    if (g_tail && !g_thought) {
        const float s = (g_tail_flips & 1) ? -1.0f : 1.0f, u = (g_tail_flips & 2) ? -1.0f : 1.0f;
        float bx0 = 0, by0 = -u * g_tail_h * 0.5f, tx0 = s * g_tail_w * 0.5f, ty0 = u * g_tail_h * 0.5f;
        float mx = g_tail_x0 + g_tail_w * 0.5f, my = g_tail_y0 + g_tail_h * 0.5f;
        if (g_tail_turned) {
            mx = g_tail_x0, my = g_tail_y0;  // 0x356950 keeps its middle in the box's corner
            const float dx = float(int32_t(R32(g_obj + kToward))), dy = float(int32_t(R32(g_obj + kToward + 4)));
            const float l = std::hypot(dx, dy);
            const float c = l > 1e-3f ? dy / l : 1.0f, sn = l > 1e-3f ? -dx / l : 0.0f;
            auto turn = [&](float& x, float& y) {
                const float rx = x * c - y * sn, ry = x * sn + y * c;
                x = rx, y = ry;
            };
            turn(bx0, by0), turn(tx0, ty0);
        }
        game_tail = std::isfinite(mx + my + bx0 + by0 + tx0 + ty0);
        game_base_x = mx + bx0, game_base_y = my + by0, tip_x = mx + tx0, tip_y = my + ty0;
    }
    if (!game_tail && (g_tail || g_thought)) {
        tip_x = F(g_obj + kAnchor), tip_y = F(g_obj + kAnchor + 4);
    }
    // its place and tip as the game's are, which jump where the game's do (a camera cut in a cutscene); its size eased
    // by the time gone, most of the way in a tenth of a second (a page's words of another width), but a bubble just
    // opening (as each line's does) or not drawn for a while starts at its size
    const float dt = std::chrono::duration<float>(now - bub.at).count();
    const bool fresh = bub.at == std::chrono::steady_clock::time_point{} || dt > 0.5f || shown < 0.15f;
    const float k = fresh ? 1.0f : 1.0f - std::exp(-std::max(dt, 0.0f) / 0.05f);
    bub.at = now;
    auto ease = [&](float& was, float to) { was += (to - was) * k; };
    ease(bub.width, width), ease(bub.tall, height);
    bub.x = bub.raw_x, bub.y = bub.raw_y;
    // The game sets its body off from the anchor ([obj+0x70] from [obj+0x68], 0xE1880) by as much times its scale, so
    // as it opens and closes the body comes out of the anchor and goes back into it, toward the tail. The ellipse
    // fades in and out at its middle at full scale instead
    if (game_scale < 0.999f) {
        const float ax = F(g_obj + kAnchor), ay = F(g_obj + kAnchor + 4);
        bub.x = ax + (bub.raw_x - ax) / game_scale, bub.y = ay + (bub.raw_y - ay) / game_scale;
    }
    bub.tip_x = tip_x, bub.tip_y = tip_y;
    bub.tip = g_tail || g_thought;
    const float cx = bub.x, cy = bub.y;
    const float hx = std::max(bub.width * 0.5f, 1.0f), hy = std::max(bub.tall * 0.5f, 1.0f);
    // the soft edge: a pixel as drawn (the frame's pixels times the render scale down), half in and half out
    const app::Scale render = app::RenderScale(640, 480);
    const float soft = render.y > 0.1f && std::isfinite(render.y) ? 1.0f / render.y : 1.0f;
    // The tail, in the frame's pixels (the display's proportions) whatever the ellipse's size: kTailLong from the
    // middle of its base to its tip, its base kTailBase along the ellipse each side of that middle. Its base's middle
    // is on the ellipse's bottom, a kTailArc share of its perimeter about its lowest point: there, the point nearest
    // the bubble's anchor. From it the tail aims at the anchor, no more than kSlant off square to the ellipse
    constexpr float kTailBase = 7.0f, kTailLong = 23.0f, kTailArc = 1.0f / 3, kSlant = 0.9f;
    constexpr float kTurn = 6.2831853f, kBottom = kTurn / 4;  // the ellipse's angle at its lowest point (y down)
    float tx = (bub.tip_x - cx) * wider, ty = bub.tip_y - cy;
    float t0 = kBottom;
    const float length = kTailLong;
    const bool tail = g_tail && !g_thought;
    float ex = 0, ey = hy, aim = kBottom;  // where the tail or trail leaves the ellipse, and its way
    if (tail || g_thought) {
        // the bottom arc's ends, a sixth of the perimeter (Ramanujan's) each side of the lowest point
        const float h = (hx - hy) * (hx - hy) / ((hx + hy) * (hx + hy));
        const float perimeter = kTurn / 2 * (hx + hy) * (1 + 3 * h / (10 + std::sqrt(4 - 3 * h)));
        const float left = Along(hx, hy, kBottom, 1, perimeter * kTailArc / 2);   // toward -x (angle up from 90)
        const float right = Along(hx, hy, kBottom, -1, perimeter * kTailArc / 2);  // toward +x
        // A speech tail's base: where the line from the middle toward the game's tail's base leaves the ellipse; it
        // aims the way the game's leans, from its base to its tip. (Its base nearest the tip, it stood square to the
        // ellipse there whatever way the game's leaned)
        const float gbx = (game_base_x - cx) * wider, gby = game_base_y - cy;
        float base;
        if (tail && game_tail) base = std::hypot(gbx, gby) > 1e-3f ? std::atan2(gby / hy, gbx / hx) : kBottom;
        else base = Nearest(hx, hy, tx, ty);
        t0 = std::clamp(kBottom + std::remainder(base - kBottom, kTurn), right, left);
        ex = hx * std::cos(t0), ey = hy * std::sin(t0);
        const float normal = std::atan2(std::sin(t0) / hy, std::cos(t0) / hx);
        const float toward = tail && game_tail && std::hypot(tx - gbx, ty - gby) > 1e-3f
                                 ? std::atan2(ty - gby, tx - gbx)
                             : std::hypot(tx - ex, ty - ey) > 1e-3f ? std::atan2(ty - ey, tx - ex)
                                                                     : normal;
        aim = normal + std::clamp(std::remainder(toward - normal, kTurn), -kSlant, kSlant);
        tx = ex + std::cos(aim) * length, ty = ey + std::sin(aim) * length;
    }
    float gap_from = 0, gap_to = 0;
    std::vector<Point> smooth_normals;  // a cloud's along the ellipse's normals: its shadow's
    std::vector<Point> p;
    if (g_thought) {
        p = Cloud(hx, hy, CloudLobe(hy), CloudOut(hy), kCloudFewest, g_obj ^ bub.page, Clock(now), smooth_normals);
    } else {
        p = Outline(hx, hy, tail, tx, ty, t0, kTailBase, &gap_from, &gap_to);
        Normals(p);
    }

    // colours: the speaker's (white without) and the sprite's alpha
    const bool coloured = B(kColoured) != 0;
    const float fr = coloured ? B(kRed) / 255.0f : 1, fg = coloured ? B(kGreen) / 255.0f : 1,
                fb = coloured ? B(kBlue) / 255.0f : 1;
    const float alpha = B(rec + kSpriteAlpha) / 255.0f * shown;  // faded in and out
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
    // Lit from the upper left (y down), so it reads raised off the picture: a short shadow cast down and to the right,
    // as if the bubble stood a little off it, and a rim shaded inside its edge, light toward the light and dark away
    // from it (Rim). The shadow outside the bubble only (it is see-through): from its edge out by blur, moved by lift
    // away from the light, kShadow dark, falling off fast; where the edge faces the light little of it shows
    constexpr float kLightX = -0.55f, kLightY = -0.835f;
    constexpr float kShadow = 0.5f;
    const float blur = std::clamp(hy * 0.18f, 4.0f, 10.0f), lift = std::clamp(hy * 0.12f, 3.0f, 6.0f);
    const float lift_x = -kLightX * lift, lift_y = -kLightY * lift;
    auto shade = [&](float f) { return Colour(0, 0, 0, alpha * kShadow * f); };
    const uint32_t shade2 = Colour(0, 0, 0, 0);
    auto cast = [&](float nx, float ny) {  // how much of the shadow shows past an edge of normal (nx, ny)
        const float l = std::hypot(nx, ny);
        const float d = l > 1e-6f ? -(nx * kLightX + ny * kLightY) / l : 0.0f;  // 1 facing away from the light
        const float s = std::clamp((d + 0.6f) / 1.6f, 0.0f, 1.0f);
        return s * s * (3 - 2 * s);
    };
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
    auto faded = [](uint32_t c, float f) { return (c & 0x00FFFFFFu) | uint32_t(float(c >> 24) * f + 0.5f) << 24; };
    // a shadow's band from the edge a..b (unit normals, moved by ox, oy) out by spread and lift times share, fa and fb
    // of it dark at the edge
    auto shadow_band = [&](const Point& a, const Point& b, float ox, float oy, float fa, float fb, float spread,
                           float share) {
        const float sx = lift_x * share, sy = lift_y * share;
        const float axm = a.x + a.nx * spread * 0.4f + sx * 0.6f, aym = a.y + a.ny * spread * 0.4f + sy * 0.6f;
        const float bxm = b.x + b.nx * spread * 0.4f + sx * 0.6f, bym = b.y + b.ny * spread * 0.4f + sy * 0.6f;
        const float ax1 = a.x + a.nx * spread + sx, ay1 = a.y + a.ny * spread + sy;
        const float bx1 = b.x + b.nx * spread + sx, by1 = b.y + b.ny * spread + sy;
        band(ox + a.x, oy + a.y, ox + b.x, oy + b.y, shade(fa), shade(fb), ox + axm, oy + aym, ox + bxm, oy + bym,
             shade(fa * 0.3f), shade(fb * 0.3f));
        band(ox + axm, oy + aym, ox + bxm, oy + bym, shade(fa * 0.3f), shade(fb * 0.3f), ox + ax1, oy + ay1, ox + bx1,
             oy + by1, shade2, shade2);
    };
    // The rim: from the edge (moved by ox, oy) in by width, toward the light lighter (white over a coloured bubble),
    // away from it darker, and a little darker all round, so it bulges; falling off fast inward, out over the soft edge
    // with it. A point's inset along its unit normal at most cap (the tail's tip, its normal a long miter)
    constexpr float kRimDark = 0.22f, kRimLight = 0.5f;
    auto rim_colour = [&](float nx, float ny) {
        const float l = std::hypot(nx, ny);
        const float d = l > 1e-6f ? (nx * kLightX + ny * kLightY) / l : 0.0f;  // 1 facing the light
        const float k = kRimDark * (1 - d) * 0.5f * alpha, h = kRimLight * std::max(d, 0.0f) * alpha;
        const float a = h + k - h * k;  // white over the bubble, then black over that
        const float w = a > 1e-6f ? h * (1 - k) / a : 0.0f;
        return Colour(w, w, w, a);
    };
    auto rim = [&](const std::vector<Point>& q, float ox, float oy, float width, float cap) {
        const size_t m = q.size();
        // a point's rim width: width along its (miter) normal, at most cap far; its inner lines share it alike
        auto inset = [&](const Point& r) {
            const float l = std::hypot(r.nx, r.ny);
            return l > 1e-6f && width * l > cap ? cap / l : width;
        };
        auto in = [&](const Point& r, float s, float& x, float& y) {
            x = ox + r.x - r.nx * s, y = oy + r.y - r.ny * s;
        };
        // The tail's tip takes none of the rim: its sides' bands meet down its middle whatever their width (it is
        // narrower than the rim near the tip), and the side toward the light drew a whitish cone there
        auto colour = [&](const Point& r) {
            const uint32_t c = rim_colour(r.nx, r.ny);
            return r.tip ? c & 0x00FFFFFFu : c;
        };
        for (size_t i = 0; i < m; i++) {
            const Point& a = q[i];
            const Point& b = q[(i + 1) % m];
            const uint32_t ca = colour(a), cb = colour(b);
            const float wa = inset(a), wb = inset(b);
            float ax0, ay0, bx0, by0, axm, aym, bxm, bym, axe, aye, bxe, bye, axo, ayo, bxo, byo;
            in(a, wa, ax0, ay0), in(b, wb, bx0, by0);
            in(a, wa * 0.35f, axm, aym), in(b, wb * 0.35f, bxm, bym);
            in(a, soft * 0.5f, axe, aye), in(b, soft * 0.5f, bxe, bye);
            in(a, -soft * 0.5f, axo, ayo), in(b, -soft * 0.5f, bxo, byo);
            band(ax0, ay0, bx0, by0, ca & 0x00FFFFFFu, cb & 0x00FFFFFFu, axm, aym, bxm, bym, faded(ca, 0.4f),
                 faded(cb, 0.4f));
            band(axm, aym, bxm, bym, faded(ca, 0.4f), faded(cb, 0.4f), axe, aye, bxe, bye, ca, cb);
            band(axe, aye, bxe, bye, ca, cb, axo, ayo, bxo, byo, ca & 0x00FFFFFFu, cb & 0x00FFFFFFu);
        }
    };
    // the shadow first, under the bubble's soft edge: the ellipse's alone (the tail casts none: its sides' came out a
    // thin skewed sliver)
    // It fades out under the tail, which is see-through too: over the shadow it came out darker than the bubble. Gone
    // within its base's arc, back over as far again each side
    // (a thought's, the cloud cast along the ellipse's normals: by its own, sharp in its dips, it threw spikes out)
    const std::vector<Point> e = g_thought ? smooth_normals : Outline(hx, hy, false, 0, 0, 0, 0);
    const float under = tail ? (gap_to - gap_from) * 0.5f : 0.0f;
    auto clear_of_tail = [&](size_t i) {
        if (!tail) return 1.0f;
        const float t = kTurn * float(i % e.size()) / float(e.size());  // the ellipse's points by angle
        const float d = std::fabs(std::remainder(t - t0, kTurn));
        const float f = std::clamp((d - under) / std::max(under, 1e-3f), 0.0f, 1.0f);
        return f * f * (3 - 2 * f);
    };
    for (size_t i = 0; i < e.size(); i++) {
        const Point& a = e[i];
        const Point& b = e[(i + 1) % e.size()];
        shadow_band(a, b, 0, 0, clear_of_tail(i) * cast(a.nx, a.ny), clear_of_tail(i + 1) * cast(b.nx, b.ny), blur,
                    1.0f);
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
    // a thought's cloud isn't one the middle sees all of (along a wide one's long sides each lobe's near side faces
    // away from it): its fan folded over itself; it is cut into triangles instead
    std::vector<uint32_t> cut;
    if (g_thought) {
        cut = EarClip(fx, fy);
        if (cut.empty()) {  // the soft edge's inner line crossed itself at a dip: the edge's own
            std::vector<float> ox, oy;
            for (const Point& q : p) ox.push_back(q.x), oy.push_back(q.y);
            cut = EarClip(ox, oy);
            fx = ox, fy = oy;
        }
    }
    if (!cut.empty()) {
        for (size_t i = 0; i + 2 < cut.size(); i += 3)  // clockwise on the frame, as the culling keeps
            for (size_t k = 0; k < 3; k++) {
                const uint32_t q = cut[i + k];
                v.push_back(at(fx[q], fy[q], fill(fx[q], fy[q])));
            }
    } else {
        fan(0, 0, fill(0, 0), ellipse, true, false);
    }
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
    // the rim over the fill (a cloud's lobes each their own, the dips between them creased)
    rim(p, 0, 0, std::clamp(hy * 0.3f, 4.0f, 12.0f), tail ? length * 0.4f : 1e30f);
    // a thought's trail: kTrail small clouds of the frame's pixels (their lobes' outer edge about that far from their
    // middle) from where it leaves the cloud toward the anchor, a gap between, a soft shadow below each
    if (g_thought) {
        constexpr float kTrail[] = {22.0f, 13.0f}, kGap = -2.0f;  // overlapping a little
        constexpr float kFirstIn = 4.0f;  // the first pulled this much further into the cloud
        constexpr float kTrailLobe = 0.55f;  // their lobes' radius, of theirs
        constexpr int kTrailFewest = 5;
        constexpr float kSwell = 0.08f, kSwellRate = 3.1f;  // each grows and shrinks by this share as well, rad/s
        // from past the cloud's edge: a lobe of its average size, from its middle inside the ellipse
        float along = kGap - kFirstIn + std::max(CloudLobe(hy) - (CloudLobe(hy) - CloudOut(hy)), 0.0f);
        const uint32_t c = fill(0, 0), cc = c & 0x00FFFFFFu;
        for (size_t k = 0; k < std::size(kTrail); k++) {
            const float r0 = kTrail[k];
            const float ox = ex + std::cos(aim) * (along + r0), oy = ey + std::sin(aim) * (along + r0);
            along += 2 * r0 + kGap;
            if (r0 < soft) continue;
            // its size swells and shrinks about its place, each out of step with the other
            const float r = kTrail[k] * (1 + kSwell * std::sin(Clock(now) * kSwellRate + 2.1f * float(k + 1)));
            // a cloud's edge is its inner ellipse plus a lobe: that ellipse r less a lobe out, so the edge is r out
            const float lobe = r * kTrailLobe, out = lobe * 0.35f;
            std::vector<Point> sm;
            const std::vector<Point> q = Cloud(r - out, r - out, lobe, out, kTrailFewest,
                                               (g_obj ^ bub.page) * 0x9E3779B1u + uint32_t(k + 1), Clock(now), sm);
            const size_t m = q.size();
            // the shadow's along the smoothed normals, shorter and closer (it stands less high)
            for (size_t i = 0; i < m; i++) {
                const Point& a = sm[i];
                const Point& b = sm[(i + 1) % m];
                shadow_band(a, b, ox, oy, cast(a.nx, a.ny), cast(b.nx, b.ny), blur * 0.6f, 0.6f);
            }
            // the fill fanned from its middle (inside every lobe, so it sees all the edge), then the soft edge
            for (size_t i = 0; i < m; i++) {
                const Point& a = q[i];
                const Point& b = q[(i + 1) % m];
                float aix, aiy, bix, biy, aox, aoy, box, boy;
                off(a, -soft * 0.5f, aix, aiy), off(b, -soft * 0.5f, bix, biy);
                off(a, soft * 0.5f, aox, aoy), off(b, soft * 0.5f, box, boy);
                v.push_back(at(ox, oy, c)), v.push_back(at(ox + aix, oy + aiy, c));
                v.push_back(at(ox + bix, oy + biy, c));
                band(ox + aix, oy + aiy, ox + bix, oy + biy, c, c, ox + aox, oy + aoy, ox + box, oy + boy, cc, cc);
            }
            rim(q, ox, oy, std::max(r * 0.35f, 2.0f), 1e30f);
        }
    }
    if (v.size() > kCapacity) {
        static bool told = false;
        if (!std::exchange(told, true)) LOGR("bubbles: %zu vertices, room for %u: not drawn", v.size(), kCapacity);
        return;
    }
    if (!g_vertices) g_vertices = mem::AllocSystem(kCapacity * sizeof(Vertex));
    if (!g_vertices) return;
    std::memcpy(mem::Base() + g_vertices, v.data(), v.size() * sizeof(Vertex));
    // colour and alpha from the vertices, times the texture's middle: the speech bubble's body's (a thought's body's
    // texture is see-through there), the texture 0x356430 binds for [0x69E490] (each kind's record: +4 its kind,
    // +0x1C the texture, or where it is for kind 4)
    if (g_thought) {
        const uint32_t record = R32(kSpeechTexture);
        if (record) {
            uint32_t texture = R32(record + 0x1C);
            if (R32(record + 4) == 4) texture = texture ? R32(texture) : 0;
            const uint32_t eax = ctx.eax;
            ctx.eax = 0;  // stage 0
            if (texture) kernel::CallGuest(ctx, kSetTexture, {texture});
            ctx.eax = eax;
        }
    }
    kernel::W32(kColorOp, kModulate), kernel::W32(kColorArg1, kDiffuse), kernel::W32(kColorArg2, kTexture);
    kernel::W32(kAlphaOp, kModulate), kernel::W32(kAlphaArg1, kDiffuse), kernel::W32(kAlphaArg2, kTexture);
    kernel::W32(kDirty, R32(kDirty) | kDirtyStages);
    const X86Context saved = ctx;
    kernel::CallGuest(ctx, kDrawVerticesUP, {kTriangleList, uint32_t(v.size()), g_vertices, uint32_t(sizeof(Vertex))});
    ctx = saved;
    if (Trace()) {
        static uint32_t traced = 0;
        if (g_frame - traced >= 30) {
            traced = g_frame;
            LOGR("bubbles: %08X body %.1f %.1f %.1f x %.1f, words %.1f..%.1f %.1f..%.1f, "
                 "ellipse at %.1f %.1f %.1f x %.1f, tip %.1f %.1f, tail %d%s %.1f %.1f %.1f x %.1f, "
                 "colour %d %02X%02X%02X alpha %02X, wider %.3f, state %02X, thought %d, flips %d, toward %d %d",
                 g_obj, bx, by, bw, bh, bub.words.x0, bub.words.x1, bub.words.y0, bub.words.y1, cx, cy, bub.width,
                 bub.tall, bub.tip_x, bub.tip_y, int(g_tail), g_tail_turned ? " turned" : "", g_tail_x0, g_tail_y0,
                 g_tail_w, g_tail_h, int(coloured), B(kRed), B(kGreen), B(kBlue), B(rec + kSpriteAlpha),
                 double(wider), B(g_obj + kState), int(g_thought), int(g_tail_flips),
                 int(int32_t(R32(g_obj + kToward))), int(int32_t(R32(g_obj + kToward + 4))));
        }
    }
}

// The bubble opens before it shows its words (0xE2940 draws them only in some of its states, [obj+0x35]), and sized
// by the body until they had drawn it opened narrow and then widened. Until they have, they are drawn see-through
// (alpha 0) before the body each frame, in a state that shows them, for their box (ConkerTextQuad): a page of more
// than four lines skips its faint lines, so its box grows as it would have. Before 0xE1880 runs: the words' drawing
// takes the texture off (0x1F31B0), and in the body's draw it left the body and the ellipse drawn without the
// bubble's texture, opaque white
//
// A page of more than four lines scrolls, its lines faded in and out of a window of four, so only the shown ones were
// measured and the bubble grew midway when a wider line or a taller icon came in. Once a page it is swept: drawn
// unseen at steps through its scroll, every line passing the window, with its alpha (the faint lines skip by it) but
// the lines' draws given alpha 0. 0xE2940 times the scroll by its descriptor ([obj+0x30]): +0x2050 the time gone
// (each call adds the clock since +0x2054, the clock then) over +0x2044 times [0x64922C] / [0x648D2C], or in a
// voiced cutscene ([0x76EC82] set, [0x8472A0] 13) the voice's place ([0x76F0F8 + 4 * +0x2038]) past +0x2048 over
// +0x2044; all four put back after
void SweepWords(X86Context& ctx, Bubble& b) {
    b.swept = true;
    const uint32_t desc = R32(g_obj + 0x30);
    const uint32_t lines = R32(kLineCount);  // as the measure just now left it
    if (!desc || lines < 5 || lines > 64) return;
    const uint32_t duration = R32(desc + 0x2044);
    if (!duration) return;
    const uint32_t gone = R32(desc + 0x2050), clock = R32(desc + 0x2054), start = R32(desc + 0x2048);
    const bool voiced = B(0x76EC82) != 0 && R32(0x8472A0) == 13;
    const uint32_t voice = voiced ? R32(0x76F0F8 + 4 * R32(desc + 0x2038)) : 0;
    const float per = F(0x64922C) / F(0x648D2C);  // time gone a whole scroll takes, per +0x2044
    if (!voiced && !(std::isfinite(per) && per > 0)) return;
    Box whole;
    const uint32_t steps = 2 * lines;
    for (uint32_t i = 0; i <= steps; i++) {
        const float at = float(i) / float(steps);
        if (voiced) kernel::W32(desc + 0x2048, voice - uint32_t(int32_t(at * float(int32_t(duration)))));
        else kernel::W32(desc + 0x2050, uint32_t(int32_t(at * float(int32_t(duration)) * per)));
        b.unseen = {};
        g_unseen = g_sweeping = true;
        kernel::CallGuest(ctx, kWords, {0, 0, 0, 255});
        g_unseen = g_sweeping = false;
        if (b.unseen.Empty()) continue;
        whole.x0 = std::min(whole.x0, b.unseen.x0), whole.x1 = std::max(whole.x1, b.unseen.x1);
        whole.y0 = 0, whole.y1 = std::max(whole.y1, b.unseen.y1 - b.unseen.y0);
    }
    kernel::W32(desc + 0x2050, gone), kernel::W32(desc + 0x2054, clock), kernel::W32(desc + 0x2048, start);
    b.unseen = {};
    b.whole = whole;
    if (Trace())
        LOGR("bubbles: %08X swept %u lines: %.1f x %.1f", g_obj, lines, double(whole.x1 - whole.x0),
             double(whole.y1));
}

void MeasureWords(X86Context& ctx, uint8_t* base) {
    if (!g_obj) return;
    Bubble& b = g_bubbles[g_obj];
    if (!b.words.Empty() || !R32(g_obj + kPage)) return;
    const uint8_t state = base[g_obj + kState];
    base[g_obj + kState] = kShowing;
    const uint32_t eax = ctx.eax;
    ctx.eax = g_obj;
    kernel::W32(kLineCount, 0);  // left as it is if the words return before counting
    g_unseen = true;
    kernel::CallGuest(ctx, kWords, {0, 0, 0, 0});
    g_unseen = false;
    if (!b.swept) {
        const Box unseen = b.unseen;
        SweepWords(ctx, b);
        b.unseen = unseen;
    }
    ctx.eax = eax;
    base[g_obj + kState] = state;
}

// 0x1F31B0 and 0x1F3340, a line of words drawn (cdecl, its alpha the sixth argument): alpha 0 while swept
X86_EXTERN_FUNC(__imp__sub_001F31B0);
X86_FUNC(sub_001F31B0) {
    if (g_sweeping) kernel::W32(ctx.esp + 0x18, 0);
    __imp__sub_001F31B0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_001F3340);
X86_FUNC(sub_001F3340) {
    if (g_sweeping) kernel::W32(ctx.esp + 0x18, 0);
    __imp__sub_001F3340(ctx, base);
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

bool subtitles::g_redraw = true;

// [[hook]] 0xE1880 (subtitles.cpp) calls this around the bubble's drawing: obj the bubble, 0 at the end
void subtitles::BubbleDrawing(uint32_t obj) {
    if (obj) {
        g_obj = 0;
        g_tail = false;
        const uint32_t desc = R32(obj + 0x30);
        if (!desc || !subtitles::g_redraw) return;
        g_obj = obj;
        g_thought = (R32(desc) & kThought) != 0;
        Bubble& b = g_bubbles[obj];
        const uint32_t page = R32(obj + kPage);
        if (b.page != page) {
            b.page = page;
            b.words = {}, b.unseen = {}, b.whole = {}, b.swept = false;
        }
        b.seen = ++g_frame;
        return;
    }
    g_obj = 0;
    g_tail = false;
    g_thought = false;
    // the bubbles not drawn for a while go
    if (g_bubbles.size() > 16)
        for (auto it = g_bubbles.begin(); it != g_bubbles.end();)
            it = g_frame - it->second.seen > 120 ? g_bubbles.erase(it) : std::next(it);
}

// [[hook]] 0xE1880 (subtitles.cpp) calls this around a key-paced bubble's draw: obj the bubble, 0 at the end
void subtitles::BubblePlacing(uint32_t obj) {
    g_obj = obj;
    g_place = obj != 0;
    g_placed = false;
    g_tail = false;
    const uint32_t desc = obj ? R32(obj + 0x30) : 0;
    g_thought = desc && (R32(desc) & kThought) != 0;
}

bool subtitles::BubblePlaced(float& x, float& y, float& w, float& h) {
    if (!g_placed) return false;
    x = g_placed_x, y = g_placed_y, w = g_placed_w, h = g_placed_h;
    return true;
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
    const bool circle = g_thought && (from == kThoughtCall1 || from == kThoughtCall2 || from == kTailCall);
    if (!g_obj || (!circle && from != kTailCall && from != kBodyCall)) return __imp__sub_00356750(ctx, base);
    const uint32_t rec = ctx.eax;
    const uint8_t flips = base[rec + kSpriteFlips];
    SeeThrough<__imp__sub_00356750>(ctx, base);
    if (circle) return;
    if (from == kTailCall) {
        g_tail = true, g_tail_turned = false, g_tail_flips = flips;
        g_tail_x0 = F(kBoxLeft), g_tail_y0 = F(kBoxTop), g_tail_w = F(kBoxWidth), g_tail_h = F(kBoxHeight);
        return;
    }
    if (g_place) {
        // the body at full scale: the game scales its box by [obj+0x50] and sets it off from the anchor by as much
        const float bx = F(kBoxLeft), by = F(kBoxTop), bw = F(kBoxWidth), bh = F(kBoxHeight);
        const float scale = std::clamp(F(g_obj + kScale), 0.0f, 1.0f);
        if (!(bw > 1 && bh > 1) || !std::isfinite(bx + by + bw + bh) || !(scale > 0.01f)) return;
        const float ax = F(g_obj + kAnchor), ay = F(g_obj + kAnchor + 4);
        g_placed_x = ax + (bx + bw * 0.5f - ax) / scale, g_placed_y = ay + (by + bh * 0.5f - ay) / scale;
        g_placed_w = bw / scale, g_placed_h = bh / scale;
        g_placed = std::isfinite(g_placed_x + g_placed_y);
        return;
    }
    DrawBubble(ctx, rec);
}

X86_EXTERN_FUNC(__imp__sub_00356790);
X86_FUNC(sub_00356790) {
    const uint32_t from = R32(ctx.esp);
    if (!g_obj || (from != kTailTurnedCall1 && from != kTailTurnedCall2)) return __imp__sub_00356790(ctx, base);
    g_tail_flips = base[ctx.eax + kSpriteFlips];
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
    if (g_place) return kernel::Return(ctx, ctx.eax, 0);  // cdecl
    Bubble& b = g_bubbles[obj];
    // the words' middle as the game would draw them, from the body's, to the ellipse's
    const float mx = b.words.Empty() || g_unseen ? 0.0f : b.x - (b.raw_x + (b.words.x0 + b.words.x1) * 0.5f);
    const float my = g_unseen ? 0.0f : b.y - b.raw_y;
    const float x = F(obj + kTextX), y = F(obj + kTextY);
    PutF(obj + kTextX, x + mx), PutF(obj + kTextY, y + my);
    g_words = true;
    g_words_now = {};
    __imp__sub_000E2940(ctx, base);
    g_words = false;
    PutF(obj + kTextX, x), PutF(obj + kTextY, y);
    if (g_words_now.Empty()) return;
    if (g_unseen) {
        b.unseen = g_words_now;
        return;
    }
    b.words.Add(g_words_now.x0 - mx - b.raw_x, g_words_now.y0 - my - b.raw_y, g_words_now.x1 - mx - b.raw_x,
                g_words_now.y1 - my - b.raw_y);
}

// [[hook]] 0xE1880 (subtitles.cpp) calls this after BubbleDrawing(obj), before the bubble is drawn
void subtitles::BubbleMeasure(X86Context& ctx) { MeasureWords(ctx, mem::Base()); }
