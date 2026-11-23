#include "cube.h"

enum { UNIT = 1000000, MINIMUM = 1, MAXIMUM = 2, RANGE = 4, STYLE = 8 };

static int space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int digit(char c) { return c >= '0' && c <= '9'; }
static char *skip(char *p) { while (space(*p)) ++p; return p; }

static int token(char **cursor, const char *word) {
    char *p = *cursor;
    while (*word && *p == *word) { ++word; ++p; }
    if (*word || (*p && !space(*p) && *p != '#')) return 0;
    *cursor = skip(p);
    return 1;
}

static int end(const char *p) { return !*p || *p == '#'; }

/* Bounded decimal parser for CUBE input. Keep enough precision for the
 * 16-bit table and accept scientific notation used by LUT exporters. */
static int number(char **cursor, int32_t *result) {
    char *p = skip(*cursor);
    int negative = *p == '-';
    if (*p == '+' || *p == '-') ++p;
    uint64_t mantissa = 0;
    int fraction = 0, digits = 0;
    while (digit(*p)) {
        if (++digits > 12) return 0;
        mantissa = mantissa * 10u + (unsigned)(*p++ - '0');
    }
    if (*p == '.') {
        ++p;
        while (digit(*p)) {
            if (++digits > 12) return 0;
            mantissa = mantissa * 10u + (unsigned)(*p++ - '0');
            ++fraction;
        }
    }
    if (!digits) return 0;
    int power = 6 - fraction;
    if (*p == 'e' || *p == 'E') {
        ++p;
        int minus = *p == '-';
        if (*p == '+' || *p == '-') ++p;
        if (!digit(*p)) return 0;
        int exponent = 0;
        while (digit(*p)) {
            exponent = exponent * 10 + (*p++ - '0');
            if (exponent > 20) return 0;
        }
        power += minus ? -exponent : exponent;
    }
    if (*p && !space(*p) && *p != '#') return 0;
    while (power < 0) { mantissa /= 10u; ++power; }
    while (power > 0) {
        if (mantissa > 10000000u) return 0;
        mantissa *= 10u;
        --power;
    }
    if (mantissa > 100000000u) return 0;
    *result = negative ? -(int32_t)mantissa : (int32_t)mantissa;
    *cursor = skip(p);
    return 1;
}

static int numbers(char *p, int32_t *values, unsigned count) {
    for (unsigned i = 0; i < count; ++i) if (!number(&p, &values[i])) return 0;
    return end(p);
}

void jpeg_cube_init(JpegCube *cube) {
    cube->grid = cube->count = cube->flags = 0;
    for (unsigned c = 0; c < 3; ++c) { cube->low[c] = 0; cube->high[c] = UNIT; }
}

int jpeg_cube_line(JpegCube *cube, char *line) {
    char *p = skip(line);
    if (token(&p, "#LUMIXPHOTOSTYLE")) {
        if ((cube->flags & STYLE) || !token(&p, "STD") || !end(p)) return 0;
        cube->flags |= STYLE;
        return 1;
    }
    if (end(p)) return 1;
    if (!cube->count && token(&p, "TITLE")) return 1;
    if (!cube->count && token(&p, "LUT_3D_SIZE")) {
        unsigned grid = 0;
        if (cube->grid || !digit(*p)) return 0;
        while (digit(*p)) {
            grid = grid * 10u + (unsigned)(*p++ - '0');
            if (grid > CUBE_MAX_GRID) return 0;
        }
        if (grid < 2 || !end(skip(p))) return 0;
        cube->grid = grid;
        return 1;
    }
    if (!cube->count && token(&p, "DOMAIN_MIN")) {
        if ((cube->flags & (MINIMUM | RANGE)) || !numbers(p, cube->low, 3)) return 0;
        cube->flags |= MINIMUM;
        return 1;
    }
    if (!cube->count && token(&p, "DOMAIN_MAX")) {
        if ((cube->flags & (MAXIMUM | RANGE)) || !numbers(p, cube->high, 3)) return 0;
        cube->flags |= MAXIMUM;
        return 1;
    }
    if (!cube->count && token(&p, "LUT_3D_INPUT_RANGE")) {
        int32_t range[2];
        if ((cube->flags & (MINIMUM | MAXIMUM | RANGE)) || !numbers(p, range, 2)) return 0;
        for (unsigned c = 0; c < 3; ++c) { cube->low[c] = range[0]; cube->high[c] = range[1]; }
        cube->flags |= RANGE;
        return 1;
    }
    uint32_t n = cube->grid;
    int32_t rgb[3];
    if (!n || cube->count >= n * n * n || !numbers(p, rgb, 3)) return 0;
    for (unsigned c = 0; c < 3; ++c) {
        int32_t value = rgb[c] < 0 ? 0 : rgb[c] > UNIT ? UNIT : rgb[c];
        cube->rgb[cube->count][c] = (uint16_t)(((uint64_t)value * 65535u + UNIT / 2) / UNIT);
    }
    ++cube->count;
    return 1;
}

