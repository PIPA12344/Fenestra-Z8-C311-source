#include "cube.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JpegCube cube;
static uint16_t curves[3][256];

static void make_cube(unsigned n, const uint8_t axes[3]) {
    jpeg_cube_init(&cube);
    cube.grid = n;
    unsigned stride[] = {1, n, n*n};
    for (unsigned i = 0; i < n*n*n; ++i) {
        for (unsigned c = 0; c < 3; ++c) {
            unsigned v = (i / stride[axes[c]]) % n;
            cube.rgb[i][c] = (uint16_t)((65535u*v + (n-1u)/2u) / (n-1u));
        }
        ++cube.count;
    }
}

static void check_curves(const uint8_t axes[3]) {
    uint8_t source[3];
    assert(jpeg_cube_finish(&cube));
    assert(jpeg_cube_compile_curves(&cube, curves, source));
    assert(!memcmp(source, axes, 3));
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned i = 129; i < 256; ++i) assert(curves[c][i] == curves[c][128]);
    }
    /* Each axis visits every possible 8-bit value, using unequal RGB inputs
     * so an incorrect red/blue mapping cannot pass on a neutral ramp. */
    for (unsigned v = 0; v < 256; ++v) {
        uint8_t rgb[] = {(uint8_t)v, (uint8_t)(97u*v + 53u), (uint8_t)(29u*v + 101u)};
        uint8_t expected[3];
        jpeg_cube_rgb(&cube, rgb[0], rgb[1], rgb[2], expected);
        for (unsigned c = 0; c < 3; ++c) {
            unsigned x = rgb[source[c]];
            int result = (curves[c][x/2] + curves[c][(x+1u)/2] + 128) / 256;
            if (result > 255) result = 255;
            assert(abs(result - expected[c]) <= 1);
        }
    }
}

int main(void) {
    const uint8_t orders[][3] = {{0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}};
    uint8_t source[3];
    make_cube(2, orders[0]);
    check_curves(orders[0]);
    for (unsigned i = 0; i <= 128; ++i)
        for (unsigned c = 0; c < 3; ++c) assert(curves[c][i] == i*256u);
    for (unsigned i = 0; i < 6; ++i) {
        make_cube(33, orders[i]);
        cube.low[0] = -160000;
        cube.high[2] = 1380000;
        for (unsigned j = 0; j < cube.count; ++j) {
            unsigned red = cube.rgb[j][0];
            cube.rgb[j][0] = (uint16_t)((red * red + 32767u) / 65535u);
            cube.rgb[j][1] = (uint16_t)(65535u - cube.rgb[j][1]);
        }
        check_curves(orders[i]);
    }
    /* Alter one non-diagonal interior node, preserving all axes and the diagonal. */
    make_cube(17, orders[0]);
    ++cube.rgb[(3*17+4)*17+5][2];
    assert(jpeg_cube_finish(&cube));
    assert(!jpeg_cube_compile_curves(&cube, curves, source));
    make_cube(3, orders[0]);
    cube.low[2] = 50000;
    cube.high[2] = 51000;
    assert(jpeg_cube_finish(&cube));
    assert(!jpeg_cube_compile_curves(&cube, curves, source));
    --cube.count;
    assert(!jpeg_cube_compile_curves(&cube, curves, source));
    puts("PASS: XM6 CUBE curves");
    return 0;
}
