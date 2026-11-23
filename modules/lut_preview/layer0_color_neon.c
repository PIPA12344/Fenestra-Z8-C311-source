#include <arm_neon.h>
#include "layer0_color.h"

/* LD1/ST1 byte elements also allow unaligned packed planes. */
static inline uint8x8_t load_bytes(const uint8_t *p) {
    uint8x8_t value;
    __asm__("ld1 {%0.8b}, [%1]" : "=w"(value) : "r"(p), "m"(*(const uint8_t (*)[8])p));
    return value;
}

static inline void store_bytes(uint8_t *p, uint8x8_t value) {
    __asm__("st1 {%1.8b}, [%2]" : "=m"(*(uint8_t (*)[8])p) : "w"(value), "r"(p));
}

/* NEON has no gather. Read each full tagged cache word atomically. */
static inline uint32x4_t gather(const uint32_t *cache, uint32x4_t index) {
    uint32x4_t entries = vdupq_n_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 0)], __ATOMIC_RELAXED));
    entries = vsetq_lane_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 1)], __ATOMIC_RELAXED), entries, 1);
    entries = vsetq_lane_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 2)], __ATOMIC_RELAXED), entries, 2);
    return vsetq_lane_u32(__atomic_load_n(&cache[vgetq_lane_u32(index, 3)], __ATOMIC_RELAXED), entries, 3);
}

static inline uint8x16x2_t lookup(const uint32_t *cache, uint8x8_t luma, uint16x4x2_t pages) {
    uint16x8_t y = vmovl_u8(luma);
    uint32x4_t lo = gather(cache, vaddw_u16(vshll_n_u16(pages.val[0], 8), vget_low_u16(y)));
    uint32x4_t hi = gather(cache, vaddw_high_u16(vshll_n_u16(pages.val[1], 8), y));
    return (uint8x16x2_t){{vreinterpretq_u8_u32(lo), vreinterpretq_u8_u32(hi)}};
}

__attribute__((noinline))
void layer0_grade_nv12_planes_neon(const JpegCube *cube, uint32_t *cache,
                                  const Layer0Frame *frame, unsigned first_row, unsigned end_row) {
    unsigned width = frame->width;
    const uint8x8_t tag_bytes = {3, 7, 11, 15, 19, 23, 27, 31};
    const uint8x8_t y_bytes_at = {0, 4, 8, 12, 16, 20, 24, 28};
    const uint8x8_t uv_even = {1, 2, 9, 10, 17, 18, 25, 26};
    const uint8x8_t uv_odd = {5, 6, 13, 14, 21, 22, 29, 30};
    for (unsigned row = first_row; row < end_row; row += 2) {
        const uint8_t *sy0 = frame->src_y + (size_t)row * width, *sy1 = sy0 + width;
        const uint8_t *suv = frame->src_uv + (size_t)(row / 2) * width;
        uint8_t *dy0 = frame->dst_y + (size_t)row * width, *dy1 = dy0 + width;
        uint8_t *duv = frame->dst_uv + (size_t)(row / 2) * width;
        unsigned x = 0;
        for (; x < (width & ~7u); x += 8u) {
            if ((x & 63u) == 0 && width - x >= 128u + 64u) {
                __builtin_prefetch(sy0 + x + 128u);
                __builtin_prefetch(sy1 + x + 128u);
                __builtin_prefetch(suv + x + 128u);
            }
            uint16x4_t uv = vreinterpret_u16_u8(load_bytes(suv + x));
            uint16x4_t group = vorr_u16(vand_u16(uv, vdup_n_u16(63)),
                                       vand_u16(vshr_n_u16(uv, 2), vdup_n_u16(63 << 6)));
            uint16x4x2_t pages = vzip_u16(group, group);
            uint8x16x2_t top = lookup(cache, load_bytes(sy0 + x), pages);
            uint8x16x2_t bottom = lookup(cache, load_bytes(sy1 + x), pages);
            uint16x4_t tag = vadd_u16(vorr_u16(vand_u16(vshr_n_u16(uv, 6), vdup_n_u16(3)),
                                             vand_u16(vshr_n_u16(uv, 12), vdup_n_u16(12))), vdup_n_u16(1));
            uint8x8_t expected = vreinterpret_u8_u16(vsli_n_u16(tag, tag, 8));
            uint8x8_t wrong = vorr_u8(veor_u8(vqtbl2_u8(top, tag_bytes), expected),
                                     veor_u8(vqtbl2_u8(bottom, tag_bytes), expected));
            if (__builtin_expect(vget_lane_u64(vreinterpret_u64_u8(wrong), 0) != 0, 0)) {
                layer0_grade_nv12_pair_scalar(cube, cache, sy0 + x, sy1 + x, suv + x,
                                              dy0 + x, dy1 + x, duv + x, 8);
                continue;
            }
            uint16x8_t sum = vaddl_u8(vqtbl2_u8(top, uv_even), vqtbl2_u8(top, uv_odd));
            sum = vaddw_u8(sum, vqtbl2_u8(bottom, uv_even));
            sum = vaddw_u8(sum, vqtbl2_u8(bottom, uv_odd));
            store_bytes(dy0 + x, vqtbl2_u8(top, y_bytes_at));
            store_bytes(dy1 + x, vqtbl2_u8(bottom, y_bytes_at));
            store_bytes(duv + x, vrshrn_n_u16(sum, 2));
        }
        if (x < width)
            layer0_grade_nv12_pair_scalar(cube, cache, sy0 + x, sy1 + x, suv + x,
                                          dy0 + x, dy1 + x, duv + x, width - x);
    }
}

__attribute__((used,retain))
int layer0_grade_nv12_rows_neon(const JpegCube *cube, uint32_t *cache,
                               const uint8_t *src_nv12, size_t src_bytes,
                               uint8_t *dst_nv12, size_t dst_bytes,
                               unsigned first_row, unsigned end_row,
                               unsigned width, unsigned height) {
    size_t y_bytes = (size_t)width * height;
    if (!cube || !cache || !src_nv12 || !dst_nv12 || cube->grid < 2 || cube->grid > 33 ||
        !width || ((width | height | first_row | end_row) & 1u) ||
        first_row > end_row || end_row > height ||
        y_bytes > src_bytes || y_bytes / 2 > src_bytes - y_bytes ||
        y_bytes > dst_bytes || y_bytes / 2 > dst_bytes - y_bytes) return 0;
    const Layer0Frame frame = {src_nv12, src_nv12 + y_bytes, dst_nv12, dst_nv12 + y_bytes, width, height};
    layer0_grade_nv12_planes_neon(cube, cache, &frame, first_row, end_row);
    return 1;
}

#define FX_NEON_SENTINEL layer0_neon_sentinel
#include "neon_sentinel.h"
