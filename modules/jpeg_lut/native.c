/* SDR JPEG color transform, including the three stock previews. */
#include <stddef.h>
#include <stdint.h>
#include "cube.h"
#include "cube_frame.h"
#include "smp.h"
#include "xm6.h"
#include "fenestra_memory.h"
#include "fenestra_native_pack.h"
#include "fenestra_native_locale.h"
#include "fenestra_z8_lut_release.h"
#include "fenestra_z8_photo_lut.h"
#include "jpeg_lut_foundation.h"

enum { IDLE, ARMED, RUNNING, APPLIED, SKIPPED, RETAINED, LOADING, PREPARING, CLEANUP, PREVIEWS };
enum { CARD = 1, FORMAT, MEMORY, LAYOUT, ENCODE, CLOSE, TIMING, PAIRING, DSP_ERROR, CUBE_ERROR,
       RELEASE_MODE, SMP_STUCK, FILE_MISSING };
enum { LUT_STATUS = 1, LUT_ARM, LUT_CANCEL, LUT_ARM_XM6, LUT_ARM_FILE, LUT_ARM_LINKED };
enum { DX_CPU, XM6_TEST, XM6_FILE, ARM_3D_FILE };
enum { MAIN, HD, VGA, THUMB, IMAGE_COUNT };
enum { JOB_EMPTY, JOB_READY, JOB_WORKING, JOB_RETURNED };
enum { STAGING_BYTES = 24 * 1024 * 1024, PAD = 512, READ_CHUNK = 32768,
       ROW_BYTES = 4096, BAKE_STEP = 8192, XM6_STAGE_BYTES = 96 * 1024 * 1024,
       XM6_CAPTURE_SECONDS = 2, SNAPSHOT_PLANE = 8256 * 5504, COOP_HZ = 100,
       CAPTURE_ROWS = 32, ARM_GRADE_ROWS = 16, ARM_FALLBACK_HZ = 250, ARM_FALLBACK_DELAY = 3 };
enum { HELPER_YIELD_US = 20000, US_PER_SECOND = 1000000 };
enum { ARM_CACHE_BYTES = CUBE_FRAME_CACHE_WORDS * sizeof(uint32_t) };
enum { LUT_CACHE_SLOTS = 2, LUT_CACHE_VERSION = 1 };
_Static_assert(ARM_CACHE_BYTES <= XM6_ARENA && ARM_CACHE_BYTES % 64 == 0,
               "ARM table must fit in the exclusive aligned workspace");

/* Card reads and capture scratch stay within the original READ_CHUNK bytes. */
typedef struct {
    char active[FX_Z8_LUT_PATH_BYTES];
    char cached[LUT_CACHE_SLOTS][FX_Z8_LUT_PATH_BYTES];
} LutReadPaths;
_Static_assert(sizeof(LutReadPaths) == 192, "read scratch path tail");

typedef struct {
    uint32_t layout, depth;
    uint16_t y_stride, uv_stride, width, height, crop_width, crop_height, crop_x, crop_y;
    uint8_t *y, *uv;
} JpegInput;

/* Cached payloads contain only CPU-owned color data. Image/DSP leases remain
 * under the capture state machine and are never carried between selections. */
typedef struct {
    uint32_t version, file_bytes, kind;
    char drive;
    uint64_t hash;
    JpegCube *cube;
    uint32_t *table;
} PreparedLut;

static PreparedLut lut_cache[LUT_CACHE_SLOTS];
static uint32_t next_cache_slot;

typedef struct {
    _Atomic uint32_t phase, reason;
    uint32_t width, height, grid, encoded;
    uint64_t staging_handle;
    uint8_t *staging;
    JpegCube *cube;
    uint32_t card_uncertain;
    int32_t fd;
    uint32_t used, total, calls;
    char line[CUBE_LINE];
    uint32_t *baked;
    uint32_t bake_pos, final_phase;
    uint64_t transform_ticks;
    uint32_t transform_rows;
    uint32_t applied_mask, next_image, lane;
    uintptr_t capture, preview_owner, metadata;
    uint32_t capture_number;
    _Atomic uint64_t expires;
    JpegInput source[IMAGE_COUNT];
    JpegInput images[IMAGE_COUNT];
    _Alignas(8) uint8_t staged_request[0x50];
    _Atomic uint32_t use_xm6;
    uint32_t arena_bytes, dsp_jobs, dsp_status;
    /* Same-frame mean Y/Cb/Cr before and after processing, from 8x8 windows. */
    uint8_t samples[IMAGE_COUNT][25][2][3];
    uint32_t layout_check, last_image, visits[IMAGE_COUNT], staged_bytes;
    uint64_t layout_capture, layout_input, layout_owner, layout_base;
    uint32_t layout_bytes, layout_kind;
    uint64_t snapshot_handle[2], spacer;
    uint8_t *snapshot[2];
    _Atomic uint32_t job_phase;
    uint32_t job_image;
    Xm6Result job_result;
    uint32_t job_status;
    uint64_t copy_ticks[IMAGE_COUNT], dsp_ticks[IMAGE_COUNT];
    uint16_t sample_xy[IMAGE_COUNT][25][2];
    uint32_t cpu_frames;
    uint64_t cpu_ticks[IMAGE_COUNT];
    uint8_t reference[IMAGE_COUNT][25][3];
    _Atomic uint32_t linked, cancel_requested;
    uint32_t reserved_capture_active;
    _Atomic uint32_t completed;
    uint32_t pool_uncertain;
    uint64_t uncertain_handle;
    uint32_t copy_yields[IMAGE_COUNT], cpu_yields[IMAGE_COUNT];
    _Atomic uint32_t output_enabled;
    uint32_t submit_stock_only;
    uint32_t eager, want_output, generation, cache_hit;
    uint32_t verifying, retry_parse;
    char drive, fd_drive;
    uint32_t fd_epoch;
    uint64_t file_hash;
    PreparedLut *candidate, *prepared;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    _Atomic(const uint32_t *) permit;
    _Atomic uint32_t output_ticket;
    _Atomic uint32_t capture_ticket;
    _Atomic(FxZ8LutIoGate *) io;
    _Atomic uint32_t io_epoch;
#endif
    char *read_scratch;
} JpegLut;

static LutReadPaths *lut_paths(const JpegLut *test) {
    return (LutReadPaths *)(test->read_scratch + READ_CHUNK);
}

__attribute__((used)) const uint32_t fx_z8_jpeg_snapshot_meta[] = {
    1, offsetof(JpegLut, copy_ticks), offsetof(JpegLut, dsp_ticks), offsetof(JpegLut, sample_xy),
    offsetof(JpegLut, job_phase), offsetof(JpegLut, snapshot_handle),
    offsetof(JpegLut, cpu_frames), offsetof(JpegLut, cpu_ticks), offsetof(JpegLut, reference),
    offsetof(JpegLut, copy_yields), offsetof(JpegLut, cpu_yields), offsetof(JpegLut, submit_stock_only)
};

static JpegLut probe = {.fd = -1};
static _Atomic uint32_t online;

enum { JPEG_STAGED = 1u, JPEG_FALLBACK = 2u, JPEG_TIMEOUT = 4u, JPEG_INTERRUPTED = 8u,
       JPEG_DIRECT = 16u, JPEG_ENCODE_FAILED = 32u, JPEG_CONTENTION = 64u, JPEG_QUEUE_FULL = 128u,
       JPEG_STOCK_ONLY = 256u };

typedef struct {
    uint64_t start, end, cache_ticks;
    uint32_t core, neon_test, neon_active, misses, rows;
} JpegStripeStats;

typedef struct {
    uint64_t hook_enter, hook_exit, grade_start, grade_end, join_end;
    uint32_t flags, stripes, capture_number;
    JpegStripeStats worker[3];
} JpegViewStats;

/* Read only after the hook returns. The encoder publishes joined workers. */
__attribute__((used)) struct {
    uint32_t version, bytes, tick_hz, view_bytes;
    JpegViewStats view[IMAGE_COUNT];
} fx_z8_jpeg_phase_telemetry = {1, sizeof(fx_z8_jpeg_phase_telemetry), 24000000, sizeof(JpegViewStats), {{0}}};

_Static_assert(sizeof(JpegStripeStats) == 48 && sizeof(JpegViewStats) == 200, "JPEG telemetry layout");
#ifdef FX_Z8_LUT_SHARED_OUTPUT
static uintptr_t encoder_task;
static unsigned encoder_neon;
static _Atomic unsigned heartbeat_lanes;

enum { BURST_SLOTS = 4, BURST_WAIT_MS = 1200, BURST_BASE_MS = 1500,
       BURST_DEPTH_MS = 500, BURST_MAX_MS = 3500, BURST_OWNED = 1, BURST_WAITING = 1u << 29 };
#define BURST_ABANDONED (UINT32_C(1) << 31)
#define BURST_PREVIEW (UINT32_C(1) << 30)
#define BURST_ORDER_MASK (BURST_WAITING - 1u)
#define BURST_FREE UINT32_MAX
_Static_assert(BURST_BASE_MS + BURST_DEPTH_MS * (BURST_SLOTS - 1u) <= BURST_MAX_MS,
               "table bounds the exposure deadline");

typedef struct {
    uintptr_t owner, preview_owner;
    uint64_t deadline, selection;
    _Atomic uint32_t turn;
    uint32_t number, next_image;
} BurstExposure;

/* The metadata lock is held only for bounded slot scans. Each nonzero turn
 * reserves its slot until that hook releases it. The phase CAS separately
 * protects the output buffer, encoder request, and SMP producer. */
static BurstExposure burst_exposure[BURST_SLOTS];
static _Atomic uint32_t burst_lock;
__attribute__((used, retain, section(".burst_data"))) struct {
    uint32_t version, bytes;
    _Atomic uint32_t contention_passes, fallbacks;
    _Atomic uint64_t wait_ticks;
    _Atomic uint32_t active_exposures;
    _Atomic uint32_t waiting_views;
    _Atomic uint32_t deadline_ms, queue_full;
} fx_z8_jpeg_burst_telemetry = {2, sizeof(fx_z8_jpeg_burst_telemetry), 0, 0, 0, 0, 0, 0, 0};

_Static_assert(sizeof(fx_z8_jpeg_burst_telemetry) == 40, "JPEG burst telemetry layout");
#define BURST_METRICS fx_z8_jpeg_burst_telemetry

__attribute__((noinline, minsize))
static void jpeg_flight(uint16_t event, uint16_t flags, uint32_t a, uint32_t b, uint32_t c) {
    fx_z8_photo_flight(event, (uint16_t)jpeg_smp_core() | flags, a, b, c);
}

/* Share stage-entry setup so all encoder boundaries fit in the fixed RX
 * reservation. The per-chunk progress store stays inline in the grade loop. */
__attribute__((noinline, minsize))
static void jpeg_heartbeat(unsigned slot, unsigned stage, uint32_t capture, uint32_t aux) {
    fx_z8_heartbeat_enter(slot, stage, capture, aux);
}

__attribute__((noinline, minsize))
static void jpeg_heartbeat_progress(unsigned slot) {
    fx_z8_heartbeat_progress(slot);
}

__attribute__((noinline, minsize))
static void jpeg_card_heartbeat(unsigned aux) {
    jpeg_heartbeat(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0, aux);
}

__attribute__((noinline, minsize))
static void jpeg_worker_heartbeat(void) {
    jpeg_heartbeat(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0, Z8_LUT_STEP);
}

__attribute__((noinline, minsize))
static int jpeg_io_take(FxZ8LutIoGate *io, uint32_t epoch) {
    return fx_z8_lut_io_take(io, epoch);
}
#endif
#ifndef FX_Z8_LUT_SHARED_OUTPUT
static _Atomic uint64_t lease;
#endif

static const FxNativeLocale stock_locale = {FX_NATIVE_LOCALE_ABI, sizeof(FxNativeLocale), 36u, 3136u,
    UINT64_C(0x4327c5a0), UINT64_C(0x439daf58)};

_Static_assert(sizeof(JpegInput) == 0x28 && offsetof(JpegInput, y) == 0x18, "stock JPEG layout");

#define NATIVE(address, type) ((type)(uintptr_t)(address))
#define ALLOC NATIVE(0x4104d2dcu, uint64_t (*)(uint32_t, uint32_t, uint32_t))
#define FREE NATIVE(0x4104d600u, uint64_t (*)(uint64_t))
#define HEAP_ALLOC NATIVE(JPEG_LUT_HEAP_ALLOC, void *(*)(uint32_t, uint32_t))
#define HEAP_FREE NATIVE(JPEG_LUT_HEAP_FREE, void (*)(void *))
#define CARD_OPEN NATIVE(JPEG_LUT_CARD_OPEN, int32_t (*)(const char *, uint32_t, uint32_t))
#define CARD_CLOSE NATIVE(JPEG_LUT_CARD_CLOSE, uint32_t (*)(int32_t))
#define CARD_READ NATIVE(JPEG_LUT_CARD_READ, uint32_t (*)(int32_t, void *, uint32_t, uint32_t *))
#define CARD_DRIVE NATIVE(JPEG_LUT_CARD_DRIVE, char (*)(void))
#define JPEG NATIVE(0x40e82090u, uint32_t (*)(uint32_t, const uint8_t *))
#define CPU_VIEW NATIVE(JPEG_LUT_CPU_IMAGE_VIEW, const uint8_t *(*)(const uint8_t *))
#define INVALIDATE NATIVE(JPEG_LUT_IMAGE_INVALIDATE, void (*)(const void *, uint32_t, uint32_t))
#define CLEAN NATIVE(JPEG_LUT_IMAGE_CLEAN, void (*)(const void *, uint32_t, uint32_t))
#define CV_CLEAN NATIVE(JPEG_LUT_IMAGE_CV_CLEAN, void (*)(const void *, uint32_t, uint32_t))
#define IMAGE_COPY NATIVE(JPEG_LUT_IMAGE_COPY, void *(*)(void *, const void *, size_t))
#define TASK_DELAY NATIVE(JPEG_LUT_TASK_DELAY, int32_t (*)(uint32_t))

static uint64_t get64(const void *p) {
    uint64_t result;
    memcpy(&result, p, sizeof(result));
    return result;
}

#ifndef FX_Z8_LUT_SHARED_OUTPUT
static uint32_t get32(const void *p) {
    uint32_t result;
    memcpy(&result, p, sizeof(result));
    return result;
}
#endif

/* These reads are for stock objects whose alignment has been checked. The
 * request descriptor still uses the unaligned-safe reader. */
typedef uint64_t __attribute__((may_alias)) NativeU64;
typedef uint32_t __attribute__((may_alias)) NativeU32;
static uint64_t native_u64(const void *p) { return *(const NativeU64 *)p; }
static uint32_t native_u32(const void *p) { return *(const NativeU32 *)p; }

__attribute__((noinline, minsize))
static int native_object(uint64_t address) {
    return !(address & 7u) && address >= 0xa00000000u && address <= 0xa40000000u - 0x80u;
}

