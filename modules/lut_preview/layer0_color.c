#include "layer0_color.h"

static __attribute__((noinline)) uint32_t miss(const JpegCube *cube, uint32_t *slot,
                                               unsigned luma, unsigned cb, unsigned cr,
                                               uint32_t key) {
    /* A preceding luma in the quad may already have filled this slot. */
    uint32_t value = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if ((value & UINT32_C(0xff000000)) != key) {
        value = key | jpeg_cube_sample(cube, luma, jpeg_cube_chroma(cb, cr));
        /* Other cores may replace the tag concurrently. Keep the tag and
         * all three output components in one single-copy atomic word. */
        __atomic_store_n(slot, value, __ATOMIC_RELAXED);
    }
    return value;
}

static inline __attribute__((always_inline)) void grade_pair(const JpegCube *cube, uint32_t *cache,
                                    const uint8_t *src_y, const uint8_t *src_y1, const uint8_t *src_uv,
                                    uint8_t *dst_y, uint8_t *dst_y1, uint8_t *dst_uv, unsigned width) {
    for (unsigned x = 0; x < width; x += 2) {
        unsigned cb = src_uv[x], cr = src_uv[x + 1];
        uint32_t key = (1u + (cb >> 6) + ((cr >> 6) << 2)) << 24;
        uint32_t *page = cache + (((cr & 63u) << 14) | ((cb & 63u) << 8));
        unsigned y00 = src_y[x], y01 = src_y[x + 1];
        unsigned y10 = src_y1[x], y11 = src_y1[x + 1];
        uint32_t a = __atomic_load_n(&page[y00], __ATOMIC_RELAXED);
        uint32_t b = __atomic_load_n(&page[y01], __ATOMIC_RELAXED);
        uint32_t c = __atomic_load_n(&page[y10], __ATOMIC_RELAXED);
        uint32_t d = __atomic_load_n(&page[y11], __ATOMIC_RELAXED);

        if (__builtin_expect((a & UINT32_C(0xff000000)) != key, 0))
            a = miss(cube, &page[y00], y00, cb, cr, key);
        if (__builtin_expect((b & UINT32_C(0xff000000)) != key, 0))
            b = miss(cube, &page[y01], y01, cb, cr, key);
        if (__builtin_expect((c & UINT32_C(0xff000000)) != key, 0))
            c = miss(cube, &page[y10], y10, cb, cr, key);
        if (__builtin_expect((d & UINT32_C(0xff000000)) != key, 0))
            d = miss(cube, &page[y11], y11, cb, cr, key);

        dst_y[x] = (uint8_t)a;
        dst_y[x + 1] = (uint8_t)b;
        dst_y1[x] = (uint8_t)c;
        dst_y1[x + 1] = (uint8_t)d;
        unsigned sum_cb = (unsigned)(uint8_t)(a >> 8) + (uint8_t)(b >> 8) +
                          (uint8_t)(c >> 8) + (uint8_t)(d >> 8);
        unsigned sum_cr = (unsigned)(uint8_t)(a >> 16) + (uint8_t)(b >> 16) +
                          (uint8_t)(c >> 16) + (uint8_t)(d >> 16);
        dst_uv[x] = (uint8_t)((sum_cb + 2) >> 2);
        dst_uv[x + 1] = (uint8_t)((sum_cr + 2) >> 2);
    }
}

void layer0_grade_nv12_pair_scalar(const JpegCube *cube, uint32_t *cache,
                                    const uint8_t *src_y, const uint8_t *src_y1, const uint8_t *src_uv,
                                    uint8_t *dst_y, uint8_t *dst_y1, uint8_t *dst_uv, unsigned width) {
    grade_pair(cube, cache, src_y, src_y1, src_uv, dst_y, dst_y1, dst_uv, width);
}

void layer0_grade_nv12_planes(const JpegCube *cube, uint32_t *cache,
                             const Layer0Frame *frame, unsigned first, unsigned end) {
    size_t width = frame->width;
    for (unsigned row = first; row < end; row += 2) {
        size_t y = row * width, uv = (row / 2u) * width;
        grade_pair(cube, cache, frame->src_y + y, frame->src_y + y + width, frame->src_uv + uv,
                   frame->dst_y + y, frame->dst_y + y + width, frame->dst_uv + uv, frame->width);
    }
}

__attribute__((used,retain))
int layer0_grade_nv12_rows(const JpegCube *cube, uint32_t *cache,
                           const uint8_t *src_nv12, size_t src_bytes,
                           uint8_t *dst_nv12, size_t dst_bytes,
                           unsigned first_row, unsigned end_row) {
    if (!cube || !cache || !src_nv12 || !dst_nv12 || cube->grid < 2 || cube->grid > 33 ||
        src_bytes < REAR_NV12 || dst_bytes < REAR_NV12 ||
        ((first_row | end_row) & 1u) || first_row > end_row || end_row > REAR_H) return 0;

    const Layer0Frame frame = {src_nv12, src_nv12 + REAR_W * REAR_H,
                              dst_nv12, dst_nv12 + REAR_W * REAR_H, REAR_W, REAR_H};
    layer0_grade_nv12_planes(cube, cache, &frame, first_row, end_row);
    return 1;
}
