#define _CRT_SECURE_NO_WARNINGS
#include "cube_frame.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_WIDTH = 8256, ROW_BYTES = MAX_WIDTH + 8, GUARD = 0xa5 };
static JpegCube cube;
static uint32_t storage[CUBE_FRAME_CACHE_WORDS + 2], warmed[CUBE_FRAME_CACHE_WORDS];
static uint32_t *const cache = storage + 1;
_Alignas(2) static uint8_t sy[ROW_BYTES], sc[ROW_BYTES], dy[ROW_BYTES], dc[ROW_BYTES];
static uint8_t expected_y[ROW_BYTES], expected_c[ROW_BYTES];
static uint32_t rng = 0x3112b52u;
static unsigned max_chroma_error;

static uint8_t clip(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

/* The original weighted sum and division are independent of the optimized
 * reduction and the new shared single-pixel sampler. */
static void reference_rgb(const uint8_t input[3], uint8_t result[3]) {
    unsigned n = cube.grid, index = 0;
    const unsigned stride[] = {1, n, n * n};
    unsigned frac[3], order[] = {0, 1, 2};
    for (unsigned c = 0; c < 3; ++c) {
        unsigned p = cube.axis[c][input[c]];
        unsigned lo = p >> 8;
        if (lo == n - 1) --lo;
        frac[c] = p - lo * 256;
        index += lo * stride[c];
    }
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = i + 1; j < 3; ++j)
            if (frac[order[i]] < frac[order[j]]) {
                unsigned swap = order[i];
                order[i] = order[j];
                order[j] = swap;
            }
    unsigned total[3] = {0};
    unsigned limit = 256;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned next = i == 3 ? 0 : frac[order[i]];
        for (unsigned c = 0; c < 3; ++c) total[c] += (limit - next) * cube.rgb[index][c];
        if (i < 3) index += stride[order[i]];
        limit = next;
    }
    for (unsigned c = 0; c < 3; ++c) result[c] = (uint8_t)((total[c] + 32896u) / 65792u);
}

