/* Nikon C3.11 LUT helpers. All control flow uses general registers only. */
#include <stddef.h>
#include <stdatomic.h>
#include "smp.h"

#ifdef FX_LUT_PREVIEW_SMP
#define NEON_SENTINEL layer0_neon_sentinel
enum { PRIORITY = 28 };
#else
#define NEON_SENTINEL jpeg_neon_sentinel
enum { PRIORITY = 22 };
#endif
extern unsigned NEON_SENTINEL(int32_t (*delay)(uint32_t));

typedef struct {
    uint32_t attributes, reserved;
    uintptr_t context, entry;
    uint32_t priority, reserved2;
    uint64_t stack_bytes;
    uintptr_t stack;
    uint32_t processor, affinity;
} Task;
_Static_assert(sizeof(Task) == 56 && offsetof(Task, entry) == 16 &&
               offsetof(Task, stack_bytes) == 32 && offsetof(Task, affinity) == 52,
               "C3.11 RTOS create record");

#ifdef FX_JPEG_SMP_TEST
extern unsigned jpeg_smp_test_core(void);
extern int32_t jpeg_smp_test_task_create(const Task *);
extern int32_t jpeg_smp_test_task_resume(int32_t);
extern int32_t jpeg_smp_test_task_delete(int32_t);
extern int32_t jpeg_smp_test_task_delay(uint32_t);
extern void jpeg_smp_test_backoff(void);
extern uint64_t jpeg_smp_test_ticks(void);
extern void jpeg_smp_test_quarantine(unsigned, unsigned, uint64_t);
extern int32_t jpeg_smp_test_sem_create(const uint32_t *);
extern int32_t jpeg_smp_test_sem_delete(int32_t);
extern int32_t jpeg_smp_test_sem_signal(int32_t);
extern int32_t jpeg_smp_test_sem_wait(int32_t);
#define TASK_CREATE jpeg_smp_test_task_create
#define TASK_RESUME jpeg_smp_test_task_resume
#define TASK_DELETE jpeg_smp_test_task_delete
#define TASK_DELAY jpeg_smp_test_task_delay
#define SEM_CREATE jpeg_smp_test_sem_create
#define SEM_DELETE jpeg_smp_test_sem_delete
#define SEM_SIGNAL jpeg_smp_test_sem_signal
#define SEM_WAIT jpeg_smp_test_sem_wait
#else
/* The generated binder is shared with native.c, which validates the prefixes. */
#include "jpeg_lut_foundation.h"
#include "fenestra_z8_photo_lut.h"
#define NATIVE(address, type) ((type)(uintptr_t)(address))
#define TASK_CREATE NATIVE(JPEG_LUT_TASK_CREATE, int32_t (*)(const Task *))
#define TASK_RESUME NATIVE(JPEG_LUT_TASK_RESUME, int32_t (*)(int32_t))
#define TASK_DELETE NATIVE(JPEG_LUT_TASK_DELETE, int32_t (*)(int32_t))
#define TASK_DELAY NATIVE(JPEG_LUT_TASK_DELAY, int32_t (*)(uint32_t))
#define SEM_CREATE NATIVE(JPEG_LUT_SEM_CREATE, int32_t (*)(const uint32_t *))
#define SEM_DELETE NATIVE(JPEG_LUT_SEM_DELETE, int32_t (*)(int32_t))
#define SEM_SIGNAL NATIVE(JPEG_LUT_SEM_SIGNAL, int32_t (*)(int32_t))
#define SEM_WAIT NATIVE(JPEG_LUT_SEM_WAIT, int32_t (*)(int32_t))
#endif

/* Low three bits are state, high bits distinguish consecutive publications. */
enum { DEAD, PARKED, QUEUED, ACTIVE, QUARANTINED, QUARANTINED_DRAINED, PHASE_MASK = 7, NEXT_GENERATION = 8 };
enum { HELPER_COUNT = 2, STACK_BYTES = 8192, CPU_LAST = 3, SEM_ALREADY_SIGNALED = -43 };
typedef struct {
    _Alignas(64) _Atomic uint32_t phase;
    _Atomic unsigned neon;
    uint32_t core, slot;
    int32_t wake;
    JpegSmpJob *job;
    JpegSmpWorker run;
    void *context;
} Helper;

