#include "gpu/vertex.h"

#include <algorithm>
#include <cstring>

#include <dxgiformat.h>

#include "core/log.h"
#include "kernel/kernel.h"

namespace gpu {

namespace {

// D3DVSDT: low nibble the kind, high nibble the component count
void Convert(uint32_t type, const uint8_t* p, float* o) {
    o[0] = 0;
    o[1] = 0;
    o[2] = 0;
    o[3] = 1;
    uint32_t kind = type & 15, n = std::min<uint32_t>((type >> 4) & 15, 4);
    if (type == 0x72) {  // FLOAT2H
        float v[3];
        std::memcpy(v, p, 12);
        o[0] = v[0];
        o[1] = v[1];
        o[3] = v[2];
        return;
    }
    switch (kind) {
        case 0:  // D3DCOLOR, bytes B G R A
            o[0] = p[2] / 255.0f;
            o[1] = p[1] / 255.0f;
            o[2] = p[0] / 255.0f;
            o[3] = p[3] / 255.0f;
            break;
        case 1:  // NORMSHORT
            for (uint32_t i = 0; i < n; i++) {
                int16_t s;
                std::memcpy(&s, p + i * 2, 2);
                o[i] = std::max(s / 32767.0f, -1.0f);
            }
            break;
        case 2:
            std::memcpy(o, p, n * 4);
            break;
        case 4:  // PBYTE
            for (uint32_t i = 0; i < n; i++) o[i] = p[i] / 255.0f;
            break;
        case 5:  // SHORT
            for (uint32_t i = 0; i < n; i++) {
                int16_t s;
                std::memcpy(&s, p + i * 2, 2);
                o[i] = s;
            }
            break;
        case 6: {  // NORMPACKED3, 11 11 10 signed
            uint32_t v;
            std::memcpy(&v, p, 4);
            int32_t x = int32_t(v << 21) >> 21, y = int32_t(v << 10) >> 21, z = int32_t(v) >> 22;
            o[0] = std::max(x / 1023.0f, -1.0f);
            o[1] = std::max(y / 1023.0f, -1.0f);
            o[2] = std::max(z / 511.0f, -1.0f);
            break;
        }
        default:
            break;
    }
}

// A/B switch: 0 converts every vertex on the CPU, as before
#ifndef XBE_RAW_VERTICES
#define XBE_RAW_VERTICES 1
#endif
constexpr bool kRawVertices = XBE_RAW_VERTICES;

// D3DVSDT type as the input assembler reads it: DXGI format, what the vertex shader does after, bytes the format reads
struct HostElement {
    uint8_t format = 0, decode = kDecodeNone, bytes = 0;
};
HostElement HostFormat(uint32_t type) {
    switch (type) {
        case 0x12: return {DXGI_FORMAT_R32_FLOAT, kDecodeNone, 4};
        case 0x22: return {DXGI_FORMAT_R32G32_FLOAT, kDecodeNone, 8};
        case 0x32: return {DXGI_FORMAT_R32G32B32_FLOAT, kDecodeNone, 12};
        case 0x42: return {DXGI_FORMAT_R32G32B32A32_FLOAT, kDecodeNone, 16};
        case 0x72: return {DXGI_FORMAT_R32G32B32_FLOAT, kDecodeFloat2H, 12};
        case 0x40: return {DXGI_FORMAT_B8G8R8A8_UNORM, kDecodeNone, 4};  // D3DCOLOR, bytes B G R A
        case 0x11: return {DXGI_FORMAT_R16_SNORM, kDecodeNone, 2};
        case 0x21: return {DXGI_FORMAT_R16G16_SNORM, kDecodeNone, 4};
        case 0x31: return {DXGI_FORMAT_R16G16B16A16_SNORM, kDecodeW1, 8};
        case 0x41: return {DXGI_FORMAT_R16G16B16A16_SNORM, kDecodeNone, 8};
        case 0x14: return {DXGI_FORMAT_R8_UNORM, kDecodeNone, 1};
        case 0x24: return {DXGI_FORMAT_R8G8_UNORM, kDecodeNone, 2};
        case 0x34: return {DXGI_FORMAT_R8G8B8A8_UNORM, kDecodeW1, 4};
        case 0x44: return {DXGI_FORMAT_R8G8B8A8_UNORM, kDecodeNone, 4};
        case 0x15: return {DXGI_FORMAT_R16_SINT, kDecodeInt, 2};
        case 0x25: return {DXGI_FORMAT_R16G16_SINT, kDecodeInt, 4};
        case 0x35: return {DXGI_FORMAT_R16G16B16A16_SINT, kDecodeIntW1, 8};
        case 0x45: return {DXGI_FORMAT_R16G16B16A16_SINT, kDecodeInt, 8};
        case 0x16: return {DXGI_FORMAT_R32_UINT, kDecodePacked, 4};
        default: return {};
    }
}

// the index list for a primitive the input assembler has no topology for, or for converted vertices
template <typename F>
void ListIndices(uint32_t prim, uint32_t count, F local, std::vector<uint32_t>& ix, Topology& topology) {
    ix.reserve(size_t(count) * 3);
    topology = Topology::Triangles;
    switch (prim) {
        case 1:  // points
            topology = Topology::Points;
            for (uint32_t i = 0; i < count; i++) ix.push_back(local(i));
            break;
        case 2:  // line list
            topology = Topology::Lines;
            for (uint32_t i = 0; i + 1 < count; i += 2) ix.insert(ix.end(), {local(i), local(i + 1)});
            break;
        case 3:  // line loop
        case 4:  // line strip
            topology = Topology::Lines;
            for (uint32_t i = 0; i + 1 < count; i++) ix.insert(ix.end(), {local(i), local(i + 1)});
            if (prim == 3 && count > 2) ix.insert(ix.end(), {local(count - 1), local(0)});
            break;
        case 5:  // triangle list
            for (uint32_t i = 0; i + 2 < count; i += 3) ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2)});
            break;
        case 6:  // triangle strip
            for (uint32_t i = 0; i + 2 < count; i++) {
                if (i & 1) ix.insert(ix.end(), {local(i + 1), local(i), local(i + 2)});
                else ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2)});
            }
            break;
        case 7:   // fan
        case 10:  // polygon
            for (uint32_t i = 1; i + 1 < count; i++) ix.insert(ix.end(), {local(0), local(i), local(i + 1)});
            break;
        case 8:  // quad list
            for (uint32_t i = 0; i + 3 < count; i += 4)
                ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2), local(i), local(i + 2), local(i + 3)});
            break;
        case 9:  // quad strip
            for (uint32_t i = 0; i + 3 < count; i += 2)
                ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2), local(i + 1), local(i + 3), local(i + 2)});
            break;
    }
}

