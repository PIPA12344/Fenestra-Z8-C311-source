#include <arm_neon.h>
#include "cube_frame.h"

/* LD1/ST1 byte elements support any byte alignment. Keep strict-align for
 * the rest of the TU, including compiler accesses to scalar structures. */
static inline uint8x8_t load_bytes(const uint8_t *p) {
    uint8x8_t value;
    __asm__("ld1 {%0.8b}, [%1]" : "=w"(value) : "r"(p), "m"(*(const uint8_t (*)[8])p));
    return value;
}

static inline void store_bytes(uint8_t *p, uint8x8_t value) {
    __asm__("st1 {%1.8b}, [%2]" : "=m"(*(uint8_t (*)[8])p) : "w"(value), "r"(p));
}

/* AArch64 NEON has no SIMD gather. Use one single-copy atomic read per lane,
 * with SIMD addressing, tag validation, rounded averaging and packing. */
static inline uint32x4_t gather(const uint32_t *cache, uint32x4_t index) {
    uint32x4_t p = vdupq_n_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 0), __ATOMIC_RELAXED));
    p = vsetq_lane_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 1), __ATOMIC_RELAXED), p, 1);
    p = vsetq_lane_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 2), __ATOMIC_RELAXED), p, 2);
    return vsetq_lane_u32(__atomic_load_n(cache + vgetq_lane_u32(index, 3), __ATOMIC_RELAXED), p, 3);
}

uint32_t jpeg_cube_cache_row_neon(const JpegCube *cube, uint32_t *cache,
                         const uint8_t *sy, const uint8_t *sc, uint8_t *dy, uint8_t *dc, uint32_t width) {
    uint32_t x = 0, misses = 0;
    uint32_t bulk = width & ~7u;
    for (; x < bulk; x += 8u) {
        if ((x & 63u) == 0 && width - x > 128u) {
            __builtin_prefetch(sy + x + 128u);
            __builtin_prefetch(sc + x + 128u);
        }
        uint16x8_t y = vmovl_u8(load_bytes(sy + x));
        uint16x4_t uv = vreinterpret_u16_u8(load_bytes(sc + x));
        uint16x4_t group = vorr_u16(vand_u16(uv, vdup_n_u16(63)),
                                     vand_u16(vshr_n_u16(uv, 2), vdup_n_u16(63 << 6)));
        uint16x4x2_t pair = vzip_u16(group, group);
        uint32x4_t lo = gather(cache, vaddw_u16(vshll_n_u16(pair.val[0], 8), vget_low_u16(y)));
        uint32x4_t hi = gather(cache, vaddw_u16(vshll_n_u16(pair.val[1], 8), vget_high_u16(y)));
        uint16x4_t tag = vadd_u16(vorr_u16(vand_u16(vshr_n_u16(uv, 6), vdup_n_u16(3)),
                                             vand_u16(vshr_n_u16(uv, 12), vdup_n_u16(12))), vdup_n_u16(1));
        uint8x16x2_t entries = {{vreinterpretq_u8_u32(lo), vreinterpretq_u8_u32(hi)}};
        uint8x8_t tags = vqtbl2_u8(entries, (uint8x8_t){3, 7, 11, 15, 19, 23, 27, 31});
        uint8x8_t tag_bytes = vreinterpret_u8_u16(vsli_n_u16(tag, tag, 8));
        uint8x8_t error = veor_u8(tag_bytes, tags);
        if (__builtin_expect(vget_lane_u64(vreinterpret_u64_u8(error), 0) != 0, 0)) {
            misses += jpeg_cube_cache_row_scalar(cube, cache, sy + x, sc + x, dy + x, dc + x, 8u);
            continue;
        }
        uint8x8_t luma = vqtbl2_u8(entries, (uint8x8_t){0, 4, 8, 12, 16, 20, 24, 28});
        uint8x8_t cbcr0 = vqtbl2_u8(entries, (uint8x8_t){1, 2, 9, 10, 17, 18, 25, 26});
        uint8x8_t cbcr1 = vqtbl2_u8(entries, (uint8x8_t){5, 6, 13, 14, 21, 22, 29, 30});
        store_bytes(dy + x, luma);
        store_bytes(dc + x, vrhadd_u8(cbcr0, cbcr1));
    }
    if (x < width)
        misses += jpeg_cube_cache_row_scalar(cube, cache, sy + x, sc + x, dy + x, dc + x, width - x);
    return misses;
}

#define FX_NEON_SENTINEL jpeg_neon_sentinel
#include "neon_sentinel.h"
