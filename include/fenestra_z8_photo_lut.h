/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FENESTRA_Z8_PHOTO_LUT_H
#define FENESTRA_Z8_PHOTO_LUT_H

#include <stdint.h>

#define FX_Z8_JPEG_LUT_LINK "camera.z8.jpeg.lut.link"
#define FX_Z8_JPEG_LUT_LINK_VERSION 1u
#define FX_Z8_JPEG_LUT_SELECTED "camera.z8.jpeg.lut.selected"
#define FX_Z8_JPEG_LUT_SELECTED_VERSION 1u
#define FX_Z8_JPEG_LUT_PREPARE "camera.z8.jpeg.lut.prepare"
#define FX_Z8_JPEG_LUT_PREPARE_VERSION 1u
#define FX_Z8_JPEG_LUT_WORKER "camera.z8.jpeg.lut.worker"
#define FX_Z8_JPEG_LUT_WORKER_VERSION 1u
#define FX_Z8_REAR_LUT_WORKER "camera.z8.rear.lut.worker"
#define FX_Z8_REAR_LUT_WORKER_VERSION 1u
#define FX_Z8_REAR_LUT_IO "camera.z8.rear.lut.io"
#define FX_Z8_REAR_LUT_IO_VERSION 1u

#define FX_Z8_LUT_DEFAULT "LOOK.CUBE"
enum { FX_Z8_LUT_NAME_BYTES = 32, FX_Z8_LUT_PATH_BYTES = 64 };

/* START, STOP and STEP run on the module worker. CAPTURE is a read-only
 * atomic query for JPEG ownership of XM6, used at preview scheduling boundaries. */
enum { Z8_LUT_START = 1, Z8_LUT_STOP, Z8_LUT_STEP, Z8_LUT_CAPTURE, Z8_LUT_COPY };
enum { Z8_LUT_OFF, Z8_LUT_LOADING, Z8_LUT_READY, Z8_LUT_CAPTURING, Z8_LUT_ERROR };
enum { Z8_LUT_PREPARE = 6, Z8_LUT_PROGRESS, Z8_LUT_FLIGHT_METRICS };
/* CAPABILITIES returns a bitmask directly. PREPARE_PATH is available only
 * from the shared-output worker and always prepares an enabled LUT. */
enum { Z8_LUT_CAPABILITIES = 9, Z8_LUT_PREPARE_PATH };
/* Old workers return FX_ARGUMENT (1) for the unknown capabilities query. */
enum { FX_Z8_LUT_CAP_PATH = 1u << 8 };
enum { REAR_CLEAR = 4, REAR_READY = 5, REAR_RETIRE = 6, REAR_PRESENTED = 7, REAR_WORKER_STEP = 2 };
/* STEP already paced an accelerated EVF turn and can resume without an outer delay. */
enum { REAR_STREAMING = 0x100 };
/* Acknowledge software LUT retirement even if a hardware bank must stay pinned. */
enum { REAR_SWITCH = 8 };
enum { REAR_FLIGHT_METRICS = 9 };
enum { FX_Z8_LUT_IDLE, FX_Z8_LUT_QUEUED, FX_Z8_LUT_READING, FX_Z8_LUT_PREPARING,
       FX_Z8_LUT_READY, FX_Z8_LUT_ERROR };
#define FX_Z8_LUT_PROGRESS_VERSION 1u
typedef struct {
    uint32_t enabled;
    char name[FX_Z8_LUT_NAME_BYTES];
} FxZ8LutSelection;
typedef struct {
    uint32_t version, generation, phase, result;
    uint32_t file_bytes, work_done, work_total, cache_hit;
} FxZ8LutProgress;
typedef uint32_t (*FxZ8JpegLutLink)(uint32_t operation);
typedef uint32_t (*FxZ8JpegLutSelected)(uint32_t operation, void *data);
typedef struct { void *cube; uint32_t cube_bytes, file_bytes; } FxZ8LutCopy;

enum {
    FX_Z8_FLIGHT_JPEG_MAIN_ENTER = 1, FX_Z8_FLIGHT_JPEG_QUEUE_WAIT,
    FX_Z8_FLIGHT_JPEG_GRADE_START, FX_Z8_FLIGHT_JPEG_GRADE_DONE,
    FX_Z8_FLIGHT_JPEG_ENCODE_START, FX_Z8_FLIGHT_JPEG_ENCODE_DONE,
    FX_Z8_FLIGHT_JPEG_FALLBACK, FX_Z8_FLIGHT_JPEG_YIELD,
    FX_Z8_FLIGHT_PREVIEW_ROUTE_SWITCH, FX_Z8_FLIGHT_PREVIEW_CAPTURE_ABORT,
    FX_Z8_FLIGHT_PREVIEW_POWER_OFF, FX_Z8_FLIGHT_PACK_FAULT, FX_Z8_FLIGHT_PACK_STOP,
    FX_Z8_FLIGHT_STALL, FX_Z8_FLIGHT_SMP_QUARANTINE
};
enum { FX_Z8_FLIGHT_BURST_FALLBACK = 1u << 4, FX_Z8_FLIGHT_DEADLINE = 1u << 5,
       FX_Z8_FLIGHT_CONTENTION = 1u << 6 };
