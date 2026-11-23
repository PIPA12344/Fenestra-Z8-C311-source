#ifndef PHOTO_WORKSPACE_H
#define PHOTO_WORKSPACE_H
#include <stdint.h>
#include <stdatomic.h>

enum {
    PHOTO_WORKSPACE_BYTES = 11U * 1024U * 1024U,
    PHOTO_WORKSPACE_MAX_BYTES = 16U * 1024U * 1024U,
    PHOTO_WORKSPACE_PREVIEW_BANK_BYTES = 1280U * 960U * 3U / 2U,
    PHOTO_WORKSPACE_PREVIEW_BANKS = 5U,
    PHOTO_WORKSPACE_PREVIEW_BYTES = (1U + PHOTO_WORKSPACE_PREVIEW_BANKS) *
                                    PHOTO_WORKSPACE_PREVIEW_BANK_BYTES + 4U * 1024U * 1024U,
    PHOTO_WORKSPACE_LCD_DOUBLE_BYTES = 1024U * 682U * 8U +
                                       1024U * 682U * 3U / 2U + 4U * 1024U * 1024U
};
enum PhotoWorkspaceStatus {
    PHOTO_WS_OK=1, PHOTO_WS_ARGUMENT=-1, PHOTO_WS_BUSY=-2,
    PHOTO_WS_MEMORY=-3, PHOTO_WS_MAPPING=-4, PHOTO_WS_RETAINED=-5
};

/* Keep one zero-initialized instance in the trusted native provider. Keep
 * this bookkeeping alive until release returns OK. Do not copy an instance. */
typedef struct {
    _Atomic uint32_t state;
    uint32_t bytes;
    uintptr_t handle, cpu;
} PhotoWorkspace;

typedef struct { uint8_t *cpu; uint32_t bytes; } PhotoWorkspaceView;

/* A normal worker task performs allocation and cache maintenance. */
int photo_workspace_acquire(PhotoWorkspace *ws,uint32_t bytes);
int photo_workspace_pin(PhotoWorkspace *ws,PhotoWorkspaceView *view);
/* Exactly once per successful pin, after the final CPU user of that pin. */
int photo_workspace_unpin(PhotoWorkspace *ws);
/* A normal worker task closes admission. While pins or acquire are in flight,
 * call again after completion. A failed native free is retained for inspection. */
int photo_workspace_release(PhotoWorkspace *ws);
#endif
