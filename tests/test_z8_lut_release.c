#include <assert.h>
#include <stdint.h>
#include "fenestra_z8_lut_release.h"

static uint32_t release_mode, timer_mode, continuous_mode, file_type;
static unsigned continuous_reads;

uint32_t fx_z8_lut_test_release_read(uintptr_t address) {
    if (address == UINT64_C(0xa03d47adc))
        return release_mode;
    if (address == UINT64_C(0xa03d7c7a4)) {
        ++continuous_reads;
        return continuous_mode;
    }
    if (address == UINT64_C(0xa03d482c8))
        return file_type;
    assert(address == UINT64_C(0xa03d47ae0));
    return timer_mode;
}

int main(void) {
    static const struct {
        uint32_t mode, timer, speed;
        int jpeg, single;
    } cases[] = {
        {0, 0, 0, 1, 1}, {0, 0, 3, 1, 1}, {0, 0, 4, 1, 1},
        {0, 0, 5, 1, 1}, {0, 0, 6, 1, 1},
        {1, 0, 0, 1, 0}, {1, 0, 1, 1, 0},
        {1, 0, 2, 0, 0}, {1, 0, 3, 0, 0}, {1, 0, 4, 0, 0},
        {1, 0, 5, 0, 0}, {1, 0, 6, 0, 0}, {1, 0, 7, 0, 0},
        {1, 0, UINT32_MAX, 0, 0},
        {4, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {1, 1, 1, 0, 0},
        {2, 0, 0, 0, 0}, {UINT32_MAX, 0, 0, 0, 0},
    };
    /* 0 RAW+JPEG, 1 RAW, 3 JPEG; unknown values keep the release decision. */
    static const uint32_t file_types[] = {0, 1, 2, 3, 4, UINT32_MAX};
    for (unsigned t = 0; t < sizeof(file_types) / sizeof(file_types[0]); ++t) {
        file_type = file_types[t];
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            int raw_only = file_type == 1u;
            release_mode = cases[i].mode;
            timer_mode = cases[i].timer;
            continuous_mode = cases[i].speed;
            continuous_reads = 0;
            assert(fx_z8_lut_single_frame() == cases[i].single);
            assert(continuous_reads == 0);
            assert(fx_z8_lut_jpeg_release() == (raw_only ? 0 : cases[i].jpeg));
            assert(continuous_reads ==
                   (unsigned)(!raw_only && release_mode == 1 && timer_mode == 0));
        }
    }
}