__attribute__((noinline, minsize))
static int uncached_span(uint64_t address, uint64_t size) {
    return size && size <= 0x20000000u && address >= 0x840000000u &&
           address <= 0xa00000000u - size;
}

static int release_handle(uint64_t *handle) {
    if (*handle && FREE(*handle) != 1u) return 0;
    *handle = 0;
    return 1;
}

static uint64_t counter(void);

static uint64_t frequency(void) {
    uint64_t value;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(value));
    return value;
}

static int yield_due(uint64_t *next, uint64_t interval, uint64_t now) {
    if (now < *next) return 0;
    if (TASK_DELAY(1u)) return 1;
    *next = counter() + interval;
    return 0;
}

typedef struct { _Alignas(64) JpegStripeStats value; } GradeStripeStats;
_Static_assert(sizeof(GradeStripeStats) == 64, "independent worker cache lines");

typedef struct {
    uint64_t deadline, next, interval;
    uint32_t delay, yields, interrupted;
} CaptureSlice;

static CaptureSlice capture_slice(uint64_t start, uint64_t deadline) {
    /* Leave half of the 20 ms CPU budget for a bounded operation between
     * clock checks, including a stock cache or copy call. */
    uint64_t interval = frequency() / COOP_HZ;
    return (CaptureSlice){.deadline=deadline, .next=start+interval, .interval=interval, .delay=1u};
}

#ifdef FX_Z8_LUT_SHARED_OUTPUT
static int permitted(const JpegLut *test, uint32_t open) {
    const uint32_t *permit = atomic_load_explicit(&test->permit, memory_order_relaxed);
    uint32_t ticket = atomic_load_explicit(&test->output_ticket, memory_order_relaxed);
    return permit && __atomic_load_n(permit, __ATOMIC_ACQUIRE) == ((ticket & ~1u) | open);
}

static int io_current(const JpegLut *test) {
    const FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
    uint32_t epoch = atomic_load_explicit(&test->io_epoch, memory_order_relaxed);
    return epoch && io && fx_z8_lut_io_epoch(io) == epoch;
}

static int capture_admitted(const JpegLut *test) {
    /* The claimed encoder generation owns its cube and staging surfaces until
     * its last preview (or shutdown). A preference can only gate a new MAIN. */
    uint32_t ticket = atomic_load_explicit(&test->capture_ticket, memory_order_acquire);
    return ticket && ticket == atomic_load_explicit(&test->output_ticket, memory_order_relaxed) &&
           atomic_load_explicit(&online, memory_order_acquire) && !test->cancel_requested && io_current(test);
}

static uint32_t active_views(const JpegLut *test) {
    const FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
    return io ? fx_z8_lut_frame_count(io) : 0;
}

static void burst_reap(JpegLut *test);
#endif

static int capture_ready(CaptureSlice *slice) {
    if (slice->interrupted) return 0;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    if (!capture_admitted(&probe)) return 0;
#endif
    uint64_t now = counter();
    if (now >= slice->deadline) return 0;
    if (now < slice->next) return 1;
    if (TASK_DELAY(slice->delay)) {
        slice->interrupted = 1;
        return 0;
    }
    slice->next = counter() + slice->interval;
    ++slice->yields;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    if (!capture_admitted(&probe)) return 0;
#endif
    return counter() < slice->deadline;
}

/* A pool allocation may straddle two DSP translation windows. Set it aside
 * while the allocator supplies another surface; no device sees that address. */
static int acquire_surface(JpegLut *test, uint32_t size, uint64_t *owned, uint8_t **data) {
    uint64_t handle = ALLOC(0, 0x6b, size + 3u * PAD);
    if (!handle) return 0;
    test->uncertain_handle = handle;
    if (!native_object(handle)) goto uncertain;
    const uint8_t *record = (const uint8_t *)(uintptr_t)handle;
    uint64_t owner = native_u64(record + 8);
    if (!native_object(owner) || native_u32(record + 0x10) != 0x24) goto uncertain;
    const uint8_t *allocation = (const uint8_t *)(uintptr_t)owner;
    uint64_t base = native_u64(allocation);
    uint32_t bytes = native_u32(allocation + 8);
    if (get64(allocation + 0x10) != handle || !uncached_span(base, bytes) ||
        base + bytes > 0x8c0000000u) goto uncertain;
    *owned = handle;
    test->uncertain_handle = 0;
    uint64_t aligned = (base + 2u * PAD - 1u) & ~(uint64_t)(PAD - 1u);
    if (aligned + size + PAD > base + bytes) return 0;
    if (test->use_xm6 != ARM_3D_FILE && (aligned >> 29) != ((aligned + size - 1u) >> 29)) {
        if (test->spacer || !test->use_xm6) return 0;
        test->spacer = handle;
        *owned = 0;
        return acquire_surface(test, size, owned, data);
    }
    *data = (uint8_t *)(uintptr_t)aligned;
    return 1;
uncertain:
    test->pool_uncertain = 1;
    return 0;
}

static int acquire_snapshots(JpegLut *test) {
    for (unsigned i = 0; i < 2; ++i)
        if (!acquire_surface(test, SNAPSHOT_PLANE, &test->snapshot_handle[i], &test->snapshot[i])) return 0;
    uintptr_t a = (uintptr_t)test->snapshot[0], b = (uintptr_t)test->snapshot[1];
    uintptr_t output = (uintptr_t)test->staging;
    if ((a < b + SNAPSHOT_PLANE && b < a + SNAPSHOT_PLANE) ||
        (a < output + test->arena_bytes && output < a + SNAPSHOT_PLANE) ||
        (b < output + test->arena_bytes && output < b + SNAPSHOT_PLANE)) {
        test->pool_uncertain = 1;
        return 0;
    }
    return 1;
}

static int acquire_staging(JpegLut *test) {
    test->arena_bytes = test->use_xm6 == ARM_3D_FILE ? XM6_STAGE_BYTES + ARM_CACHE_BYTES :
                        test->use_xm6 ? XM6_STAGE_BYTES + XM6_ARENA : STAGING_BYTES;
    return acquire_surface(test, test->arena_bytes, &test->staging_handle, &test->staging);
}

static int close_cube(JpegLut *test) {
    if (test->fd < 0) return 1;
    if (test->card_uncertain) return 0;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
    jpeg_card_heartbeat(FX_Z8_HB_CARD_DRIVE);
    /* The gate can be closed by Early OFF while this descriptor still owns
     * its original epoch. A different mount can reuse the descriptor number. */
    int owned = CARD_DRIVE() == test->fd_drive && test->fd_epoch &&
                (__atomic_load_n(&io->state, __ATOMIC_ACQUIRE) >> 32) == test->fd_epoch;
    if (owned) jpeg_card_heartbeat(FX_Z8_HB_CARD_CLOSE);
#else
    int owned = CARD_DRIVE() == test->fd_drive;
#endif
    int closed = owned && !CARD_CLOSE(test->fd);
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    fx_z8_lut_io_drop(io);
    jpeg_worker_heartbeat();
#endif
    if (closed) {
        test->fd = -1;
        return 1;
    }
    test->card_uncertain = 1;
    test->reason = CLOSE;
    return 0;
}

static void clear_cached_lut(PreparedLut *entry) {
    if (entry->cube) HEAP_FREE(entry->cube);
    if (entry->table) HEAP_FREE(entry->table);
    *entry = (PreparedLut){0};
}

static void release_capture(JpegLut *test) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    atomic_store_explicit(&test->capture_ticket, 0, memory_order_release);
#else
    (void)test;
#endif
}

static int clean_private_table(uint32_t *table) {
    const uint8_t *base = (const uint8_t *)table;
    const uint32_t chunk = 256u * 1024u;
    uint64_t interval = frequency() / COOP_HZ, next = counter() + interval;
    for (uint32_t offset = 0; offset < ARM_CACHE_BYTES; offset += chunk) {
        CLEAN(base + offset, chunk, 0);
        if (yield_due(&next, interval, counter())) return 0;
        INVALIDATE(base + offset, chunk, 0);
        if (yield_due(&next, interval, counter())) return 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}

static uint32_t finish(JpegLut *test, uint32_t phase) {
    (void)close_cube(test);
    if (test->cube && !test->prepared) HEAP_FREE(test->cube);
    test->cube = NULL;
    if (test->baked && (!test->prepared || test->baked != test->prepared->table)) {
        if (test->use_xm6 == ARM_3D_FILE) {
            if (!clean_private_table(test->baked)) test->pool_uncertain = 1;
        } else HEAP_FREE(test->baked);
    }
    test->baked = NULL;
    if (test->prepared && !test->prepared->version) clear_cached_lut(test->prepared);
    test->prepared = test->candidate = NULL;
    atomic_store_explicit(&test->output_enabled, 0, memory_order_release);
    int released = !test->pool_uncertain;
    if (released) {
        released = release_handle(&test->staging_handle);
        for (unsigned i = 0; i < 2; ++i) {
            if (!release_handle(&test->snapshot_handle[i])) released = 0;
            else test->snapshot[i] = NULL;
        }
        if (!release_handle(&test->spacer)) released = 0;
    }
    if (!released) {
        test->reason = MEMORY;
        phase = RETAINED;
    } else {
        test->staging_handle = 0;
        test->staging = NULL;
    }
    if (test->card_uncertain) phase = RETAINED;
    release_capture(test);
    atomic_store_explicit(&test->phase, phase, memory_order_release);
    return phase;
}

static uint32_t open_cube(JpegLut *test) {
    const LutReadPaths *paths = lut_paths(test);
    char path[12 + FX_Z8_LUT_PATH_BYTES] = "A:\\FENESTRA\\";
    memcpy(path + 12, paths->active, sizeof(paths->active));
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    path[0] = test->drive;
#else
    path[0] = CARD_DRIVE();
#endif
    if (path[0] != 'A' && path[0] != 'B') return CARD;
    if (test->eager) {
        if (test->retry_parse && path[0] != test->drive) return CARD;
        test->drive = path[0];
        test->file_hash = UINT64_C(14695981039346656037);
        test->candidate = NULL;
        for (unsigned i = 0; i < LUT_CACHE_SLOTS; ++i) {
            PreparedLut *entry = &lut_cache[i];
            if (entry->version == LUT_CACHE_VERSION && entry->drive == test->drive &&
                !memcmp(paths->cached[i], paths->active, sizeof(paths->active))) test->candidate = entry;
        }
        test->verifying = test->candidate && !test->retry_parse;
    }
    /* Verification streams the file through the owned read buffer and only
     * allocates a parsed CUBE when content must be parsed. */
    if (!test->verifying && !test->cube) {
        uint32_t bytes = (uint32_t)sizeof(*test->cube) + (test->eager ? (uint32_t)sizeof(Xm6Curves) : 0u);
        test->cube = HEAP_ALLOC(bytes, 0);
        if (!test->cube) return MEMORY;
    }
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
    uint32_t epoch = atomic_load_explicit(&test->io_epoch, memory_order_relaxed);
    if (!jpeg_io_take(io, epoch)) return CARD;
    jpeg_card_heartbeat(FX_Z8_HB_CARD_OPEN);
#endif
    uint32_t fd = (uint32_t)CARD_OPEN(path, 0, 0);
    int opened = (fd >> 16) <= 1u && (fd & 0xffffu) <= 0x1du;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    jpeg_worker_heartbeat();
    if (!opened) fx_z8_lut_io_drop(io);
    test->fd_epoch = epoch;
#endif
    if (!opened) return fd == UINT32_C(0x80000010) ? FILE_MISSING : CARD;
    /* Pin the original mount admission until the descriptor's owner closes. */
    test->fd = (int32_t)fd;
    test->fd_drive = path[0];
    if (!test->verifying) jpeg_cube_init(test->cube);
    return 0;
}

static uint32_t read_cube(JpegLut *test) {
    char *buffer = test->read_scratch;
    uint32_t done = 0;
    if (++test->calls > CUBE_MAX_BYTES / READ_CHUNK + 64u) return CARD;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
    if (!jpeg_io_take(io, test->fd_epoch)) return CARD;
    jpeg_card_heartbeat(FX_Z8_HB_CARD_READ);
#endif
    uint32_t result = CARD_READ(test->fd, buffer, READ_CHUNK, &done);
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    fx_z8_lut_io_drop(io);
    jpeg_worker_heartbeat();
#endif
    if (result || done > READ_CHUNK) return CARD;
    if (done > CUBE_MAX_BYTES - test->total) return FORMAT;
    uint32_t offset = test->total;
    test->total += done;
    uint64_t interval = frequency() / COOP_HZ, next = counter() + interval;
    for (uint32_t i = 0; i < done; ++i) {
        if (!(i & 255u) && yield_due(&next, interval, counter())) return TIMING;
        if (test->eager) {
            test->file_hash = (test->file_hash ^ (uint8_t)buffer[i]) * UINT64_C(1099511628211);
            if (test->verifying) continue;
        }
        if (buffer[i] != '\n') {
            if (!buffer[i] || test->used == sizeof(test->line) - 1u) return FORMAT;
            test->line[test->used++] = buffer[i];
            continue;
        }
        test->line[test->used] = 0;
        char *line = test->line;
        if (offset + i == test->used && test->used >= 3 && (uint8_t)line[0] == 0xef &&
            (uint8_t)line[1] == 0xbb && (uint8_t)line[2] == 0xbf) line += 3;
        if (!jpeg_cube_line(test->cube, line)) return FORMAT;
        test->used = 0;
    }
    if (done) return 0;
    if (test->verifying) return close_cube(test) ? 0u : CLOSE;
    test->line[test->used] = 0;
    if ((test->used && !jpeg_cube_line(test->cube, test->line)) || !jpeg_cube_finish(test->cube)) return FORMAT;
    return close_cube(test) ? 0u : CLOSE;
}

static FxResult disarm(JpegLut *test) {
    atomic_store_explicit(&test->cancel_requested, 1, memory_order_release);
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    burst_reap(test);
    if (active_views(test)) return FX_BUSY;
#endif
    if (phase == RUNNING || phase == PREVIEWS) return FX_BUSY;
    if (phase == RETAINED) return FX_CLOSE;
    if (phase != ARMED && phase != LOADING && phase != PREPARING && phase != CLEANUP) return FX_OK;
    if (!atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) return FX_BUSY;
    test->reason = 0;
    return finish(test, IDLE) == RETAINED ? FX_CLOSE : FX_OK;
}

static void reset_capture(JpegLut *test) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    atomic_store_explicit(&test->capture_ticket, 0, memory_order_release);
#endif
    test->width = test->height = test->encoded = 0;
    test->next_image = test->capture_number = test->lane = 0;
    test->capture = test->preview_owner = test->metadata = 0;
    atomic_store_explicit(&test->expires, 0, memory_order_relaxed);
    test->transform_ticks = 0;
    test->transform_rows = 0;
    test->applied_mask = 0;
    test->dsp_jobs = test->dsp_status = test->cpu_frames = 0;
    atomic_store_explicit(&test->job_phase, JOB_EMPTY, memory_order_relaxed);
    memset((uint8_t *)test + offsetof(JpegLut, copy_ticks), 0,
           sizeof(test->copy_ticks) + sizeof(test->dsp_ticks));
    memset(test->cpu_ticks, 0, sizeof(test->cpu_ticks));
    memset((uint8_t *)test + offsetof(JpegLut, copy_yields), 0,
           sizeof(test->copy_yields) + sizeof(test->cpu_yields));
    test->layout_check = test->staged_bytes = 0;
    memset(test->visits, 0, sizeof(test->visits));
}