uint32_t TypeBytes(uint32_t t) {
    if (t == 0x72) return 12;
    uint32_t kind = t & 15, n = (t >> 4) & 15;
    switch (kind) {
        case 0: return 4;
        case 1: case 5: return 2 * n;
        case 4: return n;
        case 6: return 4;
        default: return 4 * n;
    }
}

}  // namespace

void FvfInputs(uint32_t fvf, VertexShader& vs) {
    vs.inputs.fill({});
    uint32_t off = 0;
    auto add = [&](int reg, uint32_t type) {
        vs.inputs[size_t(reg)] = {0, off, type};
        off += TypeBytes(type);
    };
    switch (fvf & 0xE) {
        case 0x2: add(0, 0x32); break;
        case 0x4: add(0, 0x42); break;
        case 0x6: case 0x8: case 0xA: case 0xC: {
            add(0, 0x32);
            uint32_t weights = ((fvf & 0xE) - 0x4) / 2;
            add(1, 0x02 | (weights << 4));
            break;
        }
        default: break;
    }
    if (fvf & 0x10) add(2, 0x32);
    if (fvf & 0x20) add(6, 0x12);
    if (fvf & 0x40) add(3, 0x40);
    if (fvf & 0x80) add(4, 0x40);
    uint32_t tex = (fvf >> 8) & 15;
    static const uint32_t kTexType[4] = {0x22, 0x32, 0x42, 0x12};
    for (uint32_t i = 0; i < tex && i < 4; i++) add(int(9 + i), kTexType[(fvf >> (16 + i * 2)) & 3]);
    vs.stream_stride.fill(0);
    vs.stream_stride[0] = off;
}