static Helper worker[HELPER_COUNT];
static unsigned initialized, active;
static uint64_t generation;
static JpegSmpJob *current_job;
static FxResult join_result;

static uint64_t smp_ticks(void) {
#ifdef FX_JPEG_SMP_TEST
    return jpeg_smp_test_ticks();
#else
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#endif
}

static uint64_t cancel_window_ticks(void) {
#ifdef FX_LUT_PREVIEW_SMP
    const uint32_t hz_divisor = 2u; /* Rear polls asynchronously for up to 500 ms. */
#else
    const uint32_t hz_divisor = 50u; /* An encoder lane can block only for a bounded drain. */
#endif
#ifdef FX_JPEG_SMP_TEST
    return 24000000u / hz_divisor;
#else
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz / hz_divisor;
#endif
}

unsigned jpeg_smp_core(void) {
#ifdef FX_JPEG_SMP_TEST
    return jpeg_smp_test_core();
#else
    uint64_t value;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
    return (unsigned)(value & 15u) + 1u;
#endif
}

int jpeg_smp_can_schedule(void) {
#ifdef FX_JPEG_SMP_TEST
    return 1;
#else
    uint64_t daif;
    const volatile uint32_t *pcb;
    __asm__ volatile("mrs %0, daif; mrs %1, tpidr_el1" : "=r"(daif), "=r"(pcb));
    /* Match TASK_DELAY's IRQ nesting and dispatch-enabled checks before
     * starting work that could require a scheduling wait to join. */
    return (daif & 0x80u) == 0 && pcb && pcb[0x50 / 4] == 0u && pcb[0x14 / 4] != 0u;
#endif
}

static void worker_task(void *context) {
    Helper *w = context;
    unsigned neon = NEON_SENTINEL(TASK_DELAY);
    atomic_store_explicit(&w->neon, neon, memory_order_release);
    for (;;) {
        if (SEM_WAIT(w->wake)) {
            /* No frame is owned at this point. Park/retry after a transient
             * RTOS rejection. A stopped task leaves its unclaimed job revocable. */
            if (TASK_DELAY(1u)) break;
            continue;
        }
        uint32_t expected = atomic_load_explicit(&w->phase, memory_order_relaxed);
        if ((expected & PHASE_MASK) != QUEUED ||
            !atomic_compare_exchange_strong_explicit(&w->phase, &expected, expected ^ (QUEUED ^ ACTIVE),
                                                      memory_order_acquire, memory_order_relaxed)) continue;
        if (!jpeg_smp_cancelled(w->job) && jpeg_smp_core() == w->core) w->run(w->context, w->slot, neon);
        /* Publish the lease fence only after the callback has returned.
         * A quarantined worker can drain its lease but stays excluded. */
        uint32_t claimed = expected ^ (QUEUED ^ ACTIVE);
        if (!atomic_compare_exchange_strong_explicit(&w->phase, &claimed, expected ^ (QUEUED ^ PARKED),
                                                     memory_order_release, memory_order_relaxed)) {
            atomic_store_explicit(&w->phase, claimed ^ (QUARANTINED ^ QUARANTINED_DRAINED), memory_order_release);
            return;
        }
    }
    atomic_store_explicit(&w->phase, DEAD, memory_order_release);
}