static FxResult arm_named(JpegLut *test, uint32_t xm6, uint32_t linked, const char *name,
                          uint32_t eager, uint32_t enabled, const FxZ8LutHeldPath *held) {
    if (!atomic_load_explicit(&online, memory_order_acquire)) return FX_STATE;
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
    if (phase == RETAINED) return FX_CLOSE;
    if (phase == ARMED || phase == RUNNING || phase == LOADING || phase == PREPARING || phase == CLEANUP || phase == PREVIEWS ||
        !atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) return FX_BUSY;
    if (xm6 != XM6_TEST) {
        if (!test->read_scratch) test->read_scratch = HEAP_ALLOC(READ_CHUNK + sizeof(LutReadPaths), 0);
        if (!test->read_scratch) {
            atomic_store_explicit(&test->phase, phase, memory_order_release);
            return FX_BUDGET;
        }
        char *active = lut_paths(test)->active;
        memset(active, 0, FX_Z8_LUT_PATH_BYTES);
        while (*name) *active++ = *name++;
    }
    test->reason = 0;
    atomic_store_explicit(&test->cancel_requested, 0, memory_order_release);
    test->linked = linked;
    test->completed = 0;
    test->grid = 0;
    test->used = test->total = test->calls = 0;
    test->bake_pos = 0;
    test->use_xm6 = xm6;
    test->eager = eager;
    test->want_output = enabled;
    test->verifying = test->retry_parse = test->cache_hit = 0;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    atomic_store_explicit(&test->permit, held->output.permit, memory_order_relaxed);
    atomic_store_explicit(&test->output_ticket, held->output.ticket, memory_order_relaxed);
    atomic_store_explicit(&test->io, held->io, memory_order_relaxed);
    atomic_store_explicit(&test->io_epoch, held->io_epoch, memory_order_relaxed);
    test->drive = held->drive;
#else
    (void)held;
#endif
    if (eager && !++test->generation) ++test->generation;
    atomic_store_explicit(&test->output_enabled, eager ? 0 : enabled, memory_order_release);
    reset_capture(test);
    memset(test->samples, 0, sizeof(test->samples));
    atomic_store_explicit(&test->phase, xm6 == XM6_TEST ? PREPARING : LOADING, memory_order_release);
    if (!atomic_load_explicit(&online, memory_order_acquire)) {
        (void)disarm(test);
        return FX_STATE;
    }
    return FX_OK;
}

#ifndef FX_Z8_LUT_SHARED_OUTPUT
static FxResult arm(JpegLut *test, uint32_t xm6, uint32_t linked) {
    static const char default_name[FX_Z8_LUT_NAME_BYTES] = FX_Z8_LUT_DEFAULT;
    return arm_named(test, xm6, linked, default_name, 0, 1, NULL);
}
#endif

static uint64_t counter(void) {
    uint64_t value;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(value));
    return value;
}

static const uint8_t *cached_plane(const uint8_t *source, uint32_t bytes) {
    uintptr_t address = (uintptr_t)source;
    if ((address | bytes) & 63u || !uncached_span(address, bytes)) return NULL;
    const uint8_t *view = CPU_VIEW(source);
    if ((uintptr_t)view != address + UINT64_C(0x400000000) ||
        (uintptr_t)CPU_VIEW(source + bytes - 1u) != (uintptr_t)view + bytes - 1u) return NULL;
    return view;
}

static uint8_t *private_plane(const uint8_t *source, uint32_t bytes) {
    const uint8_t *view = cached_plane(source, bytes);
    if (view) INVALIDATE(view, bytes, 0);
    return (uint8_t *)view;
}

typedef struct {
    const JpegCube *cube;
    uint32_t *table;
    const uint8_t *sy, *sc;
    uint8_t *dy, *dc;
    CaptureSlice slice;
    uint32_t pitch, width, image;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    JpegSmpJob smp;
    _Atomic uint32_t completed_rows;
    uint32_t heartbeat, capture;
#endif
    JpegSmpRows work;
    GradeStripeStats worker[3];
} GradeFrame;

static int grade_abort(void *context) {
    const GradeFrame *frame = context;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    return !capture_admitted(&probe) || counter() >= frame->slice.deadline;
#else
    return counter() >= frame->slice.deadline || probe.cancel_requested ||
           !atomic_load_explicit(&online, memory_order_acquire);
#endif
}

__attribute__((noinline, minsize))
static int grade_stopped(void *context) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    if (jpeg_smp_cancelled(&((const GradeFrame *)context)->smp)) return 1;
#endif
    return grade_abort(context);
}

#ifdef FX_Z8_LUT_SHARED_OUTPUT
/* A queued wake owns no rows. Revoke it once every band is claimed, even
 * when the encoder did not act as a grading worker. Active leases still
 * have to return before the stock input or private output can be reused. */
static int grade_join_abort(void *context) {
    const GradeFrame *frame = context;
    if (atomic_load_explicit(&frame->work.next_row, memory_order_relaxed) >= frame->work.height)
        jpeg_smp_revoke_queued();
    return grade_abort(context);
}

#endif

static void grade_sample(GradeFrame *frame, CaptureSlice *slice);

static int grade_ready(GradeFrame *frame, CaptureSlice *slice) {
    if (grade_stopped(frame)) return 0;
    if (!slice) return 1;
    if (counter() < slice->next) return 1;
    if (!capture_ready(slice)) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
        jpeg_smp_cancel(&frame->smp);
#endif
        return 0;
    }
    return !grade_stopped(frame);
}

static void grade_worker(void *context, unsigned slot, unsigned neon_test) {
    GradeFrame *f = context;
    JpegStripeStats *stats = &f->worker[slot].value;
    uint32_t pitch = f->pitch;
    uint64_t cache_ticks = 0;
    uint32_t misses = 0, rows = 0, start, end;
    CaptureSlice *slice = &f->slice;
    CaptureSlice *chunk_slice = slice;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    CaptureSlice helper;
    unsigned heartbeat = slot ? FX_Z8_HB_JPEG_HELPER1 + slot - 1u : f->heartbeat;
    stats->core = jpeg_smp_core();
    stats->neon_test = neon_test;
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    unsigned use_neon = neon_test == 1u && (cpacr & (UINT64_C(1) << 20));
    stats->neon_active = use_neon;
    uint32_t (*grade)(const JpegCube *, uint32_t *, const uint8_t *, const uint8_t *,
                      uint8_t *, uint8_t *, unsigned) =
        use_neon ? jpeg_cube_cache_row_neon : jpeg_cube_cache_row_scalar;
    if (slot) {
        uint64_t interval = frequency() / (US_PER_SECOND / HELPER_YIELD_US);
        /* Each pinned helper owns its clock. Start small views with a delay
         * even if the preceding wake never blocked on the semaphore. */
        helper = (CaptureSlice){.deadline = slice->deadline, .interval = interval, .delay = 1u,
                                .next = f->image >= VGA ? 0 : counter() + interval};
        chunk_slice = &helper;
        slice = NULL; /* Inside a helper chunk, poll cancellation only. */
    }
    jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_GRADE, f->capture, 0);
#else
    (void)neon_test;
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    stats->core = (uint32_t)(mpidr & 15u) + 1u;
    const __typeof__(&jpeg_cube_cache_row_scalar) grade = jpeg_cube_cache_row_scalar;
#endif
    while (!grade_stopped(context) && jpeg_smp_claim_rows(&f->work, &start, &end)) {
        if (!rows) stats->start = counter();
        uint32_t row = start;
        while (row < end && grade_ready(f, chunk_slice)) {
            uint32_t first = row * pitch, limit = row + ARM_GRADE_ROWS;
            if (limit > end) limit = end;
            uint32_t length = (limit - row) * pitch;
            uint64_t stamp = counter();
            /* Nikon's 0x40d2b9e4 is CIVAC. Keep any stock producer's dirty data
             * and remove stale cached lines before loading via the CPU alias.
             * Cancellation is polled between at most 16-row cache calls and
             * individual row kernels. A call already entered cannot be revoked. */
#ifdef FX_Z8_LUT_SHARED_OUTPUT
            jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_CACHE_MAINT, f->capture, row);
#endif
            CLEAN(f->sy + first, length, 0);
            cache_ticks += counter() - stamp;
            if (!grade_ready(f, slice)) break;
            stamp = counter();
            CLEAN(f->sc + first, length, 0);
            cache_ticks += counter() - stamp;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
            jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_GRADE, f->capture, row);
#endif
            for (; row < limit && grade_ready(f, slice); ++row) {
                uint32_t offset = row * pitch, width = f->width;
                const uint8_t *sy = f->sy + offset, *sc = f->sc + offset;
                uint8_t *dy = f->dy + offset, *dc = f->dc + offset;
                misses += grade(f->cube, f->table, sy, sc, dy, dc, width);
                if (width != pitch) {
                    memcpy(dy + width, sy + width, pitch - width);
                    memcpy(dc + width, sc + width, pitch - width);
                }
            }
            length = row * pitch - first;
            int stopped = !grade_ready(f, slice);
            if (length) {
                stamp = counter();
                /* Discard an abandoned dirty band before its private buffer can
                 * be recycled. Only completed, still-admitted bands need CVAC. */
#ifdef FX_Z8_LUT_SHARED_OUTPUT
                jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_CACHE_MAINT, f->capture, row);
#endif
                void (*maintain)(const void *, uint32_t, uint32_t) = stopped ? INVALIDATE : CV_CLEAN;
                maintain(f->dy + first, length, 0);
                cache_ticks += counter() - stamp;
                if (stopped || !grade_ready(f, slice)) {
                    stopped = 1;
                    maintain = INVALIDATE;
                }
                stamp = counter();
                maintain(f->dc + first, length, 0);
                __asm__ volatile("dsb sy" ::: "memory");
                cache_ticks += counter() - stamp;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
                jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_GRADE, f->capture, row);
#endif
            }
            if (stopped) break;
        }
        rows += row - start;
        if (row != end) break;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
        jpeg_heartbeat_progress(heartbeat);
#endif
    }
    stats->cache_ticks = cache_ticks;
    stats->misses = misses;
    stats->rows = rows;
    stats->end = counter();
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    /* The final worker owns sampling after all row stores and cache fences.
     * Core 1 cannot inspect pixels in offload mode. */
    if (atomic_fetch_add_explicit(&f->completed_rows, rows, memory_order_acq_rel) + rows == f->work.height)
        grade_sample(f, chunk_slice);
    if (slot) jpeg_heartbeat(heartbeat, FX_Z8_STAGE_IDLE, 0, 0);
#else
    if (rows == f->work.height) grade_sample(f, slice);
#endif
}

#ifdef FX_Z8_LUT_SHARED_OUTPUT
static unsigned hook_neon(void) {
    const uintptr_t *pcb;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(pcb));
    /* The stock delay routine loads the current TCB from PCB offset zero. */
    uintptr_t task = pcb ? *pcb : 0;
    if (task && task != encoder_task) {
        encoder_neon = jpeg_neon_sentinel(TASK_DELAY);
        encoder_task = task;
    }
    return task ? encoder_neon : 0;
}
#endif

/* Only XM6 keeps the existing asynchronous private-snapshot handoff. */
__attribute__((used, noinline)) void fx_z8_jpeg_lut_snapshot_work(void) {
    JpegLut *test = &probe;
    uint32_t expected = JOB_READY;
    if (!atomic_compare_exchange_strong(&test->job_phase, &expected, JOB_WORKING)) return;
    unsigned image = test->job_image;
    JpegInput *out = &test->images[image];
    uint64_t start = counter();
    test->job_result = XM6_SKIPPED;
    test->job_status = 0;
    uint64_t deadline = atomic_load_explicit(&test->expires, memory_order_relaxed);
    if (start < deadline
#ifdef FX_Z8_LUT_SHARED_OUTPUT
        && capture_admitted(test)
#endif
        ) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
        jpeg_heartbeat(FX_Z8_HB_WORKER, FX_Z8_STAGE_JPEG_XM6, test->capture_number, image);
#endif
        test->job_result = jpeg_xm6_frame(test->staging + XM6_STAGE_BYTES,
            test->snapshot[0], test->snapshot[1], out->y, out->uv,
            out->width, out->height, out->y_stride, &test->job_status);
        test->dsp_ticks[image] = counter() - start;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    jpeg_worker_heartbeat();
#endif
    }
    atomic_store_explicit(&test->job_phase, JOB_RETURNED, memory_order_release);
}

#ifndef FX_Z8_LUT_SHARED_OUTPUT
static int cube_mode_blocked(const JpegLut *test) {
    /* XM6_TEST is the built-in diagnostic curve rather than a selected .cube. */
    return atomic_load_explicit(&test->use_xm6, memory_order_relaxed) != XM6_TEST &&
           !fx_z8_lut_single_frame();
}

static void pause_cube_capture(JpegLut *test) {
    /* Only the RUNNING owner can abandon a sequence. Earlier successful
     * submits have returned. Keep the prepared table and its workspace. */
    test->reason = RELEASE_MODE;
    release_capture(test);
    if (!test->cancel_requested && atomic_load_explicit(&online, memory_order_acquire)) {
        atomic_store_explicit(&test->phase, ARMED, memory_order_release);
    } else {
        test->final_phase = IDLE;
        atomic_store_explicit(&test->phase, CLEANUP, memory_order_release);
    }
}
#endif

static void reap_capture(JpegLut *test) {
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
    if (phase == RETAINED) {
        uint32_t reason = test->reason;
        if (reason == TIMING && atomic_load_explicit(&test->job_phase, memory_order_acquire) == JOB_RETURNED &&
            test->job_result != XM6_UNCERTAIN && atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) {
            (void)finish(test, SKIPPED);
            return;
        }
#ifdef FX_Z8_LUT_SHARED_OUTPUT
        if (reason == SMP_STUCK && jpeg_smp_quarantine_drained() &&
            atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) {
            fx_z8_lut_helpers_drop(atomic_load_explicit(&test->io, memory_order_relaxed),
                                   FX_Z8_LUT_HELPER_JPEG);
            phase = ARMED;
            if (test->cancel_requested || !io_current(test)) {
                test->final_phase = IDLE;
                phase = CLEANUP;
            }
            atomic_store_explicit(&test->phase, phase, memory_order_release);
        }
#endif
    }
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    burst_reap(test);
#else
    if (phase == PREVIEWS && cube_mode_blocked(test) &&
        atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) {
        pause_cube_capture(test);
        return;
    }
    if (phase == PREVIEWS && counter() >= test->expires &&
        atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) {
        if (counter() < test->expires) {
            atomic_store_explicit(&test->phase, PREVIEWS, memory_order_release);
            return;
        }
        test->reason = test->use_xm6 ? TIMING : PAIRING;
        (void)finish(test, SKIPPED);
    }
#endif
}

