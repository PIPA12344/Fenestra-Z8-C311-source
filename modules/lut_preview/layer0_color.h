#ifndef Z8_LAYER0_COLOR_H
#define Z8_LAYER0_COLOR_H

#include <stddef.h>
#include "cube_frame.h"

enum { REAR_W=1024, REAR_H=682, REAR_NV12=REAR_W*REAR_H*3/2,
       REAR_CACHE=CUBE_FRAME_CACHE_WORDS*4 };

typedef struct {
    const uint8_t *src_y, *src_uv;
    uint8_t *dst_y, *dst_uv;
    unsigned width, height;
} Layer0Frame;

/* Validated plane geometry. The two routes can share this aligned tagged
 * cache as long as the cube is immutable and all grading joins before clear. */
void layer0_grade_nv12_planes(const JpegCube *cube, uint32_t *cache,
                             const Layer0Frame *frame, unsigned first, unsigned end);
void layer0_grade_nv12_planes_neon(const JpegCube *cube, uint32_t *cache,
                                  const Layer0Frame *frame, unsigned first, unsigned end);

/* Process whole 2x2 quads. Reuse a cache for the same LUT, starting with all
 * words zero. Use disjoint source and destination frames or the same frame. */
int layer0_grade_nv12_rows(const JpegCube *cube, uint32_t *cache,
                           const uint8_t *src_nv12, size_t src_bytes,
                           uint8_t *dst_nv12, size_t dst_bytes,
                           unsigned first_row, unsigned end_row);

/* The SIMD kernel also accepts packed NV12 of other even dimensions. */
int layer0_grade_nv12_rows_neon(const JpegCube *cube, uint32_t *cache,
                               const uint8_t *src_nv12, size_t src_bytes,
                               uint8_t *dst_nv12, size_t dst_bytes,
                               unsigned first_row, unsigned end_row,
                               unsigned width, unsigned height);
/* Checked by the frame kernel before passing whole quads to this fallback. */
void layer0_grade_nv12_pair_scalar(const JpegCube *cube, uint32_t *cache,
                                   const uint8_t *src_y, const uint8_t *src_y1, const uint8_t *src_uv,
                                   uint8_t *dst_y, uint8_t *dst_y1, uint8_t *dst_uv, unsigned width);

/* Only the native EL1 worker may invoke the sentinel. */
unsigned layer0_neon_sentinel(int32_t (*delay)(uint32_t));

#endif
