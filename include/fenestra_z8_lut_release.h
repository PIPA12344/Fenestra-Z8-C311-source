/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FENESTRA_Z8_LUT_RELEASE_H
#define FENESTRA_Z8_LUT_RELEASE_H

#include <stdint.h>

/* C3.11 release mode, self-timer, and continuous speed data stores. */
#define FX_Z8_LUT_RELEASE_MODE_ADDRESS UINT64_C(0xa03d47adc)
#define FX_Z8_LUT_SELF_TIMER_ADDRESS UINT64_C(0xa03d47ae0)
#define FX_Z8_LUT_CONTINUOUS_MODE_ADDRESS UINT64_C(0xa03d7c7a4)

#ifdef FX_Z8_LUT_SYNTHETIC_RELEASE
uint32_t fx_z8_lut_test_release_read(uintptr_t address);
#endif

static inline uint32_t fx_z8_lut_release_read(uintptr_t address) {
#ifdef FX_Z8_LUT_SYNTHETIC_RELEASE
    return fx_z8_lut_test_release_read(address);
#else
    return *(const volatile uint32_t *)address;
#endif
}

static inline int fx_z8_lut_single_frame(void) {
    return fx_z8_lut_release_read(FX_Z8_LUT_RELEASE_MODE_ADDRESS) == 0u &&
           fx_z8_lut_release_read(FX_Z8_LUT_SELF_TIMER_ADDRESS) == 0u;
}

static inline int fx_z8_lut_jpeg_release(void) {
    if (fx_z8_lut_release_read(FX_Z8_LUT_SELF_TIMER_ADDRESS) != 0u) return 0;
    uint32_t mode = fx_z8_lut_release_read(FX_Z8_LUT_RELEASE_MODE_ADDRESS);
    if (mode == 0u) return 1;  /* S may retain the last continuous speed. */
    /* Stock 0x5013 table at 0x440f4d44 uses DS 0x2126 to select CL or CH. */
    return mode == 1u && fx_z8_lut_release_read(FX_Z8_LUT_CONTINUOUS_MODE_ADDRESS) <= 1u;
}

#endif