static Xm6Curves *cached_curves(JpegCube *cube) {
    _Static_assert(_Alignof(JpegCube) >= _Alignof(Xm6Curves), "cached curves alignment");
    return (Xm6Curves *)(void *)(cube + 1);
}

static uint32_t warm_slice(JpegLut *test) {
    uint64_t start = counter(), interval = frequency() / COOP_HZ;
    uint32_t end = test->bake_pos + BAKE_STEP;
    while (test->bake_pos < end && !test->cancel_requested) {
        jpeg_cube_cache_warm(test->cube, test->baked, test->bake_pos, test->bake_pos + 256);
        test->bake_pos += 256;
        if (counter() - start >= interval) break;
    }
    return test->bake_pos == CUBE_FRAME_CACHE_WORDS;
}

static uint32_t advance_eager(JpegLut *test, uint32_t *phase) {
    if (*phase == LOADING) {
        if (test->fd < 0) return open_cube(test);
        uint32_t failure = read_cube(test);
        if (failure || test->fd >= 0) return failure;
        if (test->verifying) {
            PreparedLut *entry = test->candidate;
            if (entry->file_bytes != test->total || entry->hash != test->file_hash) {
                test->retry_parse = 1;
                test->total = test->used = test->calls = 0;
                return 0;
            }
            test->cube = entry->cube;
            test->prepared = entry;
            test->use_xm6 = entry->kind;
            test->baked = entry->table;
            test->bake_pos = entry->table ? CUBE_FRAME_CACHE_WORDS : 0;
            test->cache_hit = 1;
            next_cache_slot = (uint32_t)(entry - lut_cache) ^ 1u;
        }
        test->grid = test->cube->grid;
        *phase = PREPARING;
        return 0;
    }

    if (!test->prepared) {
        PreparedLut *entry = test->candidate ? test->candidate : &lut_cache[next_cache_slot];
        clear_cached_lut(entry);
        next_cache_slot = (uint32_t)(entry - lut_cache) ^ 1u;
        *entry = (PreparedLut){.file_bytes = test->total, .drive = test->drive,
                              .hash = test->file_hash, .cube = test->cube};
        LutReadPaths *paths = lut_paths(test);
        memcpy(paths->cached[entry - lut_cache], paths->active, sizeof(paths->active));
        test->prepared = entry;
        Xm6Curves *curves = cached_curves(test->cube);
        if (!jpeg_cube_compile_curves(test->cube, curves->values, curves->source)) {
            test->use_xm6 = ARM_3D_FILE;
            entry->table = HEAP_ALLOC(ARM_CACHE_BYTES, 0);
            test->baked = entry->table;
        }
        entry->kind = test->use_xm6;
        return 0;
    }

    PreparedLut *entry = test->prepared;
    if (test->baked && test->bake_pos < CUBE_FRAME_CACHE_WORDS && !warm_slice(test)) return 0;
    entry->version = LUT_CACHE_VERSION;
    if (test->want_output && !test->staging) {
        if (!acquire_staging(test)) return MEMORY;
        uint8_t *arena = test->staging + XM6_STAGE_BYTES;
        if (test->use_xm6 == XM6_FILE) {
            if (!jpeg_xm6_prepare_curves(arena, cached_curves(test->cube)) || !acquire_snapshots(test)) return MEMORY;
        } else if (!test->baked) {
            /* Under heap pressure preserve the original private-pool path. */
            test->baked = (uint32_t *)(void *)private_plane(arena, ARM_CACHE_BYTES);
            if (!test->baked) return MEMORY;
        }
        if (!release_handle(&test->spacer)) {
            test->pool_uncertain = 1;
            return MEMORY;
        }
    }
    if (test->baked && test->bake_pos < CUBE_FRAME_CACHE_WORDS) return 0;
    atomic_store_explicit(&test->output_enabled, test->want_output, memory_order_release);
    *phase = ARMED;
    return 0;
}

/* Run one bounded filesystem operation per worker request. The menu can
 * cancel between chunks, and no RTOS debug invocation performs file I/O. */
static void advance(JpegLut *test) {
    fx_z8_jpeg_lut_snapshot_work();
    reap_capture(test);
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
    if (phase != LOADING && phase != PREPARING && phase != CLEANUP) return;
    if (!atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) return;
    if (phase == CLEANUP) { (void)finish(test, test->final_phase); return; }
    if (!atomic_load_explicit(&online, memory_order_acquire) || test->cancel_requested
#ifdef FX_Z8_LUT_SHARED_OUTPUT
        || !io_current(test) || !permitted(test, 0)
#endif
        ) {
        (void)finish(test, IDLE);
        return;
    }
    uint32_t failure = 0;
    if (test->eager) {
        failure = advance_eager(test, &phase);
    } else if (test->use_xm6 != XM6_TEST && !test->cube) {
        failure = open_cube(test);
    } else if (test->fd >= 0) {
        failure = read_cube(test);
    } else if (test->use_xm6 == ARM_3D_FILE && test->baked) {
        uint64_t interval = frequency() / COOP_HZ, next = counter() + interval;
        uint32_t end = test->bake_pos + BAKE_STEP;
        while (test->bake_pos < end && !test->cancel_requested) {
            jpeg_cube_cache_warm(test->cube, test->baked, test->bake_pos, test->bake_pos + 256);
            test->bake_pos += 256;
            if (yield_due(&next, interval, counter())) { failure = TIMING; break; }
        }
        if (test->bake_pos == CUBE_FRAME_CACHE_WORDS) phase = ARMED;
    } else if (test->use_xm6) {
        Xm6Curves curves;
        if (test->use_xm6 == XM6_FILE &&
                !jpeg_cube_compile_curves(test->cube, curves.values, curves.source))
            test->use_xm6 = ARM_3D_FILE;
        if (acquire_staging(test)) {
            uint8_t *arena = test->staging + XM6_STAGE_BYTES;
            if (test->use_xm6 == XM6_TEST) {
                if (!acquire_snapshots(test)) failure = MEMORY;
                else { jpeg_xm6_prepare(arena); phase = ARMED; }
            } else {
                test->grid = test->cube->grid;
                if (test->use_xm6 == XM6_FILE) {
                    if (!jpeg_xm6_prepare_curves(arena, &curves) || !acquire_snapshots(test))
                        failure = MEMORY;
                    else phase = ARMED;
                }
                else {
                    /* A cross-channel file sends no DSP job. Its table
                     * occupies the unused workspace in our image lease. */
                    test->baked = (uint32_t *)(void *)private_plane(arena, ARM_CACHE_BYTES);
                    if (!test->baked) failure = MEMORY;
                    else phase = PREPARING;
                }
            }
            if (!failure && !release_handle(&test->spacer)) {
                test->pool_uncertain = 1;
                failure = MEMORY;
            }
        } else failure = MEMORY;
    } else if (!test->baked) {
        test->baked = HEAP_ALLOC(CUBE_YUV_ENTRIES * (uint32_t)sizeof(*test->baked) + 2u * ROW_BYTES, 0);
        if (!test->baked) failure = MEMORY;
        phase = PREPARING;
    } else if (test->bake_pos < CUBE_YUV_ENTRIES) {
        jpeg_cube_bake(test->cube, test->baked, test->bake_pos, test->bake_pos + BAKE_STEP);
        test->bake_pos += BAKE_STEP;
    } else if (acquire_staging(test)) {
        test->grid = test->cube->grid;
        HEAP_FREE(test->cube);
        test->cube = NULL;
        phase = ARMED;
    } else {
        failure = MEMORY;
    }
    if (failure) {
        test->reason = failure;
        finish(test, SKIPPED);
        return;
    }
    atomic_store_explicit(&test->phase, phase, memory_order_release);
    if (!atomic_load_explicit(&online, memory_order_acquire) || test->cancel_requested) (void)disarm(test);
}

uint64_t fx_z8_jpeg_lut_control(uint64_t command) {
    if (command == 0) return (uintptr_t)&probe;
#ifndef FX_Z8_LUT_SHARED_OUTPUT
    if (command == LUT_STATUS) { advance(&probe); return atomic_load(&probe.phase); }
    if (command == LUT_ARM_XM6) return arm(&probe, XM6_TEST, 0);
    if (command == LUT_ARM_FILE) return arm(&probe, XM6_FILE, 0);
    if (command == LUT_ARM_LINKED) return arm(&probe, ARM_3D_FILE, 1);
    if (command == LUT_CANCEL) return disarm(&probe);
#endif
    return 0;
}

static int source_image(const JpegInput *in, uint64_t base, uint32_t bytes) {
    uint32_t w = in->crop_width, h = in->crop_height;
    if (in->layout != 1 || in->depth != 0 || !w || !h || (w | in->crop_x) & 1u ||
        w > XM6_MAX_PITCH || in->height > 8192u || w + in->crop_x > in->width ||
        h + in->crop_y > in->height || in->width > in->y_stride || in->width > in->uv_stride ||
        in->y_stride > 16384u || in->uv_stride > 16384u) return 0;
    uint64_t y = (uintptr_t)in->y, uv = (uintptr_t)in->uv;
    uint64_t y_size = (uint64_t)in->y_stride * in->height;
    uint64_t uv_size = (uint64_t)in->uv_stride * in->height;
    return y >= base && uncached_span(y, y_size) && uncached_span(uv, uv_size) &&
           y + y_size <= uv && uv + uv_size <= base + bytes;
}

static const uint8_t input_offset[IMAGE_COUNT] = {0x30, 0x48, 0x20, 0x70};

#ifndef FX_Z8_LUT_SHARED_OUTPUT
static int owner_span(const JpegLut *test, uintptr_t handle, uint64_t *base, uint32_t *bytes) {
    if (!native_object(handle)) return 0;
    uint64_t owner = get64((const uint8_t *)handle + 8);
    if (!native_object(owner)) return 0;
    *base = get64((const void *)owner);
    *bytes = get32((const uint8_t *)owner + 8);
    uint64_t staging = (uintptr_t)test->staging;
    return get64((const uint8_t *)owner + 0x10) == handle && uncached_span(*base, *bytes) &&
           (staging >= *base + *bytes || *base >= staging + test->arena_bytes);
}

static int capture_inputs(JpegLut *test, const uint8_t *request, uintptr_t capture) {
    if (!native_object(capture) || capture > 0xa40000000u - 0x798u) return 0;
    const uint8_t *context = (const uint8_t *)capture;
    uintptr_t main_owner = get64(context + 0x688);
    uintptr_t preview_owner = get64(context + 0x6a0);
    uintptr_t metadata = get64(context + 0x790);
    if (!native_object(preview_owner) || preview_owner > 0xa40000000u - 0xa0u ||
        get32((const uint8_t *)preview_owner + 0x10) != 0x34 ||
        !native_object(metadata) || metadata > 0xa40000000u - 0x250cu ||
        get64(request + 8) != main_owner + input_offset[MAIN]) return 0;
    uint64_t main_base, preview_base;
    uint32_t main_bytes, preview_bytes;
    if (!owner_span(test, main_owner, &main_base, &main_bytes) ||
        !owner_span(test, preview_owner, &preview_base, &preview_bytes) ||
        (main_base < preview_base + preview_bytes && preview_base < main_base + main_bytes)) return 0;
    for (unsigned i = 0; i < IMAGE_COUNT; ++i) {
        uintptr_t handle = i == MAIN ? main_owner : preview_owner;
        const JpegInput *in = (const JpegInput *)(handle + input_offset[i]);
        uint64_t base = i == MAIN ? main_base : preview_base;
        uint32_t bytes = i == MAIN ? main_bytes : preview_bytes;
        if (!source_image(in, base, bytes)) return 0;
        test->source[i] = *in;
    }

    /* The card-based CPU path retains its traced DX S geometry. */
    const JpegInput *main = &test->source[MAIN], *hd = &test->source[HD];
    const JpegInput *vga = &test->source[VGA], *thumb = &test->source[THUMB];
    int dx = main->crop_width == 2688 && main->crop_height == 1792;
    int fx = main->crop_width == 8256 && main->crop_height == 5504;
    if (!(dx || (test->use_xm6 && fx)) ||
        hd->crop_width != 1620 || hd->crop_height != 1080 ||
        vga->crop_width != 640 || vga->crop_height != 424 ||
        thumb->width != 160 || thumb->height != 120 || thumb->crop_width != 160 ||
        thumb->crop_height != 120 || thumb->crop_x || thumb->crop_y ||
        (uintptr_t)vga->uv + (uint64_t)vga->uv_stride * vga->height > (uintptr_t)hd->y ||
        (uintptr_t)hd->uv + (uint64_t)hd->uv_stride * hd->height > (uintptr_t)thumb->y) return 0;
    test->capture = capture;
    test->preview_owner = preview_owner;
    test->metadata = metadata;
    test->capture_number = get32((const uint8_t *)metadata + 0x2508);
    test->width = main->crop_width;
    test->height = main->crop_height;
    return 1;
}

