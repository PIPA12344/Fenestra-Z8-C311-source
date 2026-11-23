#ifndef FX_JPEG_LUT_SMP_H
#define FX_JPEG_LUT_SMP_H

#include <stdatomic.h>
#include <stdint.h>
#include "fenestra.h"

typedef void (*JpegSmpWorker)(void *context, unsigned slot, unsigned neon_status);
typedef struct {
    _Atomic uint64_t cancel_tick;
    uint64_t generation;
} JpegSmpJob;

/* Keep row allocation off the cache lines used for grading and telemetry. */
typedef struct {
    _Alignas(64) _Atomic uint32_t next_row;
    uint32_t height, chunk_rows;
} JpegSmpRows;

static inline uint32_t jpeg_smp_chunk_rows(uint32_t height) {
    return height >= 1792u ? 64u : height == 1080u ? 32u : height;
}

/* Called only after the worker's abort check. The dispatch publication
 * already makes the descriptor visible. The RMW only allocates rows. */
static inline int jpeg_smp_claim_rows(JpegSmpRows *job, uint32_t *first, uint32_t *end) {
    uint32_t start = atomic_fetch_add_explicit(&job->next_row, job->chunk_rows, memory_order_relaxed);
    if (start >= job->height) return 0;
    uint32_t limit = start + job->chunk_rows;
    *first = start;
    *end = limit < job->height ? limit : job->height;
    return 1;
}

/* One producer per provider. Helpers acquire their image/cube lease only by
 * claiming a queued job. A quarantined context and its owned resources must
 * remain resident until all late callbacks report that their leases drained. */
unsigned jpeg_smp_core(void);
int jpeg_smp_can_schedule(void);
void jpeg_smp_begin(unsigned hook_core);
unsigned jpeg_smp_available(unsigned hook_core);
unsigned jpeg_smp_neon_status(unsigned core);
int jpeg_smp_quarantine_drained(void);
int jpeg_smp_dispatch(unsigned hook_core, unsigned helpers, JpegSmpJob *job, JpegSmpWorker run, void *context);
void jpeg_smp_cancel(JpegSmpJob *job);
static inline int jpeg_smp_cancelled(const JpegSmpJob *job) {
    return atomic_load_explicit(&job->cancel_tick, memory_order_acquire) != 0;
}
/* All row chunks are claimed. Revoke queued wakes and then join active work. */
void jpeg_smp_revoke_queued(void);
/* FX_INTERRUPTED is a cancelled, fully drained job. FX_TIMEOUT excludes the
 * timed-out helpers permanently and retains their leases until callback exit. */
/* Poll once without scheduling or releasing a live descriptor. FX_BUSY
 * means the caller still owns this job and must poll its completion later. */
FxResult jpeg_smp_poll(int (*abort_requested)(void *), void *context);
FxResult jpeg_smp_join(int (*abort_requested)(void *), void *context);

#endif