typedef struct {
    uint32_t contention_passes, fallbacks, wait_ms, queue_full, deadline_ms, active_exposures;
} FxZ8JpegFlightMetrics;
typedef struct {
    uint32_t active_route, grade_us, worker_cores, neon_active, capture_aborts, timeouts;
} FxZ8RearFlightMetrics;

/* The providers enter the registry before the menu. Probe the fixed v4
 * veneer so a standalone provider or an older composition can omit it. */
#define FX_Z8_FLIGHT_ENTRY 0x40b60340u
#define FX_Z8_FLIGHT_ABI 0x46523431u
static inline void fx_z8_photo_flight(uint16_t event, uint16_t core_flags,
                                     uint32_t a, uint32_t b, uint32_t c) {
    if (*(const volatile uint32_t *)(uintptr_t)(FX_Z8_FLIGHT_ENTRY + 4u) == FX_Z8_FLIGHT_ABI)
        ((void (*)(uint16_t, uint16_t, uint32_t, uint32_t, uint32_t))
          (uintptr_t)FX_Z8_FLIGHT_ENTRY)(event, core_flags, a, b, c);
}

enum {
    FX_Z8_HB_JPEG_LANE0, FX_Z8_HB_JPEG_LANE1, FX_Z8_HB_JPEG_HELPER1, FX_Z8_HB_JPEG_HELPER2,
    FX_Z8_HB_REAR_STEP, FX_Z8_HB_REAR_HELPER1, FX_Z8_HB_REAR_HELPER2, FX_Z8_HB_WORKER,
    FX_Z8_HB_COUNT
};
enum {
    FX_Z8_STAGE_IDLE, FX_Z8_STAGE_JPEG_QUEUE_WAIT, FX_Z8_STAGE_JPEG_SMP_DISPATCH,
    FX_Z8_STAGE_JPEG_GRADE, FX_Z8_STAGE_JPEG_SMP_JOIN, FX_Z8_STAGE_JPEG_CACHE_MAINT,
    FX_Z8_STAGE_JPEG_ENCODE_STAGED, FX_Z8_STAGE_JPEG_ENCODE_STOCK, FX_Z8_STAGE_JPEG_XM6,
    FX_Z8_STAGE_REAR_COPY, FX_Z8_STAGE_REAR_GRADE, FX_Z8_STAGE_REAR_JOIN,
    FX_Z8_STAGE_WORKER_CARD_IO, FX_Z8_STAGE_WORKER_STEP
};
enum {
    FX_Z8_HB_CARD_DRIVE = 1, FX_Z8_HB_CARD_OPEN, FX_Z8_HB_CARD_READ, FX_Z8_HB_CARD_WRITE,
    FX_Z8_HB_CARD_CLOSE, FX_Z8_HB_CARD_REMOVE, FX_Z8_HB_CARD_RENAME, FX_Z8_HB_DIR_OPEN,
    FX_Z8_HB_DIR_NEXT, FX_Z8_HB_DIR_CLOSE, FX_Z8_HB_CARD_CLEAN, FX_Z8_HB_CRASH_WRITE
};
typedef struct {
    uint32_t stage, cap_num, core, aux;
    uint64_t enter_tick, progress_tick;
} FxZ8Heartbeat;

#define FX_Z8_HEARTBEAT_TABLE 0x40b73808u
#define FX_Z8_HEARTBEAT_ABI 0x48425431u
#define FX_Z8_HEARTBEAT_BUSY (UINT64_C(1) << 63)
#define FX_Z8_TICKS_PER_MS 24000u

