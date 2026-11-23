#include <stddef.h>
#include "xm6.h"
#include "fenestra_memory.h"
#include "jpeg_lut_foundation.h"

typedef struct {
    uint64_t plane[3];
    uint32_t x, y, width, height, y_pitch, uv_pitch, reserved, format;
} Image;
typedef struct {
    Image image;
    uint32_t reserved0, mode;
    uint64_t work;
    uint32_t work_bytes, reserved1;
    int16_t front[12], back[12];
    uint16_t curve[3][256];
} Parameters;
typedef struct {
    uint64_t link;
    uint32_t opcode, reserved0;
    Parameters *in;
    Image *out;
    uint32_t lane, reserved1[3];
    volatile uint32_t status;
    uint32_t tail[3];
} Job;
_Static_assert(sizeof(Image) == 0x38 && sizeof(Parameters) == 0x680 &&
               offsetof(Parameters, front) == 0x50 && offsetof(Parameters, curve) == 0x80 &&
               sizeof(Job) == 0x40 && offsetof(Job, status) == 0x30, "C3.11 RGBLUT ABI");

static int idle(void) {
    const volatile uint32_t *state = (const volatile uint32_t *)(uintptr_t)0xa02e7e734u;
    const volatile uint32_t *lane = (const volatile uint32_t *)(uintptr_t)0xa02e7eed0u;
    return *state == 2 && lane[0] == 0xdeadbeefu && lane[1] == 0x10000000u &&
        lane[2] == 0xdeadbeefu && lane[3] == 0x10000000u;
}

static int mapped_span(uintptr_t start, size_t bytes) {
    const uintptr_t limit = UINT64_C(0x8c0000000);
    if (!bytes || start % 64 || start < UINT64_C(0x840000000) || start >= limit ||
        bytes > limit - start) return 0;
    return (start >> 29) == ((start + bytes - 1) >> 29);
}

static int valid_arena(uintptr_t start) {
    return mapped_span(start, XM6_CONTROL) && mapped_span(start + XM6_CONTROL, XM6_WORK);
}

static int valid_geometry(uint32_t width, uint32_t height, uint32_t pitch) {
    static const uint16_t sizes[][2] = {
        {8256, 5504}, {4128, 2752}, {2688, 1792},
        {1620, 1080}, {640, 424}, {160, 120}
    };
    if (width % 2 || pitch < width || pitch > XM6_MAX_PITCH || pitch % 64 ||
        height > 5504 || height % 8) return 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        if (width == sizes[i][0] && height == sizes[i][1]) return 1;
    return 0;
}

static int overlaps(uintptr_t a, size_t a_bytes, uintptr_t b, size_t b_bytes) {
    return a < b + b_bytes && b < a + a_bytes;
}

static int valid_buffers(uintptr_t arena, const uintptr_t planes[4], size_t plane_bytes) {
    if (!valid_arena(arena)) return 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!mapped_span(planes[i], plane_bytes) ||
            overlaps(planes[i], plane_bytes, arena, XM6_ARENA)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (overlaps(planes[i], plane_bytes, planes[j], plane_bytes)) return 0;
    }
    return 1;
}

static const int16_t front[12] = {1024,0,1436, 1024,-352,-731, 1024,1815,0, 0,0,0};

static Parameters *prepare_parameters(uint8_t *arena) {
    static const int16_t back[12] = {306,601,117, -173,-339,512, 512,-429,-83, 0,0,0};
    if (!valid_arena((uintptr_t)arena)) return NULL;
    fx_ram_zero(arena, XM6_CONTROL);
    Parameters *in = (Parameters *)(void *)(arena + 0x200);
    in->mode = 0x40;
    in->work = (uintptr_t)(arena + XM6_CONTROL);
    memcpy(in->front, front, sizeof(front));
    memcpy(in->back, back, sizeof(back));
    return in;
}

int jpeg_xm6_prepare_curves(uint8_t *arena, const Xm6Curves *curves) {
    Parameters *in = prepare_parameters(arena);
    if (!in) return 0;
    memcpy(in->curve, curves->values, sizeof(in->curve));
    for (unsigned c = 0; c < 3; ++c)
        memcpy(in->front + c * 3, front + curves->source[c] * 3, 3 * sizeof(*front));
    return 1;
}

void jpeg_xm6_prepare(uint8_t *arena) {
    Parameters *in = prepare_parameters(arena);
    if (!in) return;
    for (unsigned i = 0; i < 256; ++i) {
        unsigned s = i < 128 ? i : 128;
        for (unsigned c = 0; c < 3; ++c) in->curve[c][i] = (uint16_t)(2u*s*s);
    }
}

Xm6Result jpeg_xm6_frame(uint8_t *arena, const uint8_t *sy, const uint8_t *sc,
                        uint8_t *dy, uint8_t *dc, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t *status) {
    const uintptr_t planes[] = {(uintptr_t)sy, (uintptr_t)sc, (uintptr_t)dy, (uintptr_t)dc};
    if (!status || (uintptr_t)status % _Alignof(uint32_t) || !valid_geometry(width, height, pitch) ||
        !valid_buffers((uintptr_t)arena, planes, (size_t)pitch * height) || !idle()) return XM6_SKIPPED;
    Job *job = (Job *)(void *)arena;
    Parameters *in = (Parameters *)(void *)(arena + 0x200);
    Image *out = (Image *)(void *)(arena + 0xa00);
    uintptr_t work = (uintptr_t)(arena + XM6_CONTROL);
    if (in->mode != 0x40 || in->work != work) return XM6_SKIPPED;
    /* Match the workspace capacity used for the padded full-frame previews. */
    const uint32_t work_bytes = 0x4b000u + 0x330u * pitch;
    const Image source = {.plane = {planes[0], planes[1], 0}, .width = width, .height = height,
        .y_pitch = pitch, .uv_pitch = pitch, .format = 0x30};
    Image destination = source;
    destination.plane[0] = planes[2];
    destination.plane[1] = planes[3];
    *job = (Job){.opcode = 12, .in = in, .out = out, .lane = 1, .status = 0x72504e44u};
    in->image = source;
    in->work_bytes = work_bytes;
    *out = destination;
    __asm__ volatile("dsb sy" ::: "memory");
    /* Stock lock acquisition argument. The DSP completion deadline belongs to the caller. */
    uint32_t accepted = ((uint32_t (*)(Job *, uint32_t))(uintptr_t)JPEG_LUT_DSP_SUBMIT)(job, 200);
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t result = job->status;
    *status = result;
    if (!(accepted & 1u) || result == 0x72504e44u || !idle() || job->opcode != 12 ||
        job->in != in || job->out != out || job->lane != 1 || in->mode != 0x40 ||
        in->work != work || in->work_bytes != work_bytes ||
        memcmp(&in->image, &source, sizeof(source)) || memcmp(out, &destination, sizeof(destination)))
        return XM6_UNCERTAIN;
    return result == 0 ? XM6_COMPLETE : XM6_SKIPPED;
}