static void reference_sample(int y, int u, int v, uint8_t output[3]) {
    u -= 128;
    v -= 128;
    const uint8_t input[] = {
        clip(y + ((91881 * v + 32768) >> 16)),
        clip(y + ((-22554 * u - 46802 * v + 32768) >> 16)),
        clip(y + ((116130 * u + 32768) >> 16)),
    };
    uint8_t rgb[3];
    reference_rgb(input, rgb);
    int r = rgb[0], g = rgb[1], b = rgb[2];
    output[0] = clip((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
    output[1] = clip(128 + ((2 * (-11059 * r - 21709 * g + 32768 * b) + 65536) >> 17));
    output[2] = clip(128 + ((2 * (32768 * r - 27439 * g - 5329 * b) + 65536) >> 17));
}

static uint8_t octet(void) {
    rng = rng * 1664525u + 1013904223u;
    return (uint8_t)(rng >> 24);
}

static void make_cube(void) {
    jpeg_cube_init(&cube);
    cube.grid = 33;
    cube.low[0] = 70000;
    cube.high[2] = 900000;
    for (unsigned b = 0; b < 33; ++b)
        for (unsigned g = 0; g < 33; ++g)
            for (unsigned r = 0; r < 33; ++r) {
                uint16_t *node = cube.rgb[cube.count++];
                node[0] = (uint16_t)(65535u * r * g / 1024u);
                node[1] = (uint16_t)(65535u * g * b / 1024u);
                node[2] = (uint16_t)(65535u * (32u - r) * b / 1024u);
            }
    assert(jpeg_cube_finish(&cube));
}

static void read_cube(const char *path) {
    FILE *fp = fopen(path, "rb");
    assert(fp);
    char line[CUBE_LINE + 2];
    jpeg_cube_init(&cube);
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        assert(jpeg_cube_line(&cube, line));
    }
    assert(!ferror(fp));
    assert(fclose(fp) == 0);
    assert(jpeg_cube_finish(&cube));
}

static void check_row(unsigned width, int in_place, int aligned) {
    unsigned y_offset = aligned ? 2u : 1u;
    unsigned c_offset = aligned ? 4u : 3u;
    uint8_t originals[2][ROW_BYTES];
    memcpy(originals[0], sy, ROW_BYTES);
    memcpy(originals[1], sc, ROW_BYTES);
    memcpy(expected_y, sy, ROW_BYTES);
    memcpy(expected_c, sc, ROW_BYTES);
    jpeg_cube_422(&cube, expected_y + y_offset, expected_c + c_offset, width);
    memset(dy, GUARD, sizeof(dy));
    memset(dc, GUARD, sizeof(dc));
    uint8_t *y = in_place ? sy : dy;
    uint8_t *c = in_place ? sc : dc;
    jpeg_cube_cache_row(&cube, cache, sy + y_offset, sc + c_offset, y + y_offset, c + c_offset, width);
    for (unsigned x = 0; x < width; x += 2) {
        uint8_t a[3], b[3];
        int cb = originals[1][c_offset + x], cr = originals[1][c_offset + x + 1];
        reference_sample(originals[0][y_offset + x], cb, cr, a);
        reference_sample(originals[0][y_offset + x + 1], cb, cr, b);
        assert(y[y_offset + x] == a[0] && y[y_offset + x + 1] == b[0]);
        assert(c[c_offset + x] == ((unsigned)a[1] + b[1] + 1u) / 2u);
        assert(c[c_offset + x + 1] == ((unsigned)a[2] + b[2] + 1u) / 2u);
    }
    for (unsigned x = 0; x < ROW_BYTES; ++x) {
        if (x < y_offset || x >= y_offset + width) assert(y[x] == (in_place ? originals[0][x] : GUARD));
        else assert(y[x] == expected_y[x]);
        if (x < c_offset || x >= c_offset + width) assert(c[x] == (in_place ? originals[1][x] : GUARD));
        else {
            unsigned error = (unsigned)abs((int)c[x] - (int)expected_c[x]);
            assert(error <= 1);
            if (error > max_chroma_error) max_chroma_error = error;
        }
    }
    if (!in_place) {
        assert(!memcmp(sy, originals[0], ROW_BYTES));
        assert(!memcmp(sc, originals[1], ROW_BYTES));
    }
}

static void prepare(void) {
    storage[0] = storage[CUBE_FRAME_CACHE_WORDS + 1] = 0xbaadcafeu;
    memset(cache, 0, sizeof(warmed));
    jpeg_cube_cache_warm(&cube, cache, 1, CUBE_FRAME_CACHE_WORDS / 2);
    assert(cache[0] == 0 && cache[CUBE_FRAME_CACHE_WORDS / 2] == 0);
    jpeg_cube_cache_warm(&cube, cache, CUBE_FRAME_CACHE_WORDS / 2, CUBE_FRAME_CACHE_WORDS + 1);
    jpeg_cube_cache_warm(&cube, cache, 0, 1);
    memcpy(warmed, cache, sizeof(warmed));
    for (unsigned i = 0; i < CUBE_FRAME_CACHE_WORDS; ++i) assert(warmed[i] >> 24);
}

static void check(void) {
    max_chroma_error = 0;
    prepare();
    for (unsigned x = 0; x < ROW_BYTES; ++x) {
        sy[x] = octet();
        sc[x] = (uint8_t)(96u + (octet() & 63u));
    }
    check_row(MAX_WIDTH, 0, 1);
    assert(!memcmp(warmed, cache, sizeof(warmed)));
    /* Same luma and low six chroma bits repeatedly collide in both
     * directions. Each of the sixteen tags must select its own sample. */
    for (unsigned tag = 0; tag < 32; ++tag) {
        unsigned t = (tag & 16u) ? (31u - tag) : tag;
        sy[1] = 107; sy[2] = 205;
        sc[3] = (uint8_t)(23u + 64u * (t & 3u));
        sc[4] = (uint8_t)(11u + 64u * (t >> 2));
        check_row(2, 0, 0);
    }
    const unsigned widths[] = {0, 2, 160, 640, 1620, 8256};
    for (unsigned row = 0; row < 72; ++row) {
        for (unsigned x = 0; x < ROW_BYTES; ++x) { sy[x] = octet(); sc[x] = octet(); }
        check_row(widths[row % 6], (int)(row / 6u & 1u), (int)(row / 12u & 1u));
    }
    assert(storage[0] == 0xbaadcafeu && storage[CUBE_FRAME_CACHE_WORDS + 1] == 0xbaadcafeu);
    printf("PASS grid %u max Y error 0 max chroma error %u\n", cube.grid, max_chroma_error);
}

int main(int argc, char **argv) {
    for (unsigned t = 0; t < 65664; ++t) assert(jpeg_cube_reduce(t) == t / 257u);
    make_cube();
    check();
    for (int i = 1; i < argc; ++i) {
        read_cube(argv[i]);
        check();
    }
    return 0;
}