void jpeg_smp_begin(unsigned hook_core) {
    if (initialized || hook_core < 1u || hook_core > CPU_LAST || !jpeg_smp_can_schedule()) return;
    initialized = 1u;
    unsigned next = 0;
    /* Keep both providers' helpers off Core 1. Runtime dispatch excludes the
     * calling core, including when the producer first enters after migration. */
    for (unsigned core = 2; core <= CPU_LAST; ++core) {
        Helper *w = &worker[next++];
        w->slot = next;
        const uint32_t config[3] = {0u, 0u, 1u};
        int32_t wake = SEM_CREATE(config);
        if (wake < 1) continue;
        w->wake = wake;
        w->core = core;
        /* C3.11 allocates and owns a kernel stack when the stack pointer is
         * zero. Published native IMAGE records survive logical disarm, so
         * keep the parked task and semaphore resident for this boot. */
        const Task task = {0, 0, (uintptr_t)w, (uintptr_t)worker_task,
                           PRIORITY, 0, STACK_BYTES, 0, core, 1u << (core - 1u)};
        int32_t id = TASK_CREATE(&task);
        if (id > 0) {
            atomic_store_explicit(&w->phase, PARKED, memory_order_relaxed);
            if (!TASK_RESUME(id)) continue;
            atomic_store_explicit(&w->phase, DEAD, memory_order_release);
            /* The stock delete accepts only dormant dynamic IDs and frees
             * a kernel-owned stack. Retain the semaphore if delete rejects. */
            if (TASK_DELETE(id)) continue;
        }
        (void)SEM_DELETE(wake);
    }
}

static int usable(const Helper *w, unsigned hook_core) {
    return w->core != hook_core && (atomic_load_explicit(&w->phase, memory_order_acquire) & PHASE_MASK) == PARKED;
}

unsigned jpeg_smp_available(unsigned hook_core) {
    if (hook_core < 1u || hook_core > CPU_LAST || !jpeg_smp_can_schedule()) return 0u;
    unsigned count = 0;
    for (unsigned i = 0; i < HELPER_COUNT; ++i) count += (unsigned)usable(&worker[i], hook_core);
    return count;
}

unsigned jpeg_smp_neon_status(unsigned core) {
    for (unsigned i = 0; i < HELPER_COUNT; ++i)
        if (worker[i].core == core) return atomic_load_explicit(&worker[i].neon, memory_order_acquire);
    return 0u;
}

int jpeg_smp_quarantine_drained(void) {
    for (unsigned i = 0; i < HELPER_COUNT; ++i)
        if ((atomic_load_explicit(&worker[i].phase, memory_order_acquire) & PHASE_MASK) == QUARANTINED)
            return 0;
    return 1;
}

int jpeg_smp_dispatch(unsigned hook_core, unsigned helpers, JpegSmpJob *job, JpegSmpWorker run, void *context) {
    current_job = job;
    join_result = FX_OK;
    job->generation = ++generation;
    atomic_store_explicit(&job->cancel_tick, 0, memory_order_relaxed);
    if (!helpers) return 1;
    if (!jpeg_smp_can_schedule()) goto interrupted;
    for (unsigned i = 0; i < HELPER_COUNT; ++i) {
        Helper *w = &worker[i];
        uint32_t state = atomic_load_explicit(&w->phase, memory_order_acquire);
        if (w->core == hook_core || (state & PHASE_MASK) != PARKED) continue;
        w->job = job;
        w->run = run;
        w->context = context;
        if (!atomic_compare_exchange_strong_explicit(&w->phase, &state, state + NEXT_GENERATION + QUEUED - PARKED,
                                                      memory_order_release, memory_order_relaxed)) continue;
        active |= 1u << i;
        int32_t wake = SEM_SIGNAL(w->wake);
        if (wake && wake != SEM_ALREADY_SIGNALED) break;
        if (!--helpers) return 1;
    }
interrupted:
    jpeg_smp_cancel(job);
    return 0;
}

void jpeg_smp_cancel(JpegSmpJob *job) {
    if (jpeg_smp_cancelled(job)) return;
    uint64_t expected = 0, now = smp_ticks();
    atomic_compare_exchange_strong_explicit(&job->cancel_tick, &expected, now ? now : 1,
                                             memory_order_release, memory_order_relaxed);
}