static int stage_images(JpegLut *test, uint64_t start, uint64_t budget) {
    CaptureSlice slice = capture_slice(start, start + budget);
    uint32_t offset = 0;
    uint8_t *row_y = (uint8_t *)(void *)(test->baked + CUBE_YUV_ENTRIES);
    uint8_t *row_uv = row_y + ROW_BYTES;
    for (unsigned i = 0; i < IMAGE_COUNT; ++i) {
        const JpegInput *in = &test->source[i];
        JpegInput *out = &test->images[i];
        uint32_t width = in->crop_width, height = in->crop_height;
        uint32_t stride = (width + 63u) & ~63u;
        uint32_t plane = stride * height;
        offset = (offset + PAD - 1u) & ~(PAD - 1u);
        if (offset + 2u * plane > STAGING_BYTES) return 0;
        *out = (JpegInput){.layout = 1, .y_stride = (uint16_t)stride, .uv_stride = (uint16_t)stride,
            .width = (uint16_t)width, .height = (uint16_t)height,
            .crop_width = (uint16_t)width, .crop_height = (uint16_t)height,
            .y = test->staging + offset, .uv = test->staging + offset + plane};
        offset += 2u * plane;
        for (uint32_t row = 0; row < height; ++row) {
            if (!capture_ready(&slice) || test->cancel_requested) return 0;
            uint32_t sy = (row + in->crop_y) * in->y_stride + in->crop_x;
            uint32_t suv = (row + in->crop_y) * in->uv_stride + in->crop_x;
            fx_ram_copy(row_y, in->y + sy, width);
            fx_ram_copy(row_uv, in->uv + suv, width);
            fx_ram_zero(row_y + width, stride - width);
            fx_ram_zero(row_uv + width, stride - width);
            if (i != THUMB || (row >= 7u && row < 113u))
                jpeg_cube_apply(test->baked, row_y, row_uv, width);
            fx_ram_copy(out->y + row * stride, row_y, stride);
            fx_ram_copy(out->uv + row * stride, row_uv, stride);
            if (i == MAIN) test->transform_rows = row + 1u;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return counter() - start < budget;
}

#endif

static void average_patch(const JpegInput *in, unsigned x, unsigned y, uint8_t out[3]) {
    unsigned sy = 0, cb = 0, cr = 0;
    for (unsigned row = 0; row < 8; ++row) {
        const uint8_t *luma = in->y + (in->crop_y + y + row) * in->y_stride + in->crop_x + x;
        const uint8_t *uv = in->uv + (in->crop_y + y + row) * in->uv_stride + in->crop_x + x;
        for (unsigned i = 0; i < 8; i += 2) {
            sy += (unsigned)luma[i] + luma[i+1];
            cb += uv[i];
            cr += uv[i+1];
        }
    }
    out[0] = (uint8_t)((sy+32u)/64u);
    out[1] = (uint8_t)((cb+16u)/32u);
    out[2] = (uint8_t)((cr+16u)/32u);
}

static void reference_patch(const JpegCube *cube, const JpegInput *snapshot,
                            unsigned x, unsigned y, uint8_t out[3]) {
    uint8_t luma[64], chroma[64];
    JpegInput tile = {.y=luma, .uv=chroma, .y_stride=8, .uv_stride=8};
    for (unsigned r = 0; r < 8; ++r) {
        memcpy(luma + 8u*r, snapshot->y + (y+r)*snapshot->y_stride + x, 8);
        memcpy(chroma + 8u*r, snapshot->uv + (y+r)*snapshot->uv_stride + x, 8);
        jpeg_cube_422(cube, luma + 8u*r, chroma + 8u*r, 8);
    }
    average_patch(&tile, 0, 0, out);
}

/* Keep the borders and center. In each of 16 interior tiles use one distinct
 * chromatic patch to check the two axes even in small, letterboxed previews. */
static void sample_positions(const JpegInput *im, uint16_t xy[25][2]) {
    unsigned w = im->width - 8u, h = im->height - 8u;
    for (unsigned i = 0; i < 9; ++i) {
        xy[i][0] = (uint16_t)((w*(i%3u)/2u) & ~1u);
        xy[i][1] = (uint16_t)(h*(i/3u)/2u);
    }
    for (unsigned tile = 0; tile < 16; ++tile) {
        int best = -1;
        for (unsigned n = 0; n < 9; ++n) {
            unsigned x = (w*(6u*(tile%4u)+2u*(n%3u)+1u)/24u) & ~1u;
            unsigned y = h*(6u*(tile/4u)+2u*(n/3u)+1u)/24u;
            const uint8_t *pair = im->uv + (y+4u)*im->uv_stride + x+4u;
            int c = (int)pair[(tile+(tile/4u))&1u]-128;
            int magnitude = c*c;
            if (magnitude <= best) continue;
            best = magnitude;
            xy[9u+tile][0] = (uint16_t)x;
            xy[9u+tile][1] = (uint16_t)y;
        }
    }
}

/* Synchronize the borrowed producer view without discarding dirty data.
 * DSP always receives the separate, uncached pool snapshot. */
static FxResult snapshot_plane(uint8_t *dest, const uint8_t *source, uint32_t bytes,
                               uint32_t pitch, CaptureSlice *slice, unsigned heartbeat) {
#ifndef FX_Z8_LUT_SHARED_OUTPUT
    (void)heartbeat;
#endif
    const uint8_t *view = cached_plane(source, bytes);
    if (!view) return FX_BOUNDS;
    for (uint32_t offset=0; offset<bytes;) {
        if (!capture_ready(slice)) return FX_TIMEOUT;
        uint32_t count = bytes-offset, chunk = CAPTURE_ROWS*pitch;
        if (count > chunk) count = chunk;
        CLEAN(view+offset, count, 0);
        if (!capture_ready(slice)) return FX_TIMEOUT;
        if (IMAGE_COPY(dest+offset, view+offset, count) != dest+offset) return FX_IO;
        offset += count;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
        jpeg_heartbeat_progress(heartbeat);
#endif
    }
    return capture_ready(slice) ? FX_OK : FX_TIMEOUT;
}

static void grade_sample(GradeFrame *frame, CaptureSlice *slice) {
    JpegLut *test = &probe;
    unsigned image = frame->image;
    const JpegInput *in = &test->source[image], *out = &test->images[image];
    if (grade_stopped(frame)) return;
    JpegInput borrowed = *out;
    borrowed.y = in->y + in->crop_y * frame->pitch;
    borrowed.uv = in->uv + in->crop_y * frame->pitch;
    sample_positions(&borrowed, test->sample_xy[image]);
    for (unsigned i = 0; i < 25; ++i) {
        if (!grade_ready(frame, slice)) return;
        unsigned x = test->sample_xy[image][i][0], row = test->sample_xy[image][i][1];
        average_patch(&borrowed, x, row, test->samples[image][i][0]);
        if (grade_stopped(frame)) return;
        reference_patch(test->cube, &borrowed, x, row, test->reference[image][i]);
        average_patch(out, x, row, test->samples[image][i][1]);
    }
}

#ifdef FX_Z8_LUT_SHARED_OUTPUT
static int take_grade_helpers(const JpegLut *test, FxZ8LutIoGate *io, const GradeFrame *frame) {
    const uint64_t until = counter() + frequency() * 12u / 1000u;
    const uint32_t epoch = atomic_load_explicit(&test->io_epoch, memory_order_relaxed);
    /* The admitted frame lease bars new rear dispatch. Only a frame already
     * using the helper pair can delay this one arena owner. */
    do {
        if (fx_z8_lut_helpers_take(io, epoch, FX_Z8_LUT_HELPER_JPEG)) return 1;
        if (TASK_DELAY(1u)) return 0;
    } while (counter() < until && !grade_abort((void *)frame));
    return 0;
}
#endif

static Xm6Result stage_arm(JpegLut *test, unsigned image, const JpegInput *in, JpegInput *out,
                           uint32_t plane, uint64_t deadline, unsigned heartbeat) {
    JpegViewStats *stats = &fx_z8_jpeg_phase_telemetry.view[image];
    stats->flags |= JPEG_DIRECT;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    /* Once armed, the owner can reuse its card-read allocation for fixed
     * job storage. No card slice can touch it while the encoder owns RUNNING,
     * and a quarantined job keeps this allocation until its callback returns. */
    _Static_assert(sizeof(GradeFrame) + 63u <= READ_CHUNK, "persistent grade job fits card scratch");
    GradeFrame *frame = (GradeFrame *)(((uintptr_t)test->read_scratch + 63u) & ~(uintptr_t)63u);
    if (!frame) return XM6_SKIPPED;
#else
    (void)heartbeat;
    GradeFrame local;
    GradeFrame *frame = &local;
#endif
    memset(frame, 0, sizeof(*frame));
    frame->cube = test->cube;
    frame->table = test->baked;
    frame->slice.deadline = deadline;
    frame->pitch = out->y_stride;
    frame->width = out->width;
    frame->image = image;
    frame->work.height = out->height;
    frame->work.chunk_rows = jpeg_smp_chunk_rows(out->height);
    const uint8_t *source_y = in->y + in->crop_y * frame->pitch;
    const uint8_t *source_uv = in->uv + in->crop_y * frame->pitch;
    frame->sy = cached_plane(source_y, plane);
    frame->sc = cached_plane(source_uv, plane);
    frame->dy = (uint8_t *)cached_plane(out->y, plane);
    frame->dc = (uint8_t *)cached_plane(out->uv, plane);
    if (!frame->sy || !frame->sc || !frame->dy || !frame->dc) return XM6_SKIPPED;
    unsigned helpers = 0, offload = 0;
    int dispatched = 1;
    FxResult joined = FX_OK;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    FxZ8LutIoGate *helpers_io = NULL;
    frame->heartbeat = heartbeat;
    frame->capture = test->capture_number;
    jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_SMP_DISPATCH, frame->capture, image);
    if (!jpeg_smp_can_schedule() || grade_abort(frame)) goto interrupted;
    /* CH keeps Core 1 available even for the two small preview encoders. */
    unsigned wanted = image <= HD ? 2u : !fx_z8_lut_single_frame();
    if (wanted) {
        FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
        if (!take_grade_helpers(test, io, frame)) {
            if (!grade_abort(frame)) stats->flags |= JPEG_CONTENTION;
            goto interrupted;
        }
        helpers_io = io;
        jpeg_smp_begin(jpeg_smp_core());
    }
    /* MAIN and the small serial previews can follow a preceding view on
     * the same stock task. Give other Core 1 work a scheduling turn. */
    unsigned core = jpeg_smp_core();
    if (image != HD && core == 1u) {
        if (TASK_DELAY(1u)) goto interrupted;
        core = jpeg_smp_core();
    }
    helpers = wanted ? jpeg_smp_available(core) : 0;
    if (helpers > wanted) helpers = wanted;
    unsigned neon = 0;
    if (core != 1u || !wanted || helpers != wanted) {
        neon = hook_neon();
        /* The sentinel yields on the first view on this stock task. */
        core = jpeg_smp_core();
        helpers = wanted ? jpeg_smp_available(core) : 0;
        if (helpers > wanted) helpers = wanted;
    }
    if (grade_abort(frame)) goto interrupted;
    offload = core == 1u && wanted && helpers == wanted;
    /* If the hook has to grade, leave margin for a bounded cache band or
     * row between clock checks, including after a scheduling migration. */
    frame->slice.interval = frequency() / ARM_FALLBACK_HZ;
    frame->slice.delay = ARM_FALLBACK_DELAY;
#else
    frame->slice = capture_slice(counter(), deadline);
#endif
    stats->stripes = helpers + 1u - offload;
    stats->grade_start = counter();
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    frame->slice.next = stats->grade_start + frame->slice.interval;
    dispatched = jpeg_smp_dispatch(core, helpers, &frame->smp, grade_worker, frame);
    if (dispatched && !offload) grade_worker(frame, 0, neon);
    jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_SMP_JOIN, frame->capture, helpers);
    joined = jpeg_smp_join(grade_join_abort, frame);
    if (helpers_io && joined != FX_TIMEOUT) fx_z8_lut_helpers_drop(helpers_io, FX_Z8_LUT_HELPER_JPEG);
#else
    grade_worker(frame, 0, 0);
#endif
    stats->join_end = counter();
    test->cpu_ticks[image] = stats->join_end - stats->grade_start;
    test->cpu_yields[image] = frame->slice.yields;
    if (joined == FX_TIMEOUT) {
        test->reason = SMP_STUCK;
        return XM6_UNCERTAIN;
    }
    uint32_t rows = 0;
    for (unsigned i = 0; i < 3u; ++i) {
        stats->worker[i] = frame->worker[i].value;
        if (stats->worker[i].end > stats->grade_end) stats->grade_end = stats->worker[i].end;
        rows += stats->worker[i].rows;
    }
    if (counter() >= deadline) test->reason = TIMING;
    if (!dispatched || joined != FX_OK || rows != out->height || grade_abort(frame)) return XM6_SKIPPED;

    ++test->cpu_frames;
    if (image == MAIN) test->transform_rows = out->height;
    test->staged_bytes += 2u * plane;
    return XM6_COMPLETE;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
interrupted:
    if (helpers_io) fx_z8_lut_helpers_drop(helpers_io, FX_Z8_LUT_HELPER_JPEG);
    if (counter() >= deadline) test->reason = TIMING;
    return XM6_SKIPPED;
#endif
}

__attribute__((noinline, minsize))
static Xm6Result stage_xm6(JpegLut *test, unsigned image, uint64_t deadline, unsigned heartbeat) {
    const JpegInput *in = &test->source[image];
    uint16_t w = in->crop_width, h = in->crop_height, pitch = in->y_stride;
    uint32_t plane = (uint32_t)pitch * h;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    static const uint32_t view_offset[IMAGE_COUNT] = {
        0, 2u * SNAPSHOT_PLANE, 2u * (SNAPSHOT_PLANE + 1664u * 1080u),
        2u * (SNAPSHOT_PLANE + 1664u * 1080u + 640u * 424u)
    };
    uint32_t offset = view_offset[image];
#else
    uint32_t offset = test->staged_bytes;
#endif
    if (in->crop_x || pitch != in->uv_stride || pitch != ((w + 63u) & ~63u) ||
        !plane || plane > SNAPSHOT_PLANE || offset > XM6_STAGE_BYTES ||
        2u * plane > XM6_STAGE_BYTES - offset) return XM6_SKIPPED;
    JpegInput *out = &test->images[image];
    *out = (JpegInput){.layout=1, .y_stride=pitch, .uv_stride=pitch, .width=w, .height=h,
        .crop_width=w, .crop_height=h, .y=test->staging+offset, .uv=test->staging+offset+plane};
    if (test->use_xm6 == ARM_3D_FILE) return stage_arm(test, image, in, out, plane, deadline, heartbeat);
    uint64_t start = counter();
    CaptureSlice slice = capture_slice(start, deadline);
    const uint8_t *planes[2] = {in->y, in->uv};
    JpegInput copy = *out;
    copy.y = test->snapshot[0];
    copy.uv = test->snapshot[1];
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_CACHE_MAINT, test->capture_number, image);
#endif
    for (unsigned i = 0; i < 2; ++i) {
        FxResult result = snapshot_plane(i ? copy.uv : copy.y, planes[i]+in->crop_y*pitch, plane, pitch, &slice, heartbeat);
        test->copy_yields[image] = slice.yields;
        if (result != FX_OK) {
            if (result == FX_TIMEOUT) test->reason = TIMING;
            return XM6_SKIPPED;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    test->copy_ticks[image] = counter() - start;
    sample_positions(&copy, test->sample_xy[image]);
    for (unsigned i = 0; i < 25; ++i) {
        unsigned x = test->sample_xy[image][i][0], row = test->sample_xy[image][i][1];
        uint8_t check[3];
        average_patch(in, x, row, check);
        average_patch(&copy, x, row, test->samples[image][i][0]);
        if (memcmp(check,test->samples[image][i][0],3)) return XM6_SKIPPED;
        if (test->cube) reference_patch(test->cube, &copy, x, row, test->reference[image][i]);
    }
    if (counter() >= deadline) { test->reason = TIMING; return XM6_SKIPPED; }
    test->job_image = image;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_XM6, test->capture_number, image);
#endif
    atomic_store_explicit(&test->job_phase, JOB_READY, memory_order_release);
    while (atomic_load_explicit(&test->job_phase, memory_order_acquire) != JOB_RETURNED) {
        if (counter() >= deadline || TASK_DELAY(1)) {
            uint32_t ready = JOB_READY;
            test->reason = TIMING;
            if (atomic_compare_exchange_strong(&test->job_phase, &ready, JOB_EMPTY)) return XM6_SKIPPED;
            if (ready != JOB_RETURNED) return XM6_UNCERTAIN;
            break;
        }
    }
    test->dsp_status = test->job_status;
    if (test->job_result == XM6_UNCERTAIN) return XM6_UNCERTAIN;
    atomic_store_explicit(&test->job_phase, JOB_EMPTY, memory_order_release);
    if (test->job_result == XM6_COMPLETE) {
        ++test->dsp_jobs;
        if (image == MAIN) test->transform_rows = h;
    }
    if (counter() >= deadline) { test->reason = TIMING; return XM6_SKIPPED; }
    if (test->job_result != XM6_COMPLETE) return XM6_SKIPPED;
    for (unsigned i = 0; i < 25; ++i) {
        unsigned x = test->sample_xy[image][i][0], row = test->sample_xy[image][i][1];
        average_patch(out, x, row, test->samples[image][i][1]);
    }
    test->staged_bytes = offset + 2u*plane;
    if (counter() >= deadline) { test->reason = TIMING; return XM6_SKIPPED; }
    return XM6_COMPLETE;
}