static inline __attribute__((always_inline)) void fx_z8_heartbeat_enter(
        unsigned slot, unsigned stage, uint32_t cap_num, uint32_t aux) {
#if defined(__aarch64__)
    if (slot >= FX_Z8_HB_COUNT ||
        *(const volatile uint32_t *)(uintptr_t)(FX_Z8_FLIGHT_ENTRY + 8u) != FX_Z8_HEARTBEAT_ABI) return;
    FxZ8Heartbeat *hb = (FxZ8Heartbeat *)(uintptr_t)FX_Z8_HEARTBEAT_TABLE + slot;
    uint64_t now, core;
    __asm__ volatile("mrs %0,cntpct_el0; mrs %1,mpidr_el1" : "=r"(now), "=r"(core) :: "memory");
    uint64_t prior = __atomic_load_n(&hb->enter_tick, __ATOMIC_RELAXED);
    uint64_t enter = now > prior ? now : prior + 1u;
    /* Each slot has one owner. A busy enter_tick invalidates in-flight reads
     * while payload words change. Monotonic entry ticks also prevent ABA. */
    __atomic_store_n(&hb->enter_tick, enter | FX_Z8_HEARTBEAT_BUSY, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&hb->stage, stage, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->cap_num, cap_num, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->core, stage ? (uint32_t)((core & 15u) + 1u) : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->aux, aux, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->progress_tick, now, __ATOMIC_RELAXED);
    __atomic_store_n(&hb->enter_tick, enter, __ATOMIC_RELEASE);
#else
    (void)slot; (void)stage; (void)cap_num; (void)aux;
#endif
}

static inline __attribute__((always_inline)) void fx_z8_heartbeat_progress(unsigned slot) {
#if defined(__aarch64__)
    if (slot >= FX_Z8_HB_COUNT ||
        *(const volatile uint32_t *)(uintptr_t)(FX_Z8_FLIGHT_ENTRY + 8u) != FX_Z8_HEARTBEAT_ABI) return;
    FxZ8Heartbeat *hb = (FxZ8Heartbeat *)(uintptr_t)FX_Z8_HEARTBEAT_TABLE + slot;
    uint64_t now;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now) :: "memory");
    __atomic_store_n(&hb->progress_tick, now, __ATOMIC_RELAXED);
#else
    (void)slot;
#endif
}

static inline __attribute__((always_inline)) void fx_z8_heartbeat_idle(unsigned slot) {
    fx_z8_heartbeat_enter(slot, FX_Z8_STAGE_IDLE, 0, 0);
}

/* The menu owns this aligned word for the life of the native composition.
 * Changing the ticket revokes new output for both providers. A JPEG exposure
 * admitted earlier keeps its generation while the shared I/O epoch stays open. */
typedef struct {
    const uint32_t *permit;
    uint32_t ticket;
} FxZ8LutOutput;

/* The rear provider owns this naturally aligned word from publication through
 * retirement. The high half is the mount-session epoch, low bit admits new
 * I/O, and the remainder counts operations admitted before the gate closed.
 * A lifecycle stop sets bit 31 so a late OFF cancellation cannot reopen it. */
typedef struct { uint64_t state; } FxZ8LutIoGate;
#define FX_Z8_LUT_IO_STOPPED UINT64_C(0x80000000)
#define FX_Z8_LUT_IO_EARLY_OFF UINT64_C(0x04000000)
#define FX_Z8_LUT_IO_REFS UINT64_C(0x7bfffffe)
/* Card references use bits 1..23, helper ownership uses bits 24..25,
 * and frame leases use bits 27..30. Epoch changes preserve all three. */
#define FX_Z8_LUT_HELPER_JPEG UINT64_C(0x01000000)
#define FX_Z8_LUT_HELPER_REAR UINT64_C(0x02000000)
#define FX_Z8_LUT_HELPERS (FX_Z8_LUT_HELPER_JPEG | FX_Z8_LUT_HELPER_REAR)
#define FX_Z8_LUT_IO_FRAME UINT64_C(0x78000000)
#define FX_Z8_LUT_IO_FRAME_ONE UINT64_C(0x08000000)
#define FX_Z8_LUT_IO_CARD (FX_Z8_LUT_IO_REFS & ~(FX_Z8_LUT_IO_FRAME | FX_Z8_LUT_HELPERS))

static inline uint32_t fx_z8_lut_io_epoch(const FxZ8LutIoGate *gate) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    return state & 1u ? (uint32_t)(state >> 32) : 0;
}

static inline int fx_z8_lut_io_take(FxZ8LutIoGate *gate, uint32_t epoch) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    do {
        if (!epoch || state >> 32 != epoch || !(state & 1u) || (state & FX_Z8_LUT_IO_CARD) == FX_Z8_LUT_IO_CARD)
            return 0;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state + 2u,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return 1;
}

static inline void fx_z8_lut_io_drop(FxZ8LutIoGate *gate) {
    __atomic_fetch_sub(&gate->state, 2u, __ATOMIC_RELEASE);
}

static inline int fx_z8_lut_frame_take(FxZ8LutIoGate *gate, uint32_t epoch) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    do {
        if (!epoch || state >> 32 != epoch || !(state & 1u) ||
            (state & FX_Z8_LUT_IO_FRAME) == FX_Z8_LUT_IO_FRAME) return 0;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state + FX_Z8_LUT_IO_FRAME_ONE,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return 1;
}