static unsigned coordinate(const JpegCube *cube, unsigned c, unsigned v, unsigned cell_units) {
    int64_t offset = (int64_t)v * UNIT - (int64_t)cube->low[c] * 255;
    int64_t span = ((int64_t)cube->high[c] - cube->low[c]) * 255;
    unsigned top = (cube->grid - 1u) * cell_units;
    return offset <= 0 ? 0u : offset >= span ? top : (unsigned)((offset * top + span / 2) / span);
}

int jpeg_cube_finish(JpegCube *cube) {
    uint32_t n = cube->grid;
    if (n < 2 || n > CUBE_MAX_GRID || cube->count != n * n * n) return 0;
    for (unsigned c = 0; c < 3; ++c) {
        if (cube->high[c] <= cube->low[c]) return 0;
        for (unsigned v = 0; v < 256; ++v)
            cube->axis[c][v] = (uint16_t)coordinate(cube, c, v, 256);
    }
    return 1;
}

static int depends_only_on(const JpegCube *cube, unsigned channel, unsigned stride) {
    unsigned block = cube->grid * stride;
    for (unsigned start = 0; start < cube->count; start += block) {
        for (unsigned axis = 0; axis < block; axis += stride) {
            uint16_t expected = cube->rgb[axis][channel];
            for (unsigned i = 0; i < stride; ++i) {
                if (cube->rgb[start + axis + i][channel] != expected) return 0;
            }
        }
    }
    return 1;
}

int jpeg_cube_compile_curves(const JpegCube *cube, uint16_t curves[3][256], uint8_t source[3]) {
    static const uint8_t orders[][3] = {{0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}};
    if (!cube || !curves || !source) return 0;
    unsigned n = cube->grid;
    if (n < 2 || n > CUBE_MAX_GRID || cube->count != n * n * n) return 0;
    const unsigned strides[] = {1, n, n * n};
    unsigned allowed[3] = {0};
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned a = 0; a < 3; ++a)
            if (depends_only_on(cube, c, strides[a])) allowed[c] |= 1u << a;
        if (!allowed[c]) return 0;
    }
    unsigned order = 0;
    for (; order < sizeof(orders)/sizeof(orders[0]); ++order) {
        const uint8_t *axes = orders[order];
        if ((allowed[0] & (1u << axes[0])) && (allowed[1] & (1u << axes[1])) &&
            (allowed[2] & (1u << axes[2]))) break;
    }
    if (order == sizeof(orders)/sizeof(orders[0])) return 0;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned a = orders[order][c];
        source[c] = (uint8_t)a;
        unsigned stride = strides[a];
        if (cube->high[a] <= cube->low[a]) return 0;
        for (unsigned i = 0; i <= 128; ++i) {
            unsigned input = i < 128 ? 2u * i : 255u;
            unsigned pos = coordinate(cube, a, input, 32768u);
            unsigned lo = pos >> 15, hi = lo < n - 1u ? lo + 1u : lo;
            unsigned f = pos & 32767u;
            unsigned value = (32768u - f) * cube->rgb[lo * stride][c] + f * cube->rgb[hi * stride][c];
            /* One output code equals 128 DSP units. 65535*32768/(255*128) = 65792. */
            int knot = (int)((value + 32896u) / 65792u);
            if (i == 128) {
                /* Code 255 is halfway between the last two hardware knots. */
                knot = 2 * knot - curves[c][127];
                if (knot < 0) knot = 0;
                if (knot > 32768) knot = 32768;
            }
            curves[c][i] = (uint16_t)knot;
        }
        for (unsigned i = 129; i < 256; ++i) curves[c][i] = curves[c][128];
    }
    /* A sharp bend can fall between DSP knots. Check each 8-bit input
     * against the full interpolator before accepting the sampled curves. */
    for (unsigned v = 0; v < 256; ++v) {
        uint8_t expected[3];
        jpeg_cube_rgb(cube, (uint8_t)v, (uint8_t)v, (uint8_t)v, expected);
        for (unsigned c = 0; c < 3; ++c) {
            unsigned pixel = (curves[c][v / 2u] + (unsigned)curves[c][(v + 1u) / 2u] + 128u) >> 8;
            if (pixel > 255u) pixel = 255u;
            if (pixel > (unsigned)expected[c] + 1u || pixel + 1u < expected[c]) return 0;
        }
    }
    return 1;
}