/* Stock encoder calls carry their own input view. The main is ready earlier
 * than its sibling previews, so only borrow the current view for this call. */
static int capture_metadata_readable(uintptr_t metadata) {
    const uintptr_t bytes = 0x250cu, alias = UINT64_C(0x400000000);
    if (native_object(metadata) && metadata <= UINT64_C(0xa40000000) - bytes) return 1;
    /* The FX L capture context also stores this record in its cached CPU view.
     * Match both ends to the stock mapping before reading its capture number. */
    if ((metadata & 7u) || metadata < alias || !uncached_span(metadata-alias, bytes)) return 0;
    return (uintptr_t)CPU_VIEW((const void *)(metadata-alias)) == metadata &&
           (uintptr_t)CPU_VIEW((const void *)(metadata-alias+bytes-1u)) == metadata+bytes-1u;
}

__attribute__((noinline, minsize))
static int current_xm6_input(JpegLut *test, unsigned image, uintptr_t capture, const uint8_t *request) {
    uint64_t view = get64(request+8), handle = view - input_offset[image];
    test->layout_capture = capture;
    test->layout_input = view;
    test->layout_check = 1;
    if (!native_object(handle) || handle > 0xa40000000u-0x98u) return 0;
    const uint8_t *ctx = (const uint8_t *)capture;
    const uint8_t *record = (const uint8_t *)(uintptr_t)handle;
    test->layout_check = 2;
#ifndef FX_Z8_LUT_SHARED_OUTPUT
    if (!native_object(capture) || capture > UINT64_C(0xa40000000)-0x798u) return 0;
#endif
    if (native_u64(ctx + (image == MAIN ? 0x688 : 0x6a0)) != handle) return 0;
    uintptr_t metadata = native_u64(ctx+0x790);
    test->layout_check = 3;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    /* Match the already validated record from hook entry after any wait,
     * including MAIN, before dereferencing it or borrowing a descriptor. */
    if (metadata != test->metadata || native_u32((const uint8_t *)metadata+0x2508) != test->capture_number ||
        (image > HD && handle != test->preview_owner)) return 0;
#else
    if (!capture_metadata_readable(metadata)) return 0;
    uint32_t number = native_u32((const uint8_t *)metadata+0x2508);
    if (image == MAIN) {
        test->capture = capture; test->metadata = metadata; test->capture_number = number;
    } else if (capture != test->capture || metadata != test->metadata || number != test->capture_number ||
               (image != HD && handle != test->preview_owner)) return 0;
#endif
    if (image == HD) test->preview_owner = handle;
    JpegInput *in = &test->source[image];
    *in = *(const JpegInput *)(uintptr_t)view;
    uintptr_t owner = native_u64(record+8);
    test->layout_owner = owner;
    test->layout_kind = native_u32(record+0x10);
    test->layout_check = 4;
    if (!native_object(owner)) return 0;
    uint64_t base = native_u64((const void *)owner);
    uint32_t bytes = native_u32((const uint8_t *)owner+8);
    test->layout_base = base; test->layout_bytes = bytes;
    test->layout_check = 5;
    if (!uncached_span(base, bytes) || !source_image(in, base, bytes)) return 0;
    const uint8_t *private_planes[] = {test->staging, test->snapshot[0], test->snapshot[1]};
    for (unsigned i = 0; i < 3; ++i) {
        uintptr_t start = (uintptr_t)private_planes[i];
        if (start && start < base+bytes && base < start+(i ? SNAPSHOT_PLANE : test->arena_bytes)) return 0;
    }
    unsigned w = in->crop_width, h = in->crop_height;
    test->layout_check = 6;
    if (image == MAIN) {
        if (!((w == 8256 && h == 5504) || (w == 2688 && h == 1792) ||
              (!test->linked && w == 4128 && h == 2752))) return 0;
        test->width = w; test->height = h;
    } else if ((image == HD && (w != 1620 || h != 1080)) ||
               (image == VGA && (w != 640 || h != 424)) ||
               (image == THUMB && (w != 160 || h != 120))) return 0;
    test->layout_check = 0;
    return 1;
}

static uint32_t encode_staged(JpegLut *test, unsigned image, uint32_t lane, const uint8_t *request) {
    memcpy(test->staged_request, request, sizeof(test->staged_request));
    *(NativeU64 *)(test->staged_request + 8) = (uintptr_t)&test->images[image];
    return JPEG(lane, test->staged_request);
}

static int submit_stock_only(const JpegLut *test) {
    return __atomic_load_n(&test->submit_stock_only, __ATOMIC_RELAXED) == 1u;
}

#ifndef FX_Z8_LUT_SHARED_OUTPUT
static uint32_t cleanup(JpegLut *test, uint32_t encoded, uint32_t reason) {
    test->reason = reason;
    if (test->linked && (!reason || reason == LAYOUT)) {
        if (!reason) atomic_fetch_add_explicit(&test->completed, 1, memory_order_relaxed);
        if (!test->cancel_requested && atomic_load_explicit(&online, memory_order_acquire)) {
            release_capture(test);
            atomic_store_explicit(&test->phase, ARMED, memory_order_release);
            return encoded;
        }
        test->final_phase = IDLE;
    } else {
        test->final_phase = reason ? SKIPPED : APPLIED;
    }
    release_capture(test);
    atomic_store_explicit(&test->phase, CLEANUP, memory_order_release);
    return encoded;
}

static uint32_t retained(JpegLut *test, uint32_t encoded) {
    /* A failed encoder return is not a DMA fence. Keep its input alive until reboot. */
    test->reason = ENCODE;
    release_capture(test);
    atomic_store_explicit(&test->phase, RETAINED, memory_order_release);
    return encoded;
}

static uint32_t encode_original(JpegViewStats *stats, uint32_t lane, const uint8_t *request, uint32_t reason) {
    stats->flags |= JPEG_FALLBACK | reason;
    uint32_t encoded = JPEG(lane, request);
    stats->hook_exit = counter();
    return encoded;
}
#endif

#ifdef FX_Z8_LUT_SHARED_OUTPUT
static uint64_t burst_selection(const JpegLut *test) {
    uint64_t epoch = atomic_load_explicit(&test->io_epoch, memory_order_relaxed);
    return (epoch << 32) | atomic_load_explicit(&test->output_ticket, memory_order_relaxed);
}

static uint32_t burst_current(const JpegLut *test, uint64_t selection) {
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
    return (phase == ARMED || phase == RUNNING) && test->output_enabled &&
           atomic_load_explicit(&online, memory_order_acquire) && !test->cancel_requested &&
           burst_selection(test) == selection && io_current(test) ? phase : IDLE;
}

static void burst_release(BurstExposure *slot, int keep) {
    uint32_t owned = BURST_OWNED;
    if (keep && atomic_compare_exchange_strong_explicit(&slot->turn, &owned, 0,
                                                         memory_order_release, memory_order_relaxed)) return;
    atomic_store_explicit(&slot->turn, BURST_FREE, memory_order_release);
    atomic_fetch_sub_explicit(&BURST_METRICS.active_exposures, 1, memory_order_relaxed);
}

/* Called under burst_lock. A preview that took the stock path cannot be
 * graded later. Drop an idle slot or prevent its active view from retaining
 * it. The owning hook can simultaneously finish and publish the idle turn. */
static void burst_abandon(BurstExposure *slot, unsigned image) {
    uint32_t turn = atomic_fetch_or_explicit(&slot->turn, BURST_ABANDONED, memory_order_acq_rel);
    if (turn) return;
    if (image >= slot->next_image) burst_release(slot, 0);
    else atomic_store_explicit(&slot->turn, 0, memory_order_release);
}

static int burst_stale(const BurstExposure *slot, uint64_t selection, uint64_t now) {
    return slot->selection != selection || now >= slot->deadline;
}

static void burst_reap(JpegLut *test) {
    if (atomic_exchange_explicit(&burst_lock, 1, memory_order_acquire)) return;
    uint64_t selection = burst_selection(test), now = counter();
    if (!burst_current(test, selection)) selection = 0;
    for (unsigned i = 0; i < BURST_SLOTS; ++i) {
        BurstExposure *slot = &burst_exposure[i];
        if (slot->owner && !atomic_load_explicit(&slot->turn, memory_order_acquire) &&
            burst_stale(slot, selection, now)) burst_release(slot, 0);
    }
    atomic_store_explicit(&burst_lock, 0, memory_order_release);
}

/* Slot scans run under the metadata lock. The wait ticket uses low counter
 * bits (22 s wrap at 24 MHz, versus a 1.2 s maximum wait). A ready preview
 * ranks ahead of every pending MAIN, then each class runs in entry order. */
static int burst_ahead(uint32_t other, uint32_t ticket) {
    if ((other ^ ticket) & BURST_PREVIEW) return (other & BURST_PREVIEW) != 0;
    return (int32_t)((other - ticket) << 3) < 0;
}

static BurstExposure *burst_find(uint64_t selection, uintptr_t capture, uint32_t number,
                                 unsigned image, uint64_t now, uint32_t *failure) {
    BurstExposure *empty = NULL;
    for (unsigned i = 0; i < BURST_SLOTS; ++i) {
        BurstExposure *slot = &burst_exposure[i];
        uint32_t turn = atomic_load_explicit(&slot->turn, memory_order_acquire);
        if (slot->owner && !turn && burst_stale(slot, selection, now)) {
            burst_release(slot, 0);
            turn = BURST_FREE;
        }
        if (!slot->owner || turn == BURST_FREE) {
            if (!empty) empty = slot;
        } else if (slot->owner == capture && slot->number == number && slot->selection == selection) {
            if (image == MAIN) return NULL;
            if (!turn && image == slot->next_image) return slot;
            burst_abandon(slot, image);
            return NULL;
        }
    }
    if (image != MAIN) return NULL;
    if (empty) return empty;
    *failure = JPEG_QUEUE_FULL;
    return NULL;
}

__attribute__((noinline, minsize))
static BurstExposure *burst_admit(JpegLut *test, uint64_t selection, uintptr_t capture,
                                  uint32_t number, unsigned image, uint64_t entered,
                                  unsigned heartbeat, uint32_t *failure) {
    BurstExposure *slot = NULL;
    uint32_t ticket = ((uint32_t)entered & BURST_ORDER_MASK) | BURST_WAITING;
    if (image != MAIN) ticket |= BURST_PREVIEW;
    int queued = 0, owner = 0;
    do {
        uint64_t now = counter();
        if (now - entered > FX_Z8_TICKS_PER_MS * BURST_WAIT_MS || (queued && now >= slot->deadline)) {
            *failure = JPEG_TIMEOUT;
            break;
        }
        uint32_t phase = burst_current(test, selection);
        if (!phase || (image == MAIN && !permitted(test, 1))) {
            *failure = JPEG_INTERRUPTED;
            break;
        }
        if (atomic_exchange_explicit(&burst_lock, 1, memory_order_acquire)) continue;
        if (!queued) slot = burst_find(selection, capture, number, image, now, failure);
        if (!slot || (queued && atomic_load_explicit(&slot->turn, memory_order_acquire) != ticket)) goto unlock;

        uint32_t depth = 0;
        int first = 1;
        for (unsigned i = 0; i < BURST_SLOTS; ++i) {
            BurstExposure *other = &burst_exposure[i];
            if (other == slot) continue;
            uint32_t turn = atomic_load_explicit(&other->turn, memory_order_acquire);
            if ((turn & (BURST_WAITING | BURST_ABANDONED)) != BURST_WAITING) continue;
            ++depth;
            if (burst_ahead(turn, ticket)) first = 0;
        }
        owner = first && phase == ARMED && atomic_compare_exchange_strong(&test->phase, &phase, RUNNING);
        if (!queued && !owner && depth >= BURST_SLOTS - 1u) {
            if (image != MAIN) burst_release(slot, 0);
            *failure = JPEG_QUEUE_FULL;
            goto unlock;
        }
        if (!queued && image == MAIN) {
            /* Four exposures bound depth to three, below the 3.5 s cap. */
            uint32_t budget = BURST_BASE_MS + BURST_DEPTH_MS * (depth + (phase == RUNNING));
            slot->deadline = entered + FX_Z8_TICKS_PER_MS * budget;
            slot->selection = selection;
            slot->owner = capture;
            slot->number = number;
            slot->next_image = MAIN;
            slot->preview_owner = 0;
            atomic_store_explicit(&BURST_METRICS.deadline_ms, budget, memory_order_relaxed);
            atomic_fetch_add_explicit(&BURST_METRICS.active_exposures, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&slot->turn, owner ? BURST_OWNED : ticket, memory_order_release);
        atomic_store_explicit(&burst_lock, 0, memory_order_release);
        if (owner) break;
        if (!queued) {
            queued = 1;
            atomic_fetch_add_explicit(&BURST_METRICS.waiting_views, 1, memory_order_relaxed);
            jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_QUEUE_WAIT, number, image);
        }
    } while (!TASK_DELAY(1u));
    goto done;
unlock:
    atomic_store_explicit(&burst_lock, 0, memory_order_release);
done:
    if (queued) {
        atomic_store_explicit(&BURST_METRICS.wait_ticks, counter() - entered, memory_order_relaxed);
        atomic_fetch_sub_explicit(&BURST_METRICS.waiting_views, 1, memory_order_relaxed);
        if (!owner) burst_release(slot, 0);
    }
    return owner ? slot : NULL;
}

__attribute__((noinline, minsize))
static uint32_t encode_stock(uint32_t lane, const uint8_t *request, unsigned hb, uint32_t number, unsigned image) {
    jpeg_heartbeat(hb, FX_Z8_STAGE_JPEG_ENCODE_STOCK, number, image);
    return JPEG(lane, request);
}