bool Assemble(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* ip, Assembled& out) {
    using namespace kernel;
    out.vertices.clear();
    out.indices.clear();
    out.regs = 0;
    out.vertex_count = 0;
    out.topology = Topology::Triangles;
    out.strip = false;
    out.decode = 0;
    out.buffer_count = 0;
    out.index_src = nullptr;
    out.index16 = false;
    out.index_count = out.draw_count = 0;
    out.base_vertex = 0;
    if (!count || prim < 1 || prim > 10) return false;

    VertexShader fvf_layout;
    const VertexShader* layout = s.vs.get();
    if (!(s.vs_handle & 1)) {
        FvfInputs(s.vs_handle, fvf_layout);
        layout = &fvf_layout;
        out.pretransformed = (s.vs_handle & 0xE) == 0x4;
    } else {
        out.pretransformed = false;
    }
    if (!layout) return false;

    // the vertex range the draw reads
    uint32_t first = 0, last = count - 1;
    if (ip) {
        uint16_t lo16 = 0xFFFF, hi16 = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t v;
            std::memcpy(&v, ip + i * 2, 2);
            lo16 = std::min(lo16, v);
            hi16 = std::max(hi16, v);
        }
        first = lo16;
        last = hi16;
    }
    // logical vertex numbers in draw order
    auto at = [&](uint32_t i) -> uint32_t {
        if (!ip) return start + i;
        uint16_t v;
        std::memcpy(&v, ip + i * 2, 2);
        return v + s.base_vertex;
    };
    uint32_t lo = ip ? first + s.base_vertex : start;
    uint32_t span = last - first + 1;

    int regs[16];
    int nregs = 0;
    for (int r = 0; r < 16; r++) {
        const auto& in = layout->inputs[size_t(r)];
        if (in.stream < 0 || !in.type || in.type == 0x02) continue;
        const auto& st = s.streams[size_t(in.stream)];
        if (!st.vb) continue;
        regs[nregs++] = r;
        out.regs |= uint16_t(1u << r);
    }
    if (!nregs) return false;
    auto stride_of = [&](const VertexShader::Input& in) {
        const auto& st = s.streams[size_t(in.stream)];
        return st.stride ? st.stride : layout->stream_stride[size_t(in.stream)];
    };

    // the guest's own vertices when the input assembler can read every element of them
    bool raw = kRawVertices;
    for (int k = 0; k < nregs && raw; k++) {
        const auto& in = layout->inputs[size_t(regs[k])];
        HostElement he = HostFormat(in.type);
        uint32_t stride = stride_of(in);
        raw = he.format && stride && stride % 4 == 0 && in.offset < 0x10000 && in.offset % std::min<uint32_t>(he.bytes, 4) == 0;
        if (!raw) LOG_FIRST(32, "vertex: converted on the CPU: v%d type %02X offset %u stride %u", regs[k], in.type, in.offset, stride);
    }
    if (raw) {
        // sparse indices: the vertices they read, copied in draw order
        bool dense = uint64_t(span) <= uint64_t(count) * 4 + 1024;
        int slot[16];
        std::fill(std::begin(slot), std::end(slot), -1);
        for (int k = 0; k < nregs; k++) {
            const auto& in = layout->inputs[size_t(regs[k])];
            if (slot[in.stream] < 0) {
                const auto& st = s.streams[size_t(in.stream)];
                uint32_t stride = stride_of(in);
                const uint8_t* data = static_cast<const uint8_t*>(HostPtr(mem::VirtualFromPhysical(R32(st.vb + 4))));
                slot[in.stream] = int(out.buffer_count);
                if (dense) {
                    out.buffers[out.buffer_count++] = {data + size_t(lo) * stride, span * stride, stride};
                } else {
                    auto& g = out.gathered[out.buffer_count];
                    g.resize(size_t(count) * stride);
                    for (uint32_t i = 0; i < count; i++) std::memcpy(&g[size_t(i) * stride], data + size_t(at(i)) * stride, stride);
                    out.buffers[out.buffer_count++] = {g.data(), count * stride, stride};
                }
            }
            HostElement he = HostFormat(in.type);
            out.elements[size_t(regs[k])] = {he.format, uint8_t(slot[in.stream]), uint16_t(in.offset)};
            out.decode |= uint64_t(he.decode) << (regs[k] * 4);
        }
        out.vertex_count = dense ? span : count;
        if (prim == 1 || prim == 2 || prim == 4 || prim == 5 || prim == 6) {
            out.strip = prim == 4 || prim == 6;
            out.topology = prim == 1 ? Topology::Points : prim <= 4 ? Topology::Lines : Topology::Triangles;
            if (ip && dense) {
                out.index_src = ip;
                out.index16 = true;
                out.index_count = count;
                out.base_vertex = -int32_t(first);
            } else {
                out.draw_count = count;
            }
            return count >= (prim == 1 ? 1u : prim <= 4 ? 2u : 3u);
        }
        ListIndices(prim, count, [&](uint32_t i) { return dense ? at(i) - lo : i; }, out.indices, out.topology);
        out.index_src = out.indices.data();
        out.index_count = uint32_t(out.indices.size());
        return out.index_count != 0;
    }

    // converted on the CPU: each element to a float4, vertices in range order when the indices are dense
    std::vector<uint32_t> seq(count);
    for (uint32_t i = 0; i < count; i++) seq[i] = at(i);
    bool ranged = uint64_t(span) <= uint64_t(count) * 2 + 64;
    uint32_t n = ranged ? span : count;
    out.vertex_count = n;
    out.vertices.resize(size_t(n) * nregs * 4);
    for (int k = 0; k < nregs; k++) {
        const auto& in = layout->inputs[size_t(regs[k])];
        const auto& st = s.streams[size_t(in.stream)];
        uint32_t data = mem::VirtualFromPhysical(R32(st.vb + 4));
        uint32_t stride = stride_of(in);
        const uint8_t* src = static_cast<const uint8_t*>(HostPtr(data + in.offset));
        for (uint32_t v = 0; v < n; v++) {
            uint32_t id = ranged ? lo + v : seq[v];
            Convert(in.type, src + size_t(id) * stride, &out.vertices[(size_t(v) * nregs + k) * 4]);
        }
        out.elements[size_t(regs[k])] = {DXGI_FORMAT_R32G32B32A32_FLOAT, 0, uint16_t(k * 16)};
    }
    out.buffer_count = 1;
    out.buffers[0] = {reinterpret_cast<const uint8_t*>(out.vertices.data()), uint32_t(out.vertices.size() * 4), uint32_t(nregs * 16)};
    ListIndices(prim, count, [&](uint32_t i) { return ranged ? seq[i] - lo : i; }, out.indices, out.topology);
    out.index_src = out.indices.data();
    out.index_count = uint32_t(out.indices.size());
    return out.index_count != 0;
}

}  // namespace gpu
