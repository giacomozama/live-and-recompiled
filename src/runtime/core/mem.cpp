// memset and memcmp for Windows builds. Zig's compiler_rt brings both (weak, so these replace them) and both go a byte
// at a time: with them the render thread spent a quarter of its time clearing and comparing structs. The rest of the
// C library's are the system's or vectorized already. 16 bytes at a time with SSE2, rep stosb for long fills; no_builtin
// keeps clang from making the loops calls to the functions they are.
#ifdef _WIN32
#include <emmintrin.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>

extern "C" {

__attribute__((no_builtin)) void* memset(void* dest, int c, size_t n) {
    auto* d = static_cast<uint8_t*>(dest);
    if (n >= 2048) {
        __stosb(d, uint8_t(c), n);  // the CPU's fast string fill (ERMSB) beats any loop this long
        return dest;
    }
    if (n >= 16) {
        __m128i v = _mm_set1_epi8(char(c));
        // the last block overlaps the one before it rather than finishing a byte at a time
        for (size_t i = 0; i + 16 < n; i += 16) _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i), v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + n - 16), v);
        return dest;
    }
    if (n >= 4) {
        uint32_t w = uint8_t(c) * 0x01010101u;
        if (n >= 8) {
            uint64_t q = uint64_t(w) << 32 | w;
            __builtin_memcpy_inline(d, &q, 8);
            __builtin_memcpy_inline(d + n - 8, &q, 8);
        } else {
            __builtin_memcpy_inline(d, &w, 4);
            __builtin_memcpy_inline(d + n - 4, &w, 4);
        }
        return dest;
    }
    for (size_t i = 0; i < n; i++) d[i] = uint8_t(c);
    return dest;
}

__attribute__((no_builtin)) int memcmp(const void* left, const void* right, size_t n) {
    auto* a = static_cast<const uint8_t*>(left);
    auto* b = static_cast<const uint8_t*>(right);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        unsigned diff = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(x, y))) ^ 0xFFFFu;
        if (diff) {
            size_t k = i + size_t(__builtin_ctz(diff));
            return int(a[k]) - int(b[k]);
        }
    }
    for (; i < n; i++)
        if (a[i] != b[i]) return int(a[i]) - int(b[i]);
    return 0;
}

}  // extern "C"
#endif