static uint32_t encode_image(uint32_t lane, const uint8_t *request, uintptr_t capture, unsigned image,
                             unsigned heartbeat) {
    JpegLut *test = &probe;
    uint64_t entered = counter(), selection = burst_selection(test);
    if (lane > 1u || image >= IMAGE_COUNT) return JPEG(lane, request);
    if (image == MAIN && selection) jpeg_flight(FX_Z8_FLIGHT_JPEG_MAIN_ENTER, 0, lane, (uint32_t)capture, (uint32_t)selection);

    FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
    if (!burst_current(test, selection) ||
        (image == MAIN && (!permitted(test, 1) || !fx_z8_lut_jpeg_release())) ||
        !io || !fx_z8_lut_frame_take(io, (uint32_t)(selection >> 32))) {
        if (io && (__atomic_load_n(&io->state, __ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_EARLY_OFF))
            jpeg_flight(FX_Z8_FLIGHT_JPEG_FALLBACK, 0, JPEG_INTERRUPTED, image, 0);
        return encode_stock(lane, request, heartbeat, 0, image);
    }

    uint32_t encoded, number = 0, failure = 0;
    uintptr_t metadata;
    if (!native_object(capture) || capture > UINT64_C(0xa40000000) - 0x798u ||
        !capture_metadata_readable(metadata = native_u64((const uint8_t *)capture + 0x790))) {
        uint32_t phase = ARMED;
        if (atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) {
            test->reason = LAYOUT;
            atomic_store_explicit(&test->phase, ARMED, memory_order_release);
        }
        goto fallback;
    }
    number = native_u32((const uint8_t *)metadata + 0x2508);
    BurstExposure *slot = burst_admit(test, selection, capture, number, image, entered, heartbeat, &failure);
    if (!slot) goto fallback;

    JpegViewStats *stats = &fx_z8_jpeg_phase_telemetry.view[image];
    *stats = (JpegViewStats){.hook_enter = entered, .capture_number = number};
    if (image == MAIN) reset_capture(test);
    test->staged_bytes = 0;
    test->last_image = image;
    ++test->visits[image];
    test->reason = 0;
    test->capture = capture;
    test->metadata = metadata;
    test->capture_number = number;
    test->preview_owner = slot->preview_owner;
    atomic_store_explicit(&test->expires, slot->deadline, memory_order_relaxed);
    atomic_store_explicit(&test->capture_ticket, (uint32_t)selection, memory_order_release);
    uint32_t next_phase = ARMED, flags = 0;
    int keep = 0, submitted = 0;
    uint64_t start = counter();
    if (!burst_current(test, selection)) {
        flags = JPEG_INTERRUPTED;
    } else if (start >= slot->deadline) {
        test->reason = TIMING;
        flags = JPEG_TIMEOUT;
    } else if (!current_xm6_input(test, image, capture, request)) {
        test->reason = image == MAIN ? LAYOUT : PAIRING;
    } else {
        jpeg_flight(FX_Z8_FLIGHT_JPEG_GRADE_START, 0, number, image, test->source[image].crop_height);
        Xm6Result staged = stage_xm6(test, image, slot->deadline, heartbeat);
        jpeg_flight(FX_Z8_FLIGHT_JPEG_GRADE_DONE, 0, number, image, (uint32_t)staged);
        test->transform_ticks += counter() - start;
        if (staged == XM6_UNCERTAIN) {
            /* An unconfirmed DSP consumer may still own the staging area. */
            if (test->reason != TIMING && test->reason != SMP_STUCK) test->reason = DSP_ERROR;
            next_phase = RETAINED;
        }
        if (!burst_current(test, selection)) {
            flags = JPEG_INTERRUPTED;
        } else if (counter() >= slot->deadline || test->reason == TIMING) {
            if (test->reason != SMP_STUCK) test->reason = TIMING;
            flags = JPEG_TIMEOUT;
        } else if (staged == XM6_COMPLETE) {
            int stock_only = submit_stock_only(test);
            jpeg_flight(FX_Z8_FLIGHT_JPEG_ENCODE_START, 0, number, image, lane);
            if (stock_only) {
                encoded = encode_stock(lane, request, heartbeat, number, image);
            } else {
                jpeg_heartbeat(heartbeat, FX_Z8_STAGE_JPEG_ENCODE_STAGED, number, image);
                encoded = encode_staged(test, image, lane, request);
            }
            jpeg_flight(FX_Z8_FLIGHT_JPEG_ENCODE_DONE, 0, number, image, encoded);
            submitted = 1;
            stats->flags |= stock_only ? JPEG_STOCK_ONLY : JPEG_STAGED;
            if (image == MAIN) test->encoded = encoded;
            if (encoded & 1u) {
                if (!stock_only) test->applied_mask |= 1u << image;
                keep = image != THUMB && burst_current(test, selection) && counter() < slot->deadline;
                if (image == THUMB) atomic_fetch_add_explicit(&test->completed, 1, memory_order_relaxed);
                slot->next_image = image + 1u;
                slot->preview_owner = test->preview_owner;
            } else {
                /* Encoder failure is not a completion fence. */
                stats->flags |= JPEG_ENCODE_FAILED;
                test->reason = ENCODE;
                if (!stock_only) next_phase = RETAINED;
            }
            stats->hook_exit = counter();
            goto unlock;
        } else if (stats->flags & JPEG_CONTENTION) {
            flags = JPEG_CONTENTION;
        } else if (!test->reason) {
            test->reason = test->use_xm6 == ARM_3D_FILE ? CUBE_ERROR : DSP_ERROR;
        }
    }
    /* Record only while owning this view's telemetry. Release scratch and
     * the frame lease before entering the original stock path. */
    stats->flags |= JPEG_FALLBACK | flags;
    failure = flags;
    stats->hook_exit = counter();
unlock:
    release_capture(test);
    burst_release(slot, keep);
    atomic_store_explicit(&test->phase, next_phase, memory_order_release);
    if (!submitted) goto fallback;
    fx_z8_lut_frame_drop(io);
    return encoded;

fallback:
    atomic_fetch_add_explicit(&BURST_METRICS.fallbacks, 1, memory_order_relaxed);
    uint16_t flight_flags = 0;
    if (failure == JPEG_CONTENTION) {
        atomic_fetch_add_explicit(&BURST_METRICS.contention_passes, 1, memory_order_relaxed);
        flight_flags = FX_Z8_FLIGHT_CONTENTION;
    } else if (failure == JPEG_QUEUE_FULL) {
        atomic_fetch_add_explicit(&BURST_METRICS.queue_full, 1, memory_order_relaxed);
        flight_flags = FX_Z8_FLIGHT_BURST_FALLBACK;
    } else if (failure == JPEG_TIMEOUT) {
        flight_flags = FX_Z8_FLIGHT_DEADLINE;
    }
    jpeg_flight(FX_Z8_FLIGHT_JPEG_FALLBACK, flight_flags, failure, image, number);
    fx_z8_lut_frame_drop(io);
    encoded = encode_stock(lane, request, heartbeat, number, image);
    if (failure == JPEG_CONTENTION)
        jpeg_flight(FX_Z8_FLIGHT_JPEG_ENCODE_DONE, FX_Z8_FLIGHT_CONTENTION, number, image, encoded);
    return encoded;
}
#else
static uint32_t encode_stock_pausing_cube(JpegLut *test, JpegViewStats *stats,
                                          uint32_t lane, const uint8_t *request) {
    uint32_t encoded = encode_original(stats, lane, request, JPEG_INTERRUPTED);
    pause_cube_capture(test);
    return encoded;
}

static uint32_t encode_image(uint32_t lane, const uint8_t *request, uintptr_t capture, unsigned image) {
    JpegLut *test = &probe;
    uint64_t entered = counter();
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
    if (lane > 1u || image >= IMAGE_COUNT ||
        (phase == ARMED && (!atomic_load_explicit(&online, memory_order_acquire) ||
                           test->cancel_requested || cube_mode_blocked(test))) ||
        (phase != PREVIEWS && (phase != ARMED || image != MAIN)) ||
        !atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) return JPEG(lane, request);

    if (phase == ARMED && test->eager &&
        !atomic_load_explicit(&test->output_enabled, memory_order_acquire)) {
        atomic_store_explicit(&test->phase, ARMED, memory_order_release);
        return JPEG(lane, request);
    }
    if (test->linked && phase == PREVIEWS && (image == MAIN || capture != test->capture) &&
        !cube_mode_blocked(test)) {
        atomic_store_explicit(&test->phase, PREVIEWS, memory_order_release);
        return JPEG(lane, request);
    }
    JpegViewStats *stats = &fx_z8_jpeg_phase_telemetry.view[image];
    if (phase == ARMED) memset(fx_z8_jpeg_phase_telemetry.view, 0, sizeof(fx_z8_jpeg_phase_telemetry.view));
    stats->hook_enter = entered;
    if (test->linked && phase == ARMED) {
        reset_capture(test);
        test->reason = 0;
        if (test->cancel_requested || !atomic_load_explicit(&online, memory_order_acquire))
            return cleanup(test, encode_original(stats, lane, request, JPEG_INTERRUPTED), LAYOUT);
    }
    if (cube_mode_blocked(test)) return encode_stock_pausing_cube(test, stats, lane, request);
    uint32_t encoded;
    if (test->use_xm6) {
        uint64_t start = counter();
        if (phase == ARMED) {
            atomic_store_explicit(&test->expires, start + frequency() * XM6_CAPTURE_SECONDS, memory_order_relaxed);
        }
        /* One deadline for the main image and its previews. A stock submit
         * already in progress still needs its own completion confirmation. */
        uint64_t deadline = atomic_load_explicit(&test->expires, memory_order_relaxed);
        if (start >= deadline) return cleanup(test, encode_original(stats, lane, request, JPEG_TIMEOUT), TIMING);
        test->last_image = image; ++test->visits[image];
        if ((phase == PREVIEWS && image != test->next_image) ||
            !current_xm6_input(test, image, capture, request))
            return cleanup(test, encode_original(stats, lane, request, 0), image == MAIN ? LAYOUT : PAIRING);
        stats->capture_number = test->capture_number;
        Xm6Result staged = stage_xm6(test, image, deadline, 0);
        test->transform_ticks += counter()-start;
        int blocked = cube_mode_blocked(test);
        if (staged == XM6_COMPLETE && !blocked) {
            goto submitted;
        }
        if (staged == XM6_UNCERTAIN) {
            if (test->reason != TIMING) test->reason = DSP_ERROR;
            encoded = encode_original(stats, lane, request, test->reason == TIMING ? JPEG_TIMEOUT : 0);
            atomic_store_explicit(&test->phase, RETAINED, memory_order_release);
            return encoded;
        }
        if (test->reason == TIMING) stats->flags |= JPEG_TIMEOUT;
        if (blocked) return encode_stock_pausing_cube(test, stats, lane, request);
        return cleanup(test, encode_original(stats, lane, request, 0),
            test->reason == TIMING ? TIMING : test->use_xm6 == ARM_3D_FILE ? CUBE_ERROR : DSP_ERROR);
    } else if (phase == ARMED) {
        if (!capture_inputs(test, request, capture)) {
            test->reason = LAYOUT;
            encoded = encode_original(stats, lane, request, 0);
            atomic_store_explicit(&test->phase, ARMED, memory_order_release);
            return encoded;
        }
        uint64_t frequency;
        __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
        uint64_t start = counter();
        int staged = frequency && stage_images(test, start, frequency / 2u);
        test->transform_ticks = counter() - start;
        if (cube_mode_blocked(test)) return encode_stock_pausing_cube(test, stats, lane, request);
        if (!staged) return cleanup(test, encode_original(stats, lane, request, JPEG_TIMEOUT), TIMING);
        test->lane = lane;
        atomic_store_explicit(&test->expires, start + 2u * frequency, memory_order_relaxed);
    } else {
        if (capture != test->capture || lane != test->lane || counter() >= test->expires ||
            image != test->next_image) return cleanup(test, encode_original(stats, lane, request, 0), PAIRING);
        const uint8_t *context = (const uint8_t *)capture;
        uintptr_t address = get64(request + 8);
        if (get64(context + 0x6a0) != test->preview_owner || get64(context + 0x790) != test->metadata ||
            get32((const uint8_t *)test->metadata + 0x2508) != test->capture_number ||
            address != test->preview_owner + input_offset[image] ||
            memcmp((const void *)address, &test->source[image], sizeof(JpegInput)))
            return cleanup(test, encode_original(stats, lane, request, 0), PAIRING);
    }
    if (cube_mode_blocked(test)) return encode_stock_pausing_cube(test, stats, lane, request);
submitted:;
    int stock_only = submit_stock_only(test);
    encoded = stock_only ? JPEG(lane, request) : encode_staged(test, image, lane, request);
    stats->flags |= (stock_only ? JPEG_STOCK_ONLY : JPEG_STAGED) | ((encoded & 1u) ? 0u : JPEG_ENCODE_FAILED);
    stats->hook_exit = counter();
    if (image == MAIN) test->encoded = encoded;
    if (!(encoded & 1u)) return stock_only ? cleanup(test, encoded, ENCODE) : retained(test, encoded);
    if (!stock_only) test->applied_mask |= 1u << image;
    if (image == THUMB) return cleanup(test, encoded, 0);
    test->next_image = image + 1u;
    atomic_store_explicit(&test->phase, PREVIEWS, memory_order_release);
    return encoded;
}
#endif

/* Preserve the stock return address and callee-saved registers. The matching
 * stock caller keeps its capture context in x19 or x21 at each call site. */
__attribute__((used, noinline))
uint32_t fx_z8_jpeg_lut_encode(uint32_t lane, const uint8_t *request, uintptr_t capture, unsigned image) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    if (lane > 1u || image >= IMAGE_COUNT) return JPEG(lane, request);
    uint32_t mask = 1u << lane;
    /* Unusual same-lane reentry must leave the first owner's stage intact. */
    int own = !(atomic_fetch_or_explicit(&heartbeat_lanes, mask, memory_order_acquire) & mask);
    unsigned hb = own ? FX_Z8_HB_JPEG_LANE0 + lane : FX_Z8_HB_COUNT;
    uint32_t encoded = encode_image(lane, request, capture, image, hb);
    if (own) {
        jpeg_heartbeat(hb, FX_Z8_STAGE_IDLE, 0, 0);
        atomic_fetch_and_explicit(&heartbeat_lanes, ~mask, memory_order_release);
    }
    return encoded;
#else
    return encode_image(lane, request, capture, image);
#endif
}

#define JPEG_ENTRY(name, context, image) \
    __attribute__((naked)) void fx_z8_jpeg_lut_##name(void) { \
        __asm__ volatile("mov x2, " #context "\nmov w3, #" #image "\nb fx_z8_jpeg_lut_encode"); \
    }
JPEG_ENTRY(main, x19, 0)
JPEG_ENTRY(hd, x21, 1)
JPEG_ENTRY(vga, x21, 2)
JPEG_ENTRY(thumb, x19, 3)

