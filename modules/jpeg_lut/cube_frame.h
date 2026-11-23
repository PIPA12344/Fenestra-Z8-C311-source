#ifndef FX_JPEG_CUBE_FRAME_H
#define FX_JPEG_CUBE_FRAME_H

#include "cube.h"

enum { CUBE_FRAME_CACHE_WORDS = 1u << 20 };

/* Prepare all words in [0, CUBE_FRAME_CACHE_WORDS) with the same finished
 * CUBE before processing. Work may be split into disjoint or retried ranges.
 * Concurrent frame workers share the aligned word array using relaxed
 * atomics. The full warm/grade lifetime belongs to one immutable CUBE. */
void jpeg_cube_cache_warm(const JpegCube *cube, uint32_t *cache, uint32_t first, uint32_t end);

/* Independent Y and interleaved Cb/Cr planes, one byte per visible pixel
 * in each plane. Width is even for 4:2:2. Only the active row is touched.
 * Each output plane may also be its matching input plane. */
void jpeg_cube_cache_row(const JpegCube *cube, uint32_t *cache,
                         const uint8_t *sy, const uint8_t *sc, uint8_t *dy, uint8_t *dc, uint32_t width);
uint32_t jpeg_cube_cache_row_scalar(const JpegCube *cube, uint32_t *cache,
                         const uint8_t *sy, const uint8_t *sc, uint8_t *dy, uint8_t *dc, uint32_t width);
uint32_t jpeg_cube_cache_row_neon(const JpegCube *cube, uint32_t *cache,
                         const uint8_t *sy, const uint8_t *sc, uint8_t *dy, uint8_t *dc, uint32_t width);

enum { JPEG_NEON_UNAVAILABLE, JPEG_NEON_PASS, JPEG_NEON_FAILED };
/* Invoke only from a verified native EL1 RTOS task context. */
unsigned jpeg_neon_sentinel(int32_t (*delay)(uint32_t));

#endif
