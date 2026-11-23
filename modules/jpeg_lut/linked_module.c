#include "fenestra_sdk.h"

/* The live-view pack owns the single menu switch and drives native JPEG work. */
int fenestra_module_entry(const FxModuleServices *base) {
    return !base || base->bytes != sizeof(FxServiceModule);
}
