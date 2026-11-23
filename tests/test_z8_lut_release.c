#include <assert.h>
#include <stdint.h>
#include "fenestra_z8_lut_release.h"

static uint32_t release_mode, timer_mode;

uint32_t fx_z8_lut_test_release_read(uintptr_t address) {
    if (address == UINT64_C(0xa03d47adc))
        return release_mode;
    assert(address == UINT64_C(0xa03d47ae0));
    return timer_mode;
}

int main(void) {
    assert(fx_z8_lut_single_frame());
    release_mode = 1u;
    assert(!fx_z8_lut_single_frame());
    release_mode = 0u;
    timer_mode = 1u;
    assert(!fx_z8_lut_single_frame());
    release_mode = UINT32_MAX;
    timer_mode = UINT32_MAX;
    assert(!fx_z8_lut_single_frame());
    release_mode = timer_mode = 0u;
    assert(fx_z8_lut_single_frame());
}