static void join_backoff(void) {
#ifdef FX_JPEG_SMP_TEST
    jpeg_smp_test_backoff();
#else
    uint64_t start, now;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    do __asm__ volatile("yield; mrs %0, cntpct_el0" : "=r"(now));
    while (now - start < 2400u);
#endif
}

static uint32_t revoke_queued(Helper *w, uint32_t state) {
    if ((state & PHASE_MASK) == QUEUED) {
        /* A late semaphore wait can observe a new job only through its
         * corresponding epoch CAS. It cannot revive a revoked frame. */
        uint32_t parked = state ^ (QUEUED ^ PARKED);
        if (atomic_compare_exchange_strong_explicit(&w->phase, &state, parked,
                                                    memory_order_acq_rel, memory_order_acquire)) return parked;
    }
    return state;
}

void jpeg_smp_revoke_queued(void) {
    for (unsigned i = 0; i < HELPER_COUNT; ++i) {
        if (!(active & (1u << i))) continue;
        Helper *w = &worker[i];
        revoke_queued(w, atomic_load_explicit(&w->phase, memory_order_acquire));
    }
}

static void record_quarantine(const Helper *w, const JpegSmpJob *job) {
#ifdef FX_JPEG_SMP_TEST
    jpeg_smp_test_quarantine(w->slot, w->core, job->generation);
#else
#ifdef FX_LUT_PREVIEW_SMP
    const unsigned first = FX_Z8_HB_REAR_HELPER1;
#else
    const unsigned first = FX_Z8_HB_JPEG_HELPER1;
#endif
    fx_z8_photo_flight(FX_Z8_FLIGHT_SMP_QUARANTINE, (uint16_t)jpeg_smp_core(),
                       first + w->slot - 1u, w->core, (uint32_t)job->generation);
#endif
}

FxResult jpeg_smp_poll(int (*abort_requested)(void *), void *context) {
    /* QUEUED can be revoked even if the RTOS never dispatches its wake. An
     * ACTIVE helper has already acquired the frame: cancel its bounded row
     * loop and wait for PARKED. At the hard bound, detach the helper without
     * revoking any outstanding memory ownership. */
    JpegSmpJob *job = current_job;
    if (!job) return FX_STATE;
    if (active) {
        if (abort_requested(context)) jpeg_smp_cancel(job);
        uint64_t cancel_tick = atomic_load_explicit(&job->cancel_tick, memory_order_acquire);
        uint64_t now = cancel_tick ? smp_ticks() : 0;
        int expired = cancel_tick && now >= cancel_tick && now - cancel_tick >= cancel_window_ticks();
        for (unsigned i = 0; i < HELPER_COUNT; ++i) {
            if (!(active & (1u << i))) continue;
            Helper *w = &worker[i];
            uint32_t state = atomic_load_explicit(&w->phase, memory_order_acquire);
            if (cancel_tick) state = revoke_queued(w, state);
            if ((state & PHASE_MASK) == DEAD) jpeg_smp_cancel(job);
            if (expired && (state & PHASE_MASK) == ACTIVE &&
                atomic_compare_exchange_strong_explicit(&w->phase, &state, state ^ (ACTIVE ^ QUARANTINED),
                                                        memory_order_acq_rel, memory_order_acquire)) {
                join_result = FX_TIMEOUT;
                active &= ~(1u << i);
                record_quarantine(w, job);
            }
            if ((state & PHASE_MASK) <= PARKED) active &= ~(1u << i);
        }
    }
    if (active) return FX_BUSY;
    current_job = NULL;
    return join_result == FX_OK && jpeg_smp_cancelled(job) ? FX_INTERRUPTED : join_result;
}

FxResult jpeg_smp_join(int (*abort_requested)(void *), void *context) {
    for (;;) {
        FxResult result = jpeg_smp_poll(abort_requested, context);
        if (result != FX_BUSY) return result;
        /* On delay rejection cancel and poll the quarantine bound with a
         * short backoff so an ACTIVE helper retains its lease. */
        if (TASK_DELAY(1u)) {
            jpeg_smp_cancel(current_job);
            join_backoff();
        }
    }
}