#ifndef FX_Z8_LUT_SHARED_OUTPUT
static FxResult resource(void *context, uint32_t operation, uint64_t owner, uint64_t token,
                         uint64_t *cookie, const FxServiceRequest *request, FxServiceReply *reply) {
    (void)owner;
    JpegLut *test = context;
    if (operation == FX_NATIVE_PACK_ATTACH) {
        uint64_t empty = 0;
        if (!token || !atomic_compare_exchange_strong(&lease, &empty, token)) return FX_BUSY;
        *cookie = token;
        return FX_OK;
    }
    if (!token || *cookie != token || atomic_load(&lease) != token) return FX_STALE;
    if (operation == FX_NATIVE_PACK_DETACH || operation == FX_NATIVE_PACK_PANIC) {
        FxResult stopped = disarm(test);
        if (stopped == FX_BUSY) return stopped;
        if (operation == FX_NATIVE_PACK_DETACH) {
            atomic_store(&lease, 0);
            *cookie = 0;
        }
        return FX_OK;
    }
    if (operation != FX_NATIVE_PACK_CALL || !request || !reply || request->bytes || request->arg[3])
        return FX_ARGUMENT;
    FxResult result;
    switch (request->arg[2]) {
        case LUT_STATUS: advance(test); result = FX_OK; break;
        case LUT_ARM: result = arm(test, DX_CPU, 0); break;
        case LUT_ARM_XM6: result = arm(test, XM6_TEST, 0); break;
        case LUT_ARM_FILE: result = arm(test, XM6_FILE, 0); break;
        case LUT_CANCEL: result = disarm(test); break;
        default: return FX_ARGUMENT;
    }
    reply->value[0] = atomic_load_explicit(&test->phase, memory_order_acquire);
    reply->value[1] = atomic_load_explicit(&test->reason, memory_order_relaxed);
    reply->value[2] = test->use_xm6;
    return result;
}

static const FxNativePackProvider provider = {
    .name = "camera.jpeg.lut", .version = 1, .kind = FX_NATIVE_PACK_DEVICE,
    .call_abi = FX_NATIVE_PACK_CALL_DEVICE, .capabilities = FX_SDK_BINDING,
    .call_context = FX_NATIVE_PACK_WORKER, .resource = resource, .context = &probe,
};
#endif

static uint32_t link(uint32_t operation) {
    JpegLut *test = &probe;
    switch (operation) {
        case Z8_LUT_CAPTURE:
            /* Only published XM6 work gates the linked live-view provider.
             * An uncertain DSP completion keeps the job visible for safety. */
            return atomic_load_explicit(&test->job_phase, memory_order_acquire) != JOB_EMPTY;
#ifndef FX_Z8_LUT_SHARED_OUTPUT
        case Z8_LUT_START: return arm(test, XM6_FILE, 1);
#endif
        case Z8_LUT_STOP: {
            FxResult result = disarm(test);
            if (result == FX_BUSY || (result == FX_CLOSE && (test->reason == TIMING || test->reason == SMP_STUCK))) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
                /* Return an unpublished DSP/ARM job without submitting it. */
                fx_z8_jpeg_lut_snapshot_work();
#endif
                reap_capture(test);
                return disarm(test);
            }
            return result;
        }
        case Z8_LUT_STEP:
            advance(test);
            switch (atomic_load_explicit(&test->phase, memory_order_acquire)) {
                case LOADING: case PREPARING: return Z8_LUT_LOADING;
                case RUNNING: case PREVIEWS: return Z8_LUT_CAPTURING;
#ifdef FX_Z8_LUT_SHARED_OUTPUT
                case RETAINED: return test->reason == SMP_STUCK ? Z8_LUT_CAPTURING : Z8_LUT_ERROR;
#endif
                case ARMED: return Z8_LUT_READY;
                case IDLE: return Z8_LUT_OFF;
                default: return Z8_LUT_ERROR;
            }
        default: return FX_ARGUMENT;
    }
}

static uint32_t link_selected(uint32_t operation, void *data) {
    JpegLut *test = &probe;
#ifndef FX_Z8_LUT_SHARED_OUTPUT
    if (operation == Z8_LUT_START)
        return data && fx_z8_lut_name(data) ? arm_named(test, XM6_FILE, 1, data, 0, 1, NULL) : FX_ARGUMENT;
#endif
    if (operation != Z8_LUT_COPY) return link(operation);
    if (!data) return FX_ARGUMENT;
    uint32_t phase = ARMED;
    if (!atomic_compare_exchange_strong(&test->phase, &phase, RUNNING)) return FX_BUSY;
    FxZ8LutCopy *copy = data;
    FxResult result = FX_STATE;
    if (test->linked && test->cube && !test->cancel_requested && copy->cube &&
#ifdef FX_Z8_LUT_SHARED_OUTPUT
            io_current(test) &&
#endif
            copy->cube_bytes == sizeof(*test->cube)) {
        fx_ram_copy(copy->cube, test->cube, sizeof(*test->cube));
        copy->file_bytes = test->total;
        result = FX_OK;
    }
    atomic_store_explicit(&test->phase, ARMED, memory_order_release);
    return result;
}

#ifndef FX_Z8_LUT_SHARED_OUTPUT
static int same_path(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static FxResult request_selection(JpegLut *test, const FxZ8LutSelection *q) {
    if (!q || q->enabled > 1 || !fx_z8_lut_name(q->name)) return FX_ARGUMENT;
    if (!atomic_load_explicit(&online, memory_order_acquire)) return FX_STATE;
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
    if (test->eager && !test->cancel_requested && same_path(lut_paths(test)->active, q->name) &&
        (phase == LOADING || phase == PREPARING || phase == ARMED || phase == RUNNING || phase == PREVIEWS)) {
        test->want_output = q->enabled;
        if (!q->enabled || test->staging)
            atomic_store_explicit(&test->output_enabled, q->enabled, memory_order_release);
        else if (phase == ARMED && !atomic_compare_exchange_strong(&test->phase, &phase, PREPARING))
            return FX_BUSY;
        return FX_OK;
    }
    if (test->eager) {
        FxResult stopped = (FxResult)link(Z8_LUT_STOP);
        if (stopped != FX_OK) return stopped;
    }
    return arm_named(test, XM6_FILE, 1, q->name, 1, q->enabled, NULL);
}
#endif

static FxResult preparation_error(uint32_t reason) {
    switch (reason) {
        case FORMAT: return FX_ARGUMENT;
        case MEMORY: return FX_BUDGET;
        case CLOSE: return FX_CLOSE;
        case FILE_MISSING: return FX_EMPTY;
        case TIMING: case SMP_STUCK: return FX_TIMEOUT;
        default: return FX_IO;
    }
}

static void selection_progress(const JpegLut *test, FxZ8LutProgress *p) {
    *p = (FxZ8LutProgress){.version = FX_Z8_LUT_PROGRESS_VERSION};
    if (!test->eager) return;
    p->generation = test->generation;
    p->file_bytes = test->total;
    p->work_done = test->bake_pos;
    p->work_total = test->use_xm6 == ARM_3D_FILE ? CUBE_FRAME_CACHE_WORDS : 0;
    p->cache_hit = test->cache_hit;
    switch (atomic_load_explicit(&test->phase, memory_order_acquire)) {
        case IDLE: break;
        case LOADING: p->phase = test->fd < 0 && !test->retry_parse ? FX_Z8_LUT_QUEUED : FX_Z8_LUT_READING; break;
        case PREPARING: p->phase = FX_Z8_LUT_PREPARING; break;
        case ARMED: case RUNNING: case PREVIEWS: p->phase = FX_Z8_LUT_READY; break;
        default:
            p->phase = FX_Z8_LUT_ERROR;
            p->result = preparation_error(test->reason);
            break;
    }
}

#ifdef FX_Z8_LUT_SHARED_OUTPUT
static FxResult prepare_held(const char *path, const FxZ8LutHeldPath *q) {
    if (!q->io || ((uintptr_t)q->io & 7u) || !q->io_epoch ||
        (q->drive != 'A' && q->drive != 'B') || !fx_z8_lut_output_valid(&q->output)) return FX_ARGUMENT;
    if (!fx_z8_lut_output_closed(&q->output) || fx_z8_lut_io_epoch(q->io) != q->io_epoch) return FX_STALE;
    return arm_named(&probe, XM6_FILE, 1, path, 1, 1, q);
}
#endif

static uint32_t link_eager(uint32_t operation, void *data) {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    if (operation == Z8_LUT_CAPABILITIES) return FX_Z8_LUT_CAP_PATH;
    if (operation == Z8_LUT_PREPARE_PATH) {
        const FxZ8LutHeldPath *q = data;
        return q && fx_z8_lut_path(q->path) ? prepare_held(q->path, q) : FX_ARGUMENT;
    }
    if (operation == Z8_LUT_FLIGHT_METRICS) {
        if (!data) return FX_ARGUMENT;
        *(FxZ8JpegFlightMetrics *)data = (FxZ8JpegFlightMetrics){
            atomic_load_explicit(&BURST_METRICS.contention_passes, memory_order_relaxed),
            atomic_load_explicit(&BURST_METRICS.fallbacks, memory_order_relaxed),
            (uint32_t)(atomic_load_explicit(&BURST_METRICS.wait_ticks, memory_order_relaxed) / FX_Z8_TICKS_PER_MS),
            atomic_load_explicit(&BURST_METRICS.queue_full, memory_order_relaxed),
            atomic_load_explicit(&BURST_METRICS.deadline_ms, memory_order_relaxed),
            atomic_load_explicit(&BURST_METRICS.active_exposures, memory_order_relaxed)
        };
        return FX_OK;
    }
    if (operation == Z8_LUT_PREPARE) {
        const FxZ8LutHeldSelection *q = data;
        if (!q || q->selection.enabled != 1 || !fx_z8_lut_name(q->selection.name)) return FX_ARGUMENT;
        const FxZ8LutHeldPath request = {.output = q->output, .drive = q->drive, .io = q->io,
                                        .io_epoch = q->io_epoch};
        return prepare_held(q->selection.name, &request);
    }
#else
    if (operation == Z8_LUT_PREPARE) return request_selection(&probe, data);
#endif
    if (operation != Z8_LUT_PROGRESS) return link_selected(operation, data);
    if (!data) return FX_ARGUMENT;
    selection_progress(&probe, data);
    return FX_OK;
}

static const FxNativePackSymbol link_exports[] = {
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    {FX_Z8_JPEG_LUT_WORKER, FX_Z8_JPEG_LUT_WORKER_VERSION, FX_NATIVE_PACK_SYMBOL_FUNCTION, (uintptr_t)link_eager},
#else
    {FX_Z8_JPEG_LUT_LINK, FX_Z8_JPEG_LUT_LINK_VERSION, FX_NATIVE_PACK_SYMBOL_FUNCTION, (uintptr_t)link},
    {FX_Z8_JPEG_LUT_SELECTED, FX_Z8_JPEG_LUT_SELECTED_VERSION, FX_NATIVE_PACK_SYMBOL_FUNCTION, (uintptr_t)link_selected},
    {FX_Z8_JPEG_LUT_PREPARE, FX_Z8_JPEG_LUT_PREPARE_VERSION, FX_NATIVE_PACK_SYMBOL_FUNCTION, (uintptr_t)link_eager},
#endif
    {FX_NATIVE_LOCALE_SYMBOL, 1u, FX_NATIVE_PACK_SYMBOL_DATA, (uintptr_t)&stock_locale},
};

static uint32_t pending(void *context) {
    const JpegLut *test = context;
    uint32_t phase = atomic_load_explicit(&test->phase, memory_order_acquire);
#ifdef FX_Z8_LUT_SHARED_OUTPUT
    if (active_views(test)) return 1;
    if (!atomic_load_explicit(&online, memory_order_acquire) &&
        (phase == ARMED || phase == LOADING || phase == PREPARING || phase == CLEANUP)) return 1;
#endif
    return phase == RUNNING || phase == PREVIEWS || phase == RETAINED;
}

static FxResult event(void *context, uint32_t action) {
    JpegLut *test = context;
    switch (action) {
        case FX_NATIVE_PACK_GUI_POLL: return FX_OK;
        case FX_NATIVE_PACK_PUBLISH:
        case FX_NATIVE_PACK_RESUME:
            if (pending(context)) return FX_BUSY;
            atomic_store_explicit(&online, 1, memory_order_release);
            return FX_OK;
        case FX_NATIVE_PACK_STOP:
        case FX_NATIVE_PACK_FAULT:
        case FX_NATIVE_PACK_ABORT:
#ifdef FX_Z8_LUT_SHARED_OUTPUT
            {
                FxZ8LutIoGate *io = atomic_load_explicit(&test->io, memory_order_relaxed);
                if (io) fx_z8_lut_io_close(io, 1);
                atomic_store_explicit(&online, 0, memory_order_release);
                atomic_store_explicit(&test->cancel_requested, 1, memory_order_release);
                /* The retrying menu worker owns Z8_LUT_STOP and the open file
                 * descriptor. Lifecycle code never closes a shared card handle. */
                if (pending(test)) return FX_BUSY;
            }
#else
            atomic_store_explicit(&online, 0, memory_order_release);
            reap_capture(context);
            if (disarm(context) == FX_BUSY || pending(context)) return FX_BUSY;
#endif
            for (unsigned i = 0; i < LUT_CACHE_SLOTS; ++i) clear_cached_lut(&lut_cache[i]);
            if (test->read_scratch) {
                HEAP_FREE(test->read_scratch);
                test->read_scratch = NULL;
            }
            return FX_OK;
        default: return FX_ARGUMENT;
    }
}

FxResult fx_native_pack_entry(const FxNativePackHost *host, FxNativePackExports *out) {
    if (!host || !out || host->abi != FX_NATIVE_PACK_ABI || host->bytes < sizeof(*host) || !host->lookup)
        return FX_ARGUMENT;
    for (unsigned i = 0; i < sizeof(jpeg_lut_bindings) / sizeof(jpeg_lut_bindings[0]); ++i) {
        const void *address = (const void *)jpeg_lut_bindings[i].address;
        if (memcmp(address, jpeg_lut_bindings[i].prefix, sizeof(jpeg_lut_bindings[i].prefix))) return FX_INTEGRITY;
    }
    *out = (FxNativePackExports){.abi = FX_NATIVE_PACK_ABI, .bytes = sizeof(*out),
#ifndef FX_Z8_LUT_SHARED_OUTPUT
        .provider_count = 1, .providers = &provider,
#endif
        .context = &probe, .event = event, .pending = pending};
    uintptr_t locale = 0;
    FxResult found = host->lookup(host->context, FX_NATIVE_LOCALE_SYMBOL, 1, &locale);
    if (found != FX_CAPABILITY && (found != FX_OK || !locale)) return FX_INTEGRITY;
    out->symbols = link_exports;
    out->symbol_count = (uint32_t)(sizeof(link_exports) / sizeof(link_exports[0])) - (found == FX_OK);
    return FX_OK;
}