void jpeg_cube_rgb(const JpegCube *cube, uint8_t r, uint8_t g, uint8_t b, uint8_t out[3]) {
    unsigned n = cube->grid;
    unsigned pos[3] = {cube->axis[0][r], cube->axis[1][g], cube->axis[2][b]};
    unsigned stride[3] = {1, n, n * n};
    unsigned frac[3];
    unsigned base = 0;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned lo = pos[c] >> 8;
        if (lo == n - 1u) { --lo; frac[c] = 256; }
        else frac[c] = pos[c] & 255u;
        base += lo * stride[c];
    }
    /* Sort the fractions to select the four vertices of the tetrahedron. */
    unsigned a = 0, d = 1, e = 2;
    if (frac[a] < frac[d]) { a = 1; d = 0; }
    if (frac[d] < frac[e]) { unsigned old = d; d = e; e = old; }
    if (frac[a] < frac[d]) { unsigned old = a; a = d; d = old; }
    unsigned w0 = 256u - frac[a], w1 = frac[a] - frac[d];
    unsigned w2 = frac[d] - frac[e], w3 = frac[e];
    unsigned p1 = base + stride[a], p2 = p1 + stride[d], p3 = p2 + stride[e];
    for (unsigned c = 0; c < 3; ++c) {
        unsigned value = w0 * cube->rgb[base][c] + w1 * cube->rgb[p1][c]
                       + w2 * cube->rgb[p2][c] + w3 * cube->rgb[p3][c];
        out[c] = (uint8_t)jpeg_cube_reduce((value + 32896u) >> 8);
    }
}

static uint8_t byte(int value) { return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value); }

static inline void sample_rgb(const JpegCube *cube, int luma, JpegCubeChroma offset, uint8_t rgb[3]) {
    jpeg_cube_rgb(cube, byte(luma + offset.r), byte(luma + offset.g), byte(luma + offset.b), rgb);
}

uint32_t jpeg_cube_sample(const JpegCube *cube, unsigned luma, JpegCubeChroma offset) {
    uint8_t rgb[3];
    sample_rgb(cube, (int)luma, offset, rgb);
    int r = rgb[0], g = rgb[1], b = rgb[2];
    uint32_t y = byte((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
    uint32_t cb = byte(128 + ((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16));
    uint32_t cr = byte(128 + ((32768 * r - 27439 * g - 5329 * b + 32768) >> 16));
    return y | (cb << 8) | (cr << 16);
}

void jpeg_cube_422(const JpegCube *cube, uint8_t *y, uint8_t *uv, uint32_t width) {
    for (uint32_t x = 0; x < width; x += 2u) {
        JpegCubeChroma offset = jpeg_cube_chroma(uv[x], uv[x + 1u]);
        if (y[x] == y[x + 1u]) {
            uint32_t sample = jpeg_cube_sample(cube, y[x], offset);
            y[x] = y[x + 1u] = (uint8_t)sample;
            uv[x] = (uint8_t)(sample >> 8);
            uv[x + 1u] = (uint8_t)(sample >> 16);
            continue;
        }
        int sum_cb = 0, sum_cr = 0;
        for (unsigned i = 0; i < 2u; ++i) {
            uint8_t rgb[3];
            sample_rgb(cube, y[x + i], offset, rgb);
            int r = rgb[0], g = rgb[1], b = rgb[2];
            y[x + i] = byte((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
            sum_cb += -11059 * r - 21709 * g + 32768 * b;
            sum_cr += 32768 * r - 27439 * g - 5329 * b;
        }
        uv[x] = byte(128 + ((sum_cb + 65536) >> 17));
        uv[x + 1] = byte(128 + ((sum_cr + 65536) >> 17));
    }
}

/* Y remains eight-bit. Quantize chroma to 32 centers per axis, and store
 * deltas so the original chroma within a bin still contributes to the result.
 * Leave one carry bit per nine-bit delta so two entries can be summed at once. */
void jpeg_cube_bake(const JpegCube *cube, uint32_t *table, uint32_t first, uint32_t end) {
    for (uint32_t i = first; i < end; ++i) {
        uint8_t luma = (uint8_t)i;
        uint8_t cb = (uint8_t)(((i >> 8) & 31u) * 8u + 4u);
        uint8_t cr = (uint8_t)((i >> 13) * 8u + 4u);
        uint32_t sample = jpeg_cube_sample(cube, luma, jpeg_cube_chroma(cb, cr));
        uint32_t dy = (uint32_t)(255 + (int)(sample & 255u) - luma);
        uint32_t db = (uint32_t)(255 + (int)((sample >> 8) & 255u) - cb);
        uint32_t dr = (uint32_t)(255 + (int)(sample >> 16) - cr);
        table[i] = dy | (db << 10) | (dr << 20);
    }
}

void jpeg_cube_apply(const uint32_t *table, uint8_t *y, uint8_t *uv, uint32_t width) {
    for (uint32_t x = 0; x < width; x += 2u) {
        uint32_t base = ((uint32_t)(uv[x] >> 3) << 8) | ((uint32_t)(uv[x + 1] >> 3) << 13);
        uint32_t a = table[base | y[x]], b = table[base | y[x + 1]];
        uint32_t sum = a + b + (1u << 10) + (1u << 20);
        y[x] = byte((int)y[x] + (int)(a & 511u) - 255);
        y[x + 1] = byte((int)y[x + 1] + (int)(b & 511u) - 255);
        uv[x] = byte((int)uv[x] + (int)((sum >> 11) & 511u) - 255);
        uv[x + 1] = byte((int)uv[x + 1] + (int)(sum >> 21) - 255);
    }
}
