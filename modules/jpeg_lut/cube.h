#ifndef FX_JPEG_CUBE_H
#define FX_JPEG_CUBE_H
#include <stdint.h>

enum { CUBE_MAX_GRID = 33, CUBE_LINE = 384, CUBE_MAX_BYTES = 2 * 1024 * 1024 };
enum { CUBE_YUV_ENTRIES = 32 * 32 * 256 };

typedef struct {
    uint32_t grid, count, flags;
    int32_t low[3], high[3];
    uint16_t axis[3][256];
    uint16_t rgb[CUBE_MAX_GRID * CUBE_MAX_GRID * CUBE_MAX_GRID][3];
} JpegCube;

void jpeg_cube_init(JpegCube *cube);
int jpeg_cube_line(JpegCube *cube, char *line);
int jpeg_cube_finish(JpegCube *cube);
/* Compile a finished CUBE to a channel permutation followed by Q15 curves.
 * source[c] identifies the input driving output c. Use outputs only on success. */
int jpeg_cube_compile_curves(const JpegCube *cube, uint16_t curves[3][256], uint8_t source[3]);
void jpeg_cube_rgb(const JpegCube *cube, uint8_t r, uint8_t g, uint8_t b, uint8_t out[3]);
typedef struct { int r, g, b; } JpegCubeChroma;

static inline JpegCubeChroma jpeg_cube_chroma(unsigned cb, unsigned cr) {
    int u = (int)cb - 128, v = (int)cr - 128;
    return (JpegCubeChroma){
        (91881 * v + 32768) >> 16,
        (-22554 * u - 46802 * v + 32768) >> 16,
        (116130 * u + 32768) >> 16,
    };
}

/* Exact when the rounded interpolation numerator has been shifted by 8:
 * 0 <= t <= (256*65535 + 32896) >> 8 == 65663, divisor 65792/256 == 257. */
static inline unsigned jpeg_cube_reduce(unsigned t) {
    return (t - (t >> 8)) >> 8;
}

/* The same Y/Cb/Cr bytes as grading a pair of identical Y inputs. */
uint32_t jpeg_cube_sample(const JpegCube *cube, unsigned luma, JpegCubeChroma offset);
void jpeg_cube_422(const JpegCube *cube, uint8_t *y, uint8_t *uv, uint32_t width);
void jpeg_cube_bake(const JpegCube *cube, uint32_t *table, uint32_t first, uint32_t end);
void jpeg_cube_apply(const uint32_t *table, uint8_t *y, uint8_t *uv, uint32_t width);

#endif
