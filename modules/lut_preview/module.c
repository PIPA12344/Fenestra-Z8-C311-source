/* SPDX-License-Identifier: GPL-3.0-only */
#include "fenestra_sdk.h"

/* Nikon's native Photo Shooting Menu owns selection and worker lifetime. */
int fenestra_module_entry(const FxModuleServices *base) {
    return !base || base->bytes != sizeof(FxServiceModule);
}