static inline void fx_z8_lut_frame_drop(FxZ8LutIoGate *gate) {
    __atomic_fetch_sub(&gate->state, FX_Z8_LUT_IO_FRAME_ONE, __ATOMIC_RELEASE);
}

static inline uint32_t fx_z8_lut_frame_count(const FxZ8LutIoGate *gate) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    return (uint32_t)((state & FX_Z8_LUT_IO_FRAME) / FX_Z8_LUT_IO_FRAME_ONE);
}

/* Taking both helper cores as one unit prevents a JPEG dispatch from
 * overtaking an already admitted preview. A JPEG frame lease gives JPEG
 * priority over any later preview admission, even between grade and encode. */
static inline int fx_z8_lut_helpers_take(FxZ8LutIoGate *gate, uint32_t epoch, uint64_t owner) {
    uint64_t busy = FX_Z8_LUT_HELPERS | (owner == FX_Z8_LUT_HELPER_REAR ? FX_Z8_LUT_IO_FRAME : 0);
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    do {
        if (!epoch || state >> 32 != epoch || !(state & 1u) || (state & busy)) return 0;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, state | owner,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return 1;
}

static inline void fx_z8_lut_helpers_drop(FxZ8LutIoGate *gate, uint64_t owner) {
    __atomic_fetch_and(&gate->state, ~owner, __ATOMIC_RELEASE);
}

static inline void fx_z8_lut_io_close(FxZ8LutIoGate *gate, int lifecycle) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    uint64_t stop = lifecycle ? FX_Z8_LUT_IO_STOPPED : FX_Z8_LUT_IO_EARLY_OFF;
    while (!__atomic_compare_exchange_n(&gate->state, &state, (state & ~UINT64_C(1)) | stop,
                                        1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { }
}

static inline void fx_z8_lut_io_reopen(FxZ8LutIoGate *gate, int cancel) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    uint64_t next;
    do {
        const uint64_t epoch = state >> 32;
        if (epoch == UINT32_MAX || (cancel && (!epoch || (state & (FX_Z8_LUT_IO_STOPPED | 1u))))) return;
        next = ((epoch + 1u) << 32) | (state & FX_Z8_LUT_IO_REFS) | 1u;
    } while (!__atomic_compare_exchange_n(&gate->state, &state, next,
                                           1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

typedef struct {
    FxZ8LutSelection selection;
    FxZ8LutOutput output;
    char drive;
    FxZ8LutIoGate *io;
    uint32_t io_epoch;
} FxZ8LutHeldSelection;

typedef struct {
    char path[FX_Z8_LUT_PATH_BYTES];
    FxZ8LutOutput output;
    char drive;
    FxZ8LutIoGate *io;
    uint32_t io_epoch;
} FxZ8LutHeldPath;

static inline int fx_z8_lut_output_valid(const FxZ8LutOutput *out) {
    return out && out->permit && !((uintptr_t)out->permit & 3u) &&
           (out->ticket & 1u) && out->ticket > 1u;
}

static inline int fx_z8_lut_output_closed(const FxZ8LutOutput *out) {
    return __atomic_load_n(out->permit, __ATOMIC_ACQUIRE) == (out->ticket & ~1u);
}

static inline int fx_z8_lut_output_open(const FxZ8LutOutput *out) {
    return __atomic_load_n(out->permit, __ATOMIC_ACQUIRE) == out->ticket;
}

static inline int fx_z8_lut_component_char(unsigned c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static inline unsigned fx_z8_lut_category_length(const char *name) {
    unsigned n = 0;
    while (n < 26u && fx_z8_lut_component_char((unsigned char)name[n])) ++n;
    return n;
}

static inline int fx_z8_lut_category(const char *name) {
    unsigned n = fx_z8_lut_category_length(name);
    return n && !name[n];
}

/* A card filename, with space for a terminator and up to 26 stem bytes. */
static inline int fx_z8_lut_name(const char *name) {
    unsigned n = fx_z8_lut_category_length(name);
    if (!n) return 0;
    return name[n] == '.' && (name[n+1] | 32) == 'c' && (name[n+2] | 32) == 'u' &&
           (name[n+3] | 32) == 'b' && (name[n+4] | 32) == 'e' && !name[n+5];
}

/* Both components are bounded, including the 63-byte longest relative path. */
static inline int fx_z8_lut_path(const char *path) {
    if ((path[0] | 32) != 'l' || (path[1] | 32) != 'u' ||
        (path[2] | 32) != 't' || (path[3] | 32) != 's' || path[4] != '\\')
        return fx_z8_lut_name(path);
    const char *category = path + 5;
    unsigned n = fx_z8_lut_category_length(category);
    return n && category[n] == '\\' && fx_z8_lut_name(category + n + 1u);
}

#endif
