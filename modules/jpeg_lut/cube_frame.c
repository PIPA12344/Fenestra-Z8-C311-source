#include "cube_frame.h"

/* The 20-bit cache index holds the low six bits of each chroma and all
 * eight luma bits. The tag distinguishes the remaining 16 chroma ranges. */
static uint32_t tag_for(unsigned cb, unsigned cr) {
    return 1u + (cb >> 6) + ((cr >> 6) << 2);
}

_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "cache words are single-copy atomic");

#ifdef FX_Z8_LUT_SHARED_OUTPUT
/* Keep the setup path compact so the native encoder stays in its fixed RX
 * reservation. The hot scalar and NEON row kernels keep their optimization. */
__attribute__((minsize))
#endif
void jpeg_cube_cache_warm(const JpegCube *cube, uint32_t *cache, uint32_t first, uint32_t end) {
    if (end > CUBE_FRAME_CACHE_WORDS) end = CUBE_FRAME_CACHE_WORDS;
    while (first < end) {
        uint32_t group = first >> 8;
        uint32_t next = (group + 1u) << 8;
        if (next > end) next = end;
        /* For each six-bit index, choose the corresponding chroma in 96..159. */
        unsigned cb = 96u + ((group + 32u) & 63u);
        unsigned cr = 96u + (((group >> 6) + 32u) & 63u);
        uint32_t tag = tag_for(cb, cr) << 24;
        JpegCubeChroma offset = jpeg_cube_chroma(cb, cr);
        uint32_t *page = cache + (group << 8);
        for (unsigned y = first & 255u; y < next - (group << 8); ++y)
            __atomic_store_n(page + y, tag | jpeg_cube_sample(cube, y, offset), __ATOMIC_RELAXED);
        first = next;
    }
}

/* Keep tetrahedral interpolation off the common path. Each entry supplies
 * one luma and both chromas; both entries can be fetched before testing tags. */
static __attribute__((noinline)) uint64_t fill_pair(const JpegCube *cube, uint32_t *page,
                                  uint32_t ys, uint32_t uv, uint32_t tag, uint32_t a, uint32_t b) {
    unsigned y0 = ys & 255u, y1 = ys >> 8;
    JpegCubeChroma offset = jpeg_cube_chroma(uv & 255u, uv >> 8);
    if ((a >> 24) != tag) {
        a = (tag << 24) | jpeg_cube_sample(cube, y0, offset);
        __atomic_store_n(page + y0, a, __ATOMIC_RELAXED);
    }
    if (y1 == y0)
        b = a;
    else if ((b >> 24) != tag) {
        b = (tag << 24) | jpeg_cube_sample(cube, y1, offset);
        __atomic_store_n(page + y1, b, __ATOMIC_RELAXED);
    }
    return (uint64_t)a | ((uint64_t)b << 32);
}

static inline uint32_t load_pair(const uint8_t *p, int aligned) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (aligned) {
        uint16_t value;
        __builtin_memcpy(&value, __builtin_assume_aligned(p, 2), sizeof(value));
        return value;
    }
#else
    (void)aligned;
#endif
    return p[0] | ((uint32_t)p[1] << 8);
}

static inline void store_pair(uint8_t *p, uint32_t value, int aligned) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (aligned) {
        uint16_t pair = (uint16_t)value;
        __builtin_memcpy(__builtin_assume_aligned(p, 2), &pair, sizeof(pair));
        return;
    }
#else
    (void)aligned;
#endif
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static __attribute__((always_inline)) inline uint32_t render_row(const JpegCube *cube, uint32_t *cache,
                         const uint8_t *sy, const uint8_t *sc, uint8_t *dy, uint8_t *dc, uint32_t width,
                         int aligned) {
    const uint8_t *end = sy + (width & ~1u);
    uint32_t misses = 0;
    while (sy != end) {
        uint32_t uv = load_pair(sc, aligned), ys = load_pair(sy, aligned);
        sy += 2;
        sc += 2;
        unsigned cb = uv & 255u, cr = uv >> 8;
        uint32_t *page = cache + (((cr & 63u) << 14) | ((cb & 63u) << 8));
        uint32_t tag = tag_for(cb, cr);
        uint32_t a = __atomic_load_n(page + (ys & 255u), __ATOMIC_RELAXED);
        uint32_t b = __atomic_load_n(page + (ys >> 8), __ATOMIC_RELAXED);
        if (__builtin_expect((a >> 24) != tag || (b >> 24) != tag, 0)) {
            misses += (unsigned)((a >> 24) != tag);
            misses += (unsigned)((ys >> 8) != (ys & 255u) && (b >> 24) != tag);
            uint64_t pair = fill_pair(cube, page, ys, uv, tag, a, b);
            a = (uint32_t)pair;
            b = (uint32_t)(pair >> 32);
        }
        store_pair(dy, (a & 255u) | ((b & 255u) << 8), aligned);
        /* Rounded byte-wise average: the two chromas must never carry into
         * each other. Per-pixel rounding can differ from the pair oracle by 1. */
        uint32_t chroma = (a | b) - (((a ^ b) >> 1) & 0x007f7f00u);
        store_pair(dc, chroma >> 8, aligned);
        dy += 2;
        dc += 2;
    }
    return misses;
}

__attribute__((noinline)) uint32_t jpeg_cube_cache_row_scalar(const JpegCube *cube, uint32_t *cache,
                         const uint8_t *sy, const uint8_t *sc, uint8_t *dy, uint8_t *dc, uint32_t width) {
    if (width < 2u) return 0;
    if (((uintptr_t)sy | (uintptr_t)sc | (uintptr_t)dy | (uintptr_t)dc) & 1u)
        return render_row(cube, cache, sy, sc, dy, dc, width, 0);
    return render_row(cube, cache, sy, sc, dy, dc, width, 1);
}

__attribute__((noinline)) void jpeg_cube_cache_row(const JpegCube *cube, uint32_t *cache,
                         const uint8_t *sy, const uint8_t *sc, uint8_t *dy, uint8_t *dc, uint32_t width) {
    (void)jpeg_cube_cache_row_scalar(cube, cache, sy, sc, dy, dc, width);
}
