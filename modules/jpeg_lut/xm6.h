#ifndef JPEG_LUT_XM6_H
#define JPEG_LUT_XM6_H
#include <stdint.h>
#include "cube.h"

enum { XM6_MAX_PITCH = 8256, XM6_WORK = 0x4b000 + 0x330 * XM6_MAX_PITCH,
       XM6_CONTROL = 0xc00, XM6_ARENA = XM6_CONTROL + XM6_WORK };
typedef enum { XM6_UNCERTAIN = -1, XM6_SKIPPED = 0, XM6_COMPLETE = 1 } Xm6Result;
typedef struct { uint16_t values[3][256]; uint8_t source[3]; } Xm6Curves;

/* The caller owns the uncached arena and four independent planes. On
 * XM6_UNCERTAIN retain all five allocations until completion is proven. */
void jpeg_xm6_prepare(uint8_t *arena);
int jpeg_xm6_prepare_curves(uint8_t *arena, const Xm6Curves *curves);
Xm6Result jpeg_xm6_frame(uint8_t *arena, const uint8_t *sy, const uint8_t *sc,
                        uint8_t *dy, uint8_t *dc, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t *status);
#endif
