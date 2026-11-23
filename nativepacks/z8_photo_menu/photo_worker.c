/* SPDX-License-Identifier: GPL-3.0-only */
#include "photo_menu.h"
#include "fenestra_memory.h"
#include "fenestra_native_pack.h"
#include "fenestra_machine.h"
#include <stdatomic.h>

#define FN(address, type) ((type)(uintptr_t)(address))
#define CARD_DRIVE FN(0x401d450c, char (*)(void))
#define CARD_OPEN FN(0x402dd4f8, int32_t (*)(const char *, uint32_t, uint32_t))
#define CARD_CLOSE FN(0x402dd558, uint32_t (*)(int32_t))
#define CARD_READ FN(0x401d3ba0, uint32_t (*)(int32_t, void *, uint32_t, uint32_t *))
#define CARD_WRITE FN(0x402dd78c, uint32_t (*)(int32_t, const void *, uint32_t, uint32_t *, uint32_t))
#define CARD_REMOVE FN(0x402dda4c, uint32_t (*)(const char *))
#define CARD_RENAME FN(0x402dd9fc, uint32_t (*)(const char *, const char *))
#define CARD_CLEAN FN(0x401b7668, FxResult (*)(void *, uintptr_t, uint32_t, uint32_t))
#define DIR_OPEN FN(0x402ddc6c, uint32_t (*)(const char *, int32_t *))
#define DIR_NEXT FN(0x402ddd1c, uint32_t (*)(int32_t, void *))
#define DIR_CLOSE FN(0x402ddd6c, uint32_t (*)(int32_t))
#define HEAP_ALLOC FN(0x40d2c408, void *(*)(uint32_t, uint32_t))
#define HEAP_FREE FN(0x40d2c448, void (*)(void *))
#define TASK_START FN(0x401de608, FxResult (*)(PhotoTaskScope *, void (*)(void *), void *, void *, uint32_t))
#define TASK_DELAY FN(0x40187b00, int32_t (*)(uint32_t))

enum { PARKED, ARMED, DRIVE_A, DRIVE_B, WORKER_FAILED, MODE_MASK = 7, WORKER_BUSY = 8,
       GUI_OPEN = 16, GUI_EARLY_OFF = 32 };
enum { NO_JOB, DRAIN, PREPARE, STEP, COPY, SAVE, OUTPUT, CLEAN_ERROR };
enum { PAGE_OPEN = 1, PAGE_PREVIOUS, PAGE_NEXT, PAGE_ENTER, PAGE_BACK, PAGE_CLOSE, PAGE_OP_MASK = 7 };
enum { NO_SCAN, ROOT_FOLDERS, ROOT_FIND_FLAT, FOLDER_FILES };
enum { REAR_RETRY_SLICES = 250, FILE_RETAINED = 1, DIRECTORY_RETAINED = 2 };
enum { BURST_CARD_DRAIN_MS = 100 };
enum { CRASH_LOG_STOPPED = 1, CRASH_LOG_FAULTED = 2 };
enum { CRASH_TERMINAL = PHOTO_CRASH_STOP | PHOTO_CRASH_FAULT };
enum { CRASH_BACKOFF_MS = 2000, CRASH_BURST_DEFER_MS = 1500, CRASH_MAX_ATTEMPTS = 5 };
typedef struct {
    uint32_t magic, phase, native_error, reserved;
    int32_t id;
    uint32_t padding;
    uintptr_t stack;
    uint64_t bytes;
} PhotoTaskScope;
_Static_assert(sizeof(PhotoTaskScope) == 40, "foundation task scope");
_Static_assert(sizeof(_Atomic uint32_t) == sizeof(uint32_t), "shared permit ABI");

typedef struct {
    uint32_t ticket;
    char name[FX_Z8_LUT_PATH_BYTES];
} Selection;

typedef struct {
    uint64_t epoch;
    uint32_t serial, root_focus;
    char category[FX_Z8_LUT_NAME_BYTES];
} PageCommand;

typedef struct {
    int32_t handle;
    uint32_t io_epoch;
    char drive;
} RetainedHandle;

typedef struct {
    PhotoTaskScope task;
    uint32_t epoch, io_epoch, page_seen, job, restore, ticket;
    uint32_t rear_wait, rear_retiring, jpeg_drained, rear_presented, media_fault;
    FxResult save_result;
    char drive, name[FX_Z8_LUT_PATH_BYTES];
    int32_t directory;
    RetainedHandle file_retained, directory_retained;
    PhotoCatalog catalog;
    PhotoPage page;
    char category[FX_Z8_LUT_NAME_BYTES];
    char (*cache)[FX_Z8_LUT_NAME_BYTES];
    uint32_t scan, cache_count, cache_base, cache_ready;
} Worker;

/* High half identifies this lifecycle. Never compare only the mode when
 * a call can be preempted through STOP and RESUME. */
static _Atomic uint64_t activity;
static _Atomic uint32_t permit, restore_ticket, task_attempted, directory_owned;
static _Atomic uint32_t bound_io_epoch, drain_needed, retained, media_unavailable;
static _Atomic uint32_t crash_mount_epoch, crash_lifecycle;
static _Atomic uint64_t crash_exhausted;
static _Atomic uint32_t page_request, page_seq, page_words[sizeof(PhotoPage) / sizeof(uint32_t)];
static _Atomic uint32_t page_command_seq, page_command_words[sizeof(PageCommand) / sizeof(uint32_t)];
static _Atomic uint32_t status_seq, status_words[sizeof(PhotoStatus) / sizeof(uint32_t)];
static _Atomic uint32_t selection_seq, selection_words[sizeof(Selection) / sizeof(uint32_t)];
static Worker worker;
static PhotoPage root_page __attribute__((section(".browser")));
static struct {
    uint64_t request, since_ms, next_ms;
    uint32_t attempts, epoch, temp_index;
} crash_retry;
static _Alignas(64) uint8_t entry_buffer[(PHOTO_DIR_BYTES + 63u) & ~63u];
static _Alignas(4096) uint8_t write_buffer[4096];
static _Alignas(16) uint8_t worker_stack[32768] __attribute__((section(".stack"), used));
static FxZ8JpegLutSelected jpeg, rear;
static uint32_t jpeg_caps;
static FxZ8LutIoGate *io_gate;
static FxZ8LutIoGate burst_io;
extern const uint32_t photo_feature_selection;

static inline __attribute__((always_inline)) void worker_step_heartbeat(void) {
    fx_z8_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_STEP, 0, worker.job);
}

static inline __attribute__((always_inline)) void worker_card_heartbeat(unsigned operation) {
    fx_z8_heartbeat_enter(FX_Z8_HB_WORKER, FX_Z8_STAGE_WORKER_CARD_IO, 0, operation);
}

static char worker_card_drive(void) {
    worker_card_heartbeat(FX_Z8_HB_CARD_DRIVE);
    char drive = CARD_DRIVE();
    worker_step_heartbeat();
    return drive;
}

static FxZ8LutIoGate *crash_gate(void) {
    return io_gate ? io_gate : &burst_io;
}

static uint32_t mode(uint64_t state) {
    return (uint32_t)state & MODE_MASK;
}

static char state_drive(uint64_t state) {
    uint32_t value = mode(state);
    return value == DRIVE_A ? 'A' : value == DRIVE_B ? 'B' : 0;
}

static char live_drive(void) {
    return state_drive(atomic_load(&activity));
}

static int next_output(uint32_t old, uint32_t *next) {
    *next = (old & ~1u) + 2u;
    return *next != 0;
}

static uint32_t close_output(void) {
    uint32_t old = atomic_load(&permit), next;
    do {
        if (!next_output(old, &next)) next = old & ~1u;
    } while (!atomic_compare_exchange_weak(&permit, &old, next));
    return (old & ~1u) == next ? 0 : next | 1u;
}

uint64_t photo_gui_epoch(void) {
    uint64_t state = atomic_load(&activity);
    if ((state & (GUI_OPEN | GUI_EARLY_OFF)) != GUI_OPEN) return 0;
    uint32_t io_epoch = io_gate ? fx_z8_lut_io_epoch(io_gate) : 1u;
    return io_epoch ? (state & (UINT64_MAX << 32)) | io_epoch : 0;
}

void photo_burst_power(uint32_t cancel) {
    if (!cancel) {
        fx_z8_lut_io_close(&burst_io, 0);
        photo_flight_log(FX_Z8_FLIGHT_PREVIEW_POWER_OFF, photo_flight_core(), 0, 0, 0);
        /* This variant has no rear receiver to drain admitted card calls. */
        uint64_t start = photo_flight_ms();
        for (unsigned i = 0; i < BURST_CARD_DRAIN_MS &&
             (__atomic_load_n(&burst_io.state, __ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_CARD); ++i)
            if (photo_flight_ms() - start >= BURST_CARD_DRAIN_MS || TASK_DELAY(1u)) break;
    }
    uint64_t state = atomic_load(&activity), next;
    do {
        if (!(state & GUI_OPEN) || (cancel && (!(state & GUI_EARLY_OFF) || state >> 32 == UINT32_C(0x3fffffff))))
            return;
        next = cancel ? (state + (UINT64_C(1) << 32)) & ~((uint64_t)GUI_EARLY_OFF) : state | GUI_EARLY_OFF;
    } while (!atomic_compare_exchange_weak(&activity, &state, next));
    if (cancel) fx_z8_lut_io_reopen(&burst_io, 1);
}

/* Payload words are atomic as well, so readers can safely discard a copy
 * interrupted by the single producer. The GUI never waits for card work. */
static void publish(_Atomic uint32_t *sequence, _Atomic uint32_t *words,
                    const void *data, size_t bytes) {
    atomic_fetch_add(sequence, 1u);
    const uint8_t *p = data;
    for (size_t offset = 0; offset < bytes; offset += sizeof(uint32_t)) {
        uint32_t word;
        memcpy(&word, p + offset, sizeof(word));
        atomic_store(&words[offset / sizeof(word)], word);
    }
    atomic_fetch_add(sequence, 1u);
}

static int snapshot(const _Atomic uint32_t *sequence, const _Atomic uint32_t *words,
                    void *data, size_t bytes) {
    uint8_t *p = data;
    uint32_t before = atomic_load(sequence);
    if (!before || (before & 1u)) return 0;
    for (size_t offset = 0; offset < bytes; offset += sizeof(uint32_t)) {
        uint32_t word = atomic_load(&words[offset / sizeof(uint32_t)]);
        memcpy(p + offset, &word, sizeof(word));
    }
    return atomic_load(sequence) == before;
}

static void status(PhotoPhase phase, FxResult reason, uint32_t done, uint32_t total) {
    const PhotoStatus report = {worker.ticket, phase, reason, done, total};
    publish(&status_seq, status_words, &report, sizeof(report));
}

PhotoStatus photo_lut_status(void) {
    uint32_t ticket = atomic_load(&permit) | 1u;
    uint32_t value = mode(atomic_load(&activity));
    if (value == WORKER_FAILED) return (PhotoStatus){.ticket = ticket, .phase = PHOTO_WORKER_ERROR};
    if (value == PARKED) return (PhotoStatus){.ticket = ticket, .phase = PHOTO_NO_LUT};
    PhotoStatus out;
    if (snapshot(&status_seq, status_words, &out, sizeof(out)) && out.ticket == ticket) return out;
    return (PhotoStatus){.ticket = ticket, .phase = PHOTO_QUEUED};
}

uint32_t photo_lut_select(const char *name, uint64_t gui_epoch) {
    if (!io_gate || !name || (name[0] && !fx_z8_lut_path(name))) return 0;
    uint64_t state = atomic_load(&activity) & ~((uint64_t)WORKER_BUSY);
    uint32_t epoch = fx_z8_lut_io_epoch(io_gate);
    if (!(state & GUI_OPEN) || !state_drive(state) || !epoch ||
        ((state & (UINT64_MAX << 32)) | epoch) != gui_epoch ||
        epoch != atomic_load(&bound_io_epoch) || atomic_load(&media_unavailable)) return 0;
    uint32_t old = atomic_load(&permit), closed;
    if (!next_output(old, &closed) || (atomic_load(&activity) & ~((uint64_t)WORKER_BUSY)) != state ||
        fx_z8_lut_io_epoch(io_gate) != epoch || epoch != atomic_load(&bound_io_epoch) ||
        atomic_load(&media_unavailable) || !atomic_compare_exchange_strong(&permit, &old, closed))
        return 0;
    /* STOP and restore replace the entire ticket. A stale caller can never
     * close a newly admitted epoch by retrying its compare-and-exchange. */
    Selection next = {.ticket = closed | 1u};
    for (unsigned i = 0; name[i]; ++i) next.name[i] = name[i];
    publish(&selection_seq, selection_words, &next, sizeof(next));
    return next.ticket;
}

static void request_page(uint32_t op, const char *category, uint32_t focus) {
    uint32_t previous = atomic_load(&page_request) & ~PAGE_OP_MASK;
    PageCommand command = {.epoch = photo_gui_epoch(), .serial = previous + PAGE_OP_MASK + 1u + op,
                           .root_focus = focus};
    if (category) for (size_t i = 0; category[i]; ++i) command.category[i] = category[i];
    publish(&page_command_seq, page_command_words, &command, sizeof(command));
    atomic_store(&page_request, command.serial);
}

void photo_lut_request_page(int32_t direction) {
    request_page(direction < 0 ? PAGE_PREVIOUS : direction > 0 ? PAGE_NEXT : PAGE_OPEN, NULL, 0);
}

void photo_lut_enter_folder(const char *category, uint32_t root_focus) {
    if (category && (fx_z8_lut_category(category) || !photo_name_compare(category, PHOTO_FLAT_FOLDER)))
        request_page(PAGE_ENTER, category, root_focus);
}

void photo_lut_leave_folder(uint32_t root_focus) {
    request_page(PAGE_BACK, NULL, root_focus);
}

void photo_lut_close_browser(void) {
    request_page(PAGE_CLOSE, NULL, 0);
}

int photo_lut_page(PhotoPage *out) {
    uint32_t epoch = atomic_load(&restore_ticket);
    if (!live_drive() || !io_gate || !photo_gui_epoch() ||
        atomic_load(&bound_io_epoch) != fx_z8_lut_io_epoch(io_gate) ||
        !snapshot(&page_seq, page_words, out, sizeof(*out))) return 0;
    /* Page serial equals the exact GUI request. A request made while the
     * worker is between I/O slices must disable stale touch targets. */
    uint32_t request = atomic_load(&page_request);
    return (request & PAGE_OP_MASK) != PAGE_CLOSE && out->epoch == epoch &&
        epoch == atomic_load(&restore_ticket) && out->serial == request;
}

static int storage_current(const Worker *w) {
    return w->io_epoch && fx_z8_lut_io_epoch(io_gate) == w->io_epoch &&
           live_drive() == w->drive && worker_card_drive() == w->drive;
}

static int work_current(void *context) {
    const Worker *w = context;
    return (atomic_load(&permit) | 1u) == w->ticket && storage_current(w);
}

static int io_admit(Worker *w, int journal) {
    if (!storage_current(w) || !fx_z8_lut_io_take(io_gate, w->io_epoch)) return 0;
    if (journal ? work_current(w) : storage_current(w)) return 1;
    fx_z8_lut_io_drop(io_gate);
    return 0;
}

static FxResult finish_io(Worker *w, uint32_t native_error, int read) {
    fx_z8_lut_io_drop(io_gate);
    if (!native_error) return FX_OK;
    /* Nikon's volume readiness paths return these three errors. Other
     * write-side failures, such as a full card, only fail the save. */
    int mount_lost = native_error == UINT32_C(0x80001005) || native_error == UINT32_C(0x80001003) ||
                     native_error == UINT32_C(0x88000105);
    if ((read || mount_lost) && storage_current(w)) w->media_fault = 1;
    return FX_IO;
}

static int owns_handle(const Worker *w) {
    /* Owner-side close remains allowed after OFF in the same epoch. A
     * different observed drive/epoch cannot prove ownership of the descriptor. */
    return w->io_epoch && worker_card_drive() == w->drive &&
           (__atomic_load_n(&io_gate->state, __ATOMIC_ACQUIRE) >> 32) == w->io_epoch;
}

static void retain_handle(Worker *w, int32_t handle, uint32_t kind) {
    RetainedHandle *retirement = kind == FILE_RETAINED ? &w->file_retained : &w->directory_retained;
    *retirement = (RetainedHandle){handle, w->io_epoch, w->drive};
    atomic_fetch_or(&retained, kind);
    /* A nonzero close result does not prove the native descriptor survived.
     * Keep the mount-bound ownership visible and never close that number again. */
    w->media_fault = 1;
}

static FxResult file_open(void *context, const char *path, uint32_t flags, int32_t *fd) {
    Worker *w = context;
    *fd = -1;
    if (atomic_load(&retained) & FILE_RETAINED) return FX_CLOSE;
    if (!io_admit(w, 1)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_OPEN);
    *fd = CARD_OPEN(path, flags, 0);
    worker_step_heartbeat();
    if ((uint32_t)*fd == UINT32_C(0x80000010)) {
        fx_z8_lut_io_drop(io_gate);
        return FX_EMPTY;
    }
    /* Keep the opening admission pinned for the entire descriptor lifetime.
     * Early OFF can wait for the worker's safe owner-side close. */
    return *fd < 0 ? finish_io(w, (uint32_t)*fd, flags == 0) : FX_OK;
}

static FxResult file_read(void *context, int32_t fd, void *dst, uint32_t bytes, uint32_t *done) {
    Worker *w = context;
    if ((uintptr_t)dst & 63u) return FX_ARGUMENT;
    if (!io_admit(w, 1)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_READ);
    uint32_t rc = CARD_READ(fd, dst, bytes, done);
    worker_step_heartbeat();
    return finish_io(w, rc, 1);
}

static FxResult file_write(void *context, int32_t fd, const void *src, uint32_t bytes, uint32_t *done) {
    Worker *w = context;
    if (bytes != PHOTO_RECORD_BYTES) return FX_ARGUMENT;
    memcpy(write_buffer, src, bytes);
    /* Keep all possibly touched write-cache lines caller owned. */
    worker_card_heartbeat(FX_Z8_HB_CARD_CLEAN);
    FxResult rc = CARD_CLEAN(NULL, (uintptr_t)write_buffer, sizeof(write_buffer), FX_MACHINE_CLEAN);
    worker_step_heartbeat();
    if (rc != FX_OK) return rc;
    if (!io_admit(w, 1)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_WRITE);
    uint32_t result = CARD_WRITE(fd, write_buffer, bytes, done, 0);
    worker_step_heartbeat();
    return finish_io(w, result, 0);
}

static FxResult file_close(void *context, int32_t fd) {
    Worker *w = context;
    int closed = 0;
    if (owns_handle(w)) {
        worker_card_heartbeat(FX_Z8_HB_CARD_CLOSE);
        closed = !CARD_CLOSE(fd);
        worker_step_heartbeat();
    }
    fx_z8_lut_io_drop(io_gate);
    if (closed) return FX_OK;
    retain_handle(w, fd, FILE_RETAINED);
    return FX_CLOSE;
}

static FxResult file_remove(void *context, const char *path) {
    Worker *w = context;
    if (!io_admit(w, 1)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_REMOVE);
    uint32_t rc = CARD_REMOVE(path);
    worker_step_heartbeat();
    return finish_io(w, rc, 0);
}

static FxResult file_rename(void *context, const char *from, const char *to) {
    Worker *w = context;
    if (!io_admit(w, 1)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_RENAME);
    uint32_t rc = CARD_RENAME(from, to);
    worker_step_heartbeat();
    return finish_io(w, rc, 0);
}

static const PhotoFileIO files = {
    &worker, work_current, file_open, file_read, file_write, file_close, file_remove, file_rename
};

static int crash_current(uint64_t state, uint32_t epoch, uint32_t reason) {
    return epoch && (state >> 32) == epoch && !(state & FX_Z8_LUT_IO_EARLY_OFF) &&
           ((state & 1u) || ((reason & CRASH_TERMINAL) && (state & FX_Z8_LUT_IO_STOPPED)));
}

static int crash_pending(void) {
    uint64_t request = atomic_load(&crash_flush_request);
    return (request & CRASH_TERMINAL) && request != atomic_load(&crash_exhausted) &&
           crash_current(__atomic_load_n(&crash_gate()->state, __ATOMIC_ACQUIRE),
                         atomic_load(&crash_mount_epoch), (uint32_t)request);
}

/* STOP and FAULT close normal LUT admission before the worker can run. This narrow
 * exception holds card references in the same mount epoch and never crosses
 * the physical Early OFF marker. Each pathname/data operation admits anew. */
static int crash_admit(FxZ8LutIoGate *gate, uint32_t epoch, uint32_t reason) {
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
        if (!crash_current(state, epoch, reason) || (state & FX_Z8_LUT_IO_CARD) == FX_Z8_LUT_IO_CARD) return 0;
        worker_card_heartbeat(FX_Z8_HB_CARD_DRIVE);
        if (CARD_DRIVE() != 'B') return 0;
        if (__atomic_compare_exchange_n(&gate->state, &state, state + 2u, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    }
    return 0;
}

static uint32_t crash_open(FxZ8LutIoGate *gate, uint32_t epoch, uint32_t reason,
                           const char *path, uint32_t flags, int32_t *fd) {
    if (!crash_admit(gate, epoch, reason)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_OPEN);
    *fd = CARD_OPEN(path, flags, 0);
    if (*fd >= 0) return FX_OK; /* This reference pins the handle until owner-close. */
    fx_z8_lut_io_drop(gate);
    return (uint32_t)*fd == UINT32_C(0x80000010) && !flags ? FX_EMPTY : (uint32_t)*fd;
}

static uint32_t crash_close(FxZ8LutIoGate *gate, uint32_t epoch, int32_t fd, int temp) {
    worker_card_heartbeat(FX_Z8_HB_CARD_DRIVE);
    char drive = CARD_DRIVE();
    worker_card_heartbeat(FX_Z8_HB_CARD_CLOSE);
    int owned = drive == 'B' && (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) >> 32) == epoch;
    uint32_t rc = owned ? CARD_CLOSE(fd) : FX_STALE;
    fx_z8_lut_io_drop(gate);
    /* An ambiguous close forbids another close of the old descriptor.
     * Leave its temporary path alone for this mount and retry with a fresh one. */
    if (rc && temp) ++crash_retry.temp_index;
    return rc;
}

static uint32_t crash_exists(FxZ8LutIoGate *gate, uint32_t epoch, uint32_t reason,
                             const char *path, int temp) {
    int32_t fd;
    uint32_t rc = crash_open(gate, epoch, reason, path, 0, &fd);
    return rc == FX_OK ? crash_close(gate, epoch, fd, temp) : rc;
}

static uint32_t crash_remove(FxZ8LutIoGate *gate, uint32_t epoch, uint32_t reason,
                             const char *path, int temp) {
    uint32_t rc = crash_exists(gate, epoch, reason, path, temp);
    if (rc == FX_EMPTY) return FX_OK;
    if (rc != FX_OK) return rc;
    if (!crash_admit(gate, epoch, reason)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_REMOVE);
    uint32_t result = CARD_REMOVE(path);
    fx_z8_lut_io_drop(gate);
    return result;
}

static uint32_t crash_rename(FxZ8LutIoGate *gate, uint32_t epoch, uint32_t reason,
                             const char *from, const char *to) {
    if (!crash_admit(gate, epoch, reason)) return FX_STALE;
    worker_card_heartbeat(FX_Z8_HB_CARD_RENAME);
    uint32_t result = CARD_RENAME(from, to);
    fx_z8_lut_io_drop(gate);
    return result;
}

static uint32_t crash_write(FxZ8LutIoGate *gate, uint32_t epoch, uint32_t reason, uint32_t bytes) {
    static const char log[] = "B:\\FENESTRA\\FXCRASH.LOG";
    static const char old[] = "B:\\FENESTRA\\FXCRASH.OLD";
    char temp[] = "B:\\FENESTRA\\FXCRASH.TMP";
    if (crash_retry.temp_index > 9u) return FX_CLOSE;
    if (crash_retry.temp_index) temp[sizeof(temp) - 2u] = (char)('0' + crash_retry.temp_index);
    uint32_t rc = crash_remove(gate, epoch, reason, temp, 1);
    if (rc != FX_OK) return rc;
    int32_t fd;
    rc = crash_open(gate, epoch, reason, temp, 0x101u, &fd);
    if (rc != FX_OK) return rc;
    uint32_t done = 0, result = FX_STALE;
    if (crash_admit(gate, epoch, reason)) {
        worker_card_heartbeat(FX_Z8_HB_CARD_WRITE);
        result = CARD_WRITE(fd, write_buffer, bytes, &done, 0);
        fx_z8_lut_io_drop(gate);
    }
    rc = crash_close(gate, epoch, fd, 1);
    if (rc != FX_OK) return rc;
    if (result) return result;
    if (done != bytes) return FX_IO;
    uint32_t previous = crash_exists(gate, epoch, reason, log, 0);
    if (previous == FX_OK) {
        rc = crash_remove(gate, epoch, reason, old, 0);
        if (rc != FX_OK) return rc;
        rc = crash_rename(gate, epoch, reason, log, old);
        if (rc != FX_OK) return rc;
    } else if (previous != FX_EMPTY) return previous;
    rc = crash_rename(gate, epoch, reason, temp, log);
    if (rc != FX_OK && previous == FX_OK && crash_exists(gate, epoch, reason, log, 0) == FX_EMPTY)
        (void)crash_rename(gate, epoch, reason, old, log);
    return rc;
}

static void crash_slice(char drive) {
    FxZ8LutIoGate *gate = crash_gate();
    uint64_t state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    uint32_t epoch = atomic_load(&crash_mount_epoch);
    uint32_t observed_epoch = (uint32_t)(state >> 32);
    if (drive == 'B' && (state & 1u) && observed_epoch > epoch &&
        atomic_compare_exchange_strong(&crash_mount_epoch, &epoch, observed_epoch)) epoch = observed_epoch;
    uint64_t request = atomic_load_explicit(&crash_flush_request, memory_order_acquire);
    uint32_t pending = (uint32_t)request;
    if (!pending) return;
    uint32_t reason = pending & PHOTO_CRASH_FAULT ? PHOTO_CRASH_FAULT :
                      pending & PHOTO_CRASH_STOP ? PHOTO_CRASH_STOP :
                      pending & PHOTO_CRASH_STALL ? PHOTO_CRASH_STALL :
                      pending & PHOTO_CRASH_DEADLINE ? PHOTO_CRASH_DEADLINE : PHOTO_CRASH_BURST;
    if (drive != 'B' && !(reason & CRASH_TERMINAL)) return;
    if (!crash_current(state, epoch, reason)) return;
    uint64_t now = photo_flight_ms();
    int new_epoch = epoch != crash_retry.epoch;
    if (request != crash_retry.request || new_epoch) {
        if (new_epoch) {
            crash_retry.temp_index = 0;
            if (reason & CRASH_TERMINAL) photo_flight_flushed(0u);
        }
        if (new_epoch || !crash_retry.request) crash_retry.since_ms = now;
        crash_retry.epoch = epoch;
        crash_retry.request = request;
        crash_retry.next_ms = now;
        crash_retry.attempts = 0;
    }
    if (crash_retry.attempts >= CRASH_MAX_ATTEMPTS || now < crash_retry.next_ms) return;
    FxZ8JpegFlightMetrics jpeg_metrics = {0};
    FxZ8RearFlightMetrics rear_metrics = {0};
    if (jpeg) (void)jpeg(Z8_LUT_FLIGHT_METRICS, &jpeg_metrics);
    if (reason == PHOTO_CRASH_BURST && jpeg_metrics.active_exposures &&
        now - crash_retry.since_ms < CRASH_BURST_DEFER_MS) return;
    if (rear) (void)rear(REAR_FLIGHT_METRICS, &rear_metrics);
    ++crash_retry.attempts;
    ++photo_watchdog_status.write_attempts;
    uint32_t bytes = photo_flight_format((char *)write_buffer, reason, pending, now, &jpeg_metrics, &rear_metrics);
    worker_card_heartbeat(FX_Z8_HB_CARD_CLEAN);
    uint32_t error = bytes ? CARD_CLEAN(NULL, (uintptr_t)write_buffer, (bytes + 63u) & ~63u, FX_MACHINE_CLEAN) : FX_BOUNDS;
    if (!error) {
        worker_card_heartbeat(FX_Z8_HB_CRASH_WRITE);
        error = crash_write(gate, epoch, reason, bytes);
    }
    worker_step_heartbeat();
    uint64_t finished = photo_flight_ms();
    photo_watchdog_status.last_write_ms = (uint32_t)finished;
    photo_watchdog_status.last_write_err = error;
    if (error) {
        crash_retry.next_ms = finished + CRASH_BACKOFF_MS;
        if (crash_retry.attempts >= CRASH_MAX_ATTEMPTS) atomic_store(&crash_exhausted, request);
        return;
    }
    ++photo_watchdog_status.write_successes;
    int acknowledged = atomic_compare_exchange_strong(&crash_flush_request, &request, request & (UINT64_MAX << 32));
    if (acknowledged) crash_retry.request = 0;
    photo_flight_flushed(acknowledged ? pending : 0u);
}

static int close_directory(void) {
    int ok = 1;
    if (atomic_load(&directory_owned)) {
        uint32_t rc = FX_STALE;
        if (owns_handle(&worker)) {
            worker_card_heartbeat(FX_Z8_HB_DIR_CLOSE);
            rc = DIR_CLOSE(worker.directory);
            worker_step_heartbeat();
        }
        if (rc) {
            retain_handle(&worker, worker.directory, DIRECTORY_RETAINED);
            ok = 0;
        }
        fx_z8_lut_io_drop(io_gate);
        atomic_store(&directory_owned, 0);
    }
    return ok;
}

static void publish_page(uint32_t request) {
    worker.page.serial = request;
    worker.page.epoch = worker.epoch;
    publish(&page_seq, page_words, &worker.page, sizeof(worker.page));
}

static FxResult open_directory(void) {
    if (atomic_load(&retained) & DIRECTORY_RETAINED || !io_admit(&worker, 0)) return FX_STALE;
    char path[PHOTO_PATH_BYTES] = "A:\\FENESTRA";
    if (worker.scan == ROOT_FOLDERS) photo_card_path(worker.drive, "LUTS", path);
    else if (worker.scan == FOLDER_FILES && worker.category[0] != PHOTO_FLAT_FOLDER[0]) {
        char relative[FX_Z8_LUT_PATH_BYTES];
        photo_folder_path(worker.category, "", relative);
        photo_card_path(worker.drive, relative, path);
    } else path[0] = worker.drive;
    worker.directory = -1;
    worker_card_heartbeat(FX_Z8_HB_DIR_OPEN);
    uint32_t opened = DIR_OPEN(path, &worker.directory);
    worker_step_heartbeat();
    if (opened || worker.directory < 0) {
        if (opened == UINT32_C(0x80000010) && worker.directory < 0) {
            fx_z8_lut_io_drop(io_gate);
            return FX_EMPTY;
        }
        FxResult error = finish_io(&worker, opened ? opened : 1u, 1);
        if (worker.directory >= 0) retain_handle(&worker, worker.directory, DIRECTORY_RETAINED);
        return error;
    }
    atomic_store(&directory_owned, 1);
    return FX_OK;
}

static void restore(void);

static void clear_cache(void) {
    if (worker.cache) HEAP_FREE(worker.cache);
    worker.cache = NULL;
    worker.cache_ready = worker.cache_count = worker.cache_base = 0;
}

static void cache_add(const char name[FX_Z8_LUT_NAME_BYTES]) {
    unsigned low = 0, high = worker.cache_count;
    while (low < high) {
        unsigned middle = (low + high) / 2u;
        int order = photo_name_compare(name, worker.cache[middle]);
        if (!order) {
            if (memcmp(name, worker.cache[middle], FX_Z8_LUT_NAME_BYTES) < 0)
                memcpy(worker.cache[middle], name, FX_Z8_LUT_NAME_BYTES);
            return;
        }
        if (order < 0) high = middle;
        else low = middle + 1u;
    }
    if (worker.cache_count == PHOTO_CACHE_FILES) {
        clear_cache();
        return;
    }
    for (unsigned i = worker.cache_count; i > low; --i)
        fx_ram_copy(worker.cache[i], worker.cache[i - 1], FX_Z8_LUT_NAME_BYTES);
    memcpy(worker.cache[low], name, FX_Z8_LUT_NAME_BYTES);
    ++worker.cache_count;
}

static void cache_page(int direction) {
    if (direction > 0) worker.cache_base += PHOTO_FILES_PER_PAGE;
    if (direction < 0) worker.cache_base -= PHOTO_FILES_PER_PAGE;
    uint32_t remaining = worker.cache_count - worker.cache_base;
    memset(&worker.page, 0, sizeof(worker.page));
    worker.page.count = remaining < PHOTO_FILES_PER_PAGE ? remaining : PHOTO_FILES_PER_PAGE;
    worker.page.previous = worker.cache_base != 0;
    worker.page.next = remaining > PHOTO_FILES_PER_PAGE;
    memcpy(worker.page.names, worker.cache + worker.cache_base, worker.page.count * FX_Z8_LUT_NAME_BYTES);
}

static void scan_begin(int direction) {
    unsigned cursor = direction > 0 ? worker.page.count - 1u : 0u;
    photo_catalog_start(&worker.catalog, direction, worker.page.names[cursor]);
    worker.scan = worker.category[0] ? FOLDER_FILES : ROOT_FOLDERS;
}

static void publish_browser(uint32_t request) {
    worker.page.level = worker.category[0] ? PHOTO_FOLDER : PHOTO_ROOT;
    memcpy(worker.page.category, worker.category, sizeof(worker.category));
    publish_page(request);
}

static void scan_complete(uint32_t request, int ok) {
    if (ok && worker.scan == ROOT_FOLDERS) {
        worker.scan = ROOT_FIND_FLAT;
        return;
    }
    worker.scan = NO_SCAN;
    if (!ok) {
        clear_cache();
        memset(&worker.page, 0, sizeof(worker.page));
    } else if (worker.cache) {
        worker.cache_ready = 1;
        cache_page(0);
    } else if (!worker.catalog.count && worker.catalog.direction) {
        /* A file or category may have disappeared since the prior scan. */
        scan_begin(0);
        return;
    } else photo_catalog_finish(&worker.catalog, &worker.page);
    publish_browser(request);
    if (ok && !atomic_load(&retained) && atomic_exchange(&media_unavailable, 0)) {
        worker.job = NO_JOB;
        restore();
    }
}

static int accept_page_request(uint32_t request) {
    PageCommand command;
    if (!snapshot(&page_command_seq, page_command_words, &command, sizeof(command)) || command.serial != request)
        return 0;
    (void)close_directory();
    worker.scan = NO_SCAN;
    worker.page_seen = request;
    uint32_t op = request & PAGE_OP_MASK;
    if (op != PAGE_CLOSE && command.epoch != photo_gui_epoch()) op = PAGE_OPEN;
    switch (op) {
    case PAGE_ENTER:
        if (worker.category[0] || !command.root_focus || command.root_focus > worker.page.count ||
            photo_name_compare(command.category, worker.page.names[command.root_focus - 1u])) break;
        root_page = worker.page;
        memcpy(worker.category, command.category, sizeof(worker.category));
        worker.cache = HEAP_ALLOC(PHOTO_CACHE_FILES * FX_Z8_LUT_NAME_BYTES, 0);
        scan_begin(0);
        return 1;
    case PAGE_BACK:
        if (worker.category[0]) worker.page = root_page;
        memset(worker.category, 0, sizeof(worker.category));
        clear_cache();
        worker.page.focus = command.root_focus <= worker.page.count ? command.root_focus : 0;
        break;
    case PAGE_PREVIOUS: case PAGE_NEXT: {
        int direction = op == PAGE_PREVIOUS ? -1 : 1;
        if (!worker.page.count || (direction < 0 ? !worker.page.previous : !worker.page.next)) break;
        if (worker.cache_ready) { cache_page(direction); break; }
        scan_begin(direction);
        return 1;
    }
    case PAGE_OPEN: case PAGE_CLOSE:
        memset(worker.category, 0, sizeof(worker.category));
        clear_cache();
        if (op == PAGE_CLOSE) return 0;
        scan_begin(0);
        return 1;
    default: return 0;
    }
    publish_browser(request);
    return 0;
}

static void directory_slice(void) {
    uint32_t request = atomic_load(&page_request);
    if (request != worker.page_seen && !accept_page_request(request)) return;
    for (unsigned i = 0; i < 8 && worker.scan; ++i) {
        if (request != atomic_load(&page_request)) return;
        if (!atomic_load(&directory_owned)) {
            FxResult opened = open_directory();
            if (opened != FX_OK) {
                scan_complete(request, opened == FX_EMPTY);
                continue;
            }
        }
        if (request != atomic_load(&page_request) || !io_admit(&worker, 0)) { close_directory(); return; }
        memset(entry_buffer, 0, sizeof(entry_buffer));
        worker_card_heartbeat(FX_Z8_HB_DIR_NEXT);
        uint32_t rc = DIR_NEXT(worker.directory, entry_buffer);
        worker_step_heartbeat();
        int eof = rc == UINT32_C(0x80000030);
        (void)finish_io(&worker, eof ? 0 : rc, 1);
        if (rc) {
            scan_complete(request, close_directory() && eof);
            continue;
        }
        char name[FX_Z8_LUT_NAME_BYTES];
        int folders = worker.scan == ROOT_FOLDERS;
        if (!photo_dir_name(entry_buffer, worker.category, folders, name)) continue;
        if (worker.scan == ROOT_FIND_FLAT) {
            const char flat[FX_Z8_LUT_NAME_BYTES] = PHOTO_FLAT_FOLDER;
            photo_catalog_add(&worker.catalog, flat);
            scan_complete(request, close_directory());
        } else {
            photo_catalog_add(&worker.catalog, name);
            if (worker.cache) cache_add(name);
        }
    }
}

static void restore(void) {
    PhotoJournal journal;
    FxResult rc = photo_journal_load(&files, worker.drive, &journal);
    if (!work_current(&worker)) return;
    if (rc == FX_EMPTY || (rc == FX_OK && !journal.choice.path[0])) {
        status(PHOTO_NO_LUT, FX_OK, 0, 0);
        return;
    }
    if (rc != FX_OK) { status(PHOTO_JOURNAL_ERROR, rc, 0, 0); return; }
    rc = photo_file_exists(&files, worker.drive, journal.choice.path);
    if (!work_current(&worker)) return;
    if (rc != FX_OK) {
        status(rc == FX_EMPTY ? PHOTO_MISSING : PHOTO_LOAD_ERROR, rc, 0, 0);
        return;
    }
    memcpy(worker.name, journal.choice.path, sizeof(worker.name));
    worker.restore = 1;
    worker.job = DRAIN;
    status(PHOTO_QUEUED, FX_OK, 0, 0);
}

static void failed(FxResult rc) {
    uint32_t opened = worker.ticket;
    (void)atomic_compare_exchange_strong(&permit, &opened, worker.ticket & ~1u);
    worker.save_result = rc;
    status(rc == FX_EMPTY ? PHOTO_MISSING : PHOTO_LOAD_ERROR, rc, 0, 0);
    if (rc == FX_IO || rc == FX_CLOSE || rc == FX_STALE) worker.media_fault = 1;
    worker.job = CLEAN_ERROR;
}

static FxResult stop_jpeg(void) {
    if (worker.jpeg_drained) return FX_OK;
    FxResult rc = (FxResult)jpeg(Z8_LUT_STOP, NULL);
    if (rc == FX_OK) worker.jpeg_drained = 1;
    return rc;
}

static FxResult retire_rear(uint32_t operation) {
    FxResult rc = (FxResult)rear(operation, NULL);
    worker.rear_retiring = rc == FX_BUSY;
    return rc;
}

static FxResult prepare_jpeg(FxZ8LutOutput output) {
    if (jpeg_caps & FX_Z8_LUT_CAP_PATH) {
        FxZ8LutHeldPath request = {.output = output, .drive = worker.drive,
                                  .io = io_gate, .io_epoch = worker.io_epoch};
        memcpy(request.path, worker.name, sizeof(request.path));
        return (FxResult)jpeg(Z8_LUT_PREPARE_PATH, &request);
    }
    if (!fx_z8_lut_name(worker.name)) return FX_CAPABILITY;
    FxZ8LutHeldSelection request = {.selection.enabled = 1, .output = output, .drive = worker.drive,
                                    .io = io_gate, .io_epoch = worker.io_epoch};
    memcpy(request.selection.name, worker.name, sizeof(request.selection.name));
    return (FxResult)jpeg(Z8_LUT_PREPARE, &request);
}

static int prepare_job(void) {
    FxZ8LutOutput output = {(const uint32_t *)&permit, worker.ticket};
    FxResult rc;
    switch (worker.job) {
    case DRAIN:
        rc = stop_jpeg();
        if (rc == FX_BUSY) break;
        if (rc != FX_OK) { failed(rc); break; }
        rc = retire_rear(worker.name[0] ? REAR_SWITCH : REAR_RETIRE);
        if (rc == FX_BUSY) {
            /* No LUT is already bypassed and JPEG stopped. The journal
             * commit does not depend on the old LCD bank's VSync fence. */
            if (!worker.name[0]) worker.job = SAVE;
            else if (++worker.rear_wait >= REAR_RETRY_SLICES) failed(FX_TIMEOUT);
            break;
        }
        if (rc != FX_OK) { failed(rc); break; }
        worker.job = worker.name[0] ? PREPARE : SAVE;
        break;
    case PREPARE:
        worker.jpeg_drained = 0;
        rc = prepare_jpeg(output);
        if (rc == FX_OK) worker.job = STEP;
        else if (rc != FX_BUSY) failed(rc);
        break;
    case STEP: {
        FxZ8LutProgress progress;
        uint32_t phase = jpeg(Z8_LUT_STEP, NULL);
        if (!work_current(&worker)) break;
        if (jpeg(Z8_LUT_PROGRESS, &progress) != FX_OK || progress.version != FX_Z8_LUT_PROGRESS_VERSION) {
            failed(FX_INTEGRITY);
            break;
        }
        if (progress.phase == FX_Z8_LUT_READY) worker.job = COPY;
        else if (progress.phase == FX_Z8_LUT_ERROR) failed((FxResult)progress.result);
        else if (progress.phase == FX_Z8_LUT_READING) status(PHOTO_READING, FX_OK, 0, 0);
        else if (progress.phase == FX_Z8_LUT_PREPARING)
            status(PHOTO_PREPARING, FX_OK, progress.work_done, progress.work_total);
        else if (phase == Z8_LUT_OFF || phase == Z8_LUT_ERROR) failed(FX_STATE);
        break;
    }
    case COPY: {
        rc = (FxResult)rear(REAR_READY, &output);
        if (rc == FX_BUSY) break;
        if (rc != FX_OK) { failed(rc); break; }
        uint32_t closed = worker.ticket & ~1u;
        if (work_current(&worker) && atomic_compare_exchange_strong(&permit, &closed, worker.ticket)) {
            status(PHOTO_READY, FX_OK, 0, 0);
            worker.rear_presented = 0;
            worker.save_result = FX_OK;
            worker.job = worker.restore ? OUTPUT : SAVE;
        }
        break;
    }
    case SAVE:
        rc = photo_journal_save(&files, worker.drive, worker.name);
        if (!work_current(&worker)) break;
        worker.save_result = rc;
        if (rc == FX_STALE) worker.media_fault = 1;
        if (worker.name[0]) {
            status(rc == FX_OK ? PHOTO_READY : PHOTO_SAVE_ERROR, rc, 0, 0);
            worker.job = OUTPUT;
        } else {
            PhotoPhase settled = worker.rear_retiring ? PHOTO_CLEAR_RETIRING : PHOTO_NO_LUT;
            status(rc == FX_OK ? settled : PHOTO_CLEAR_SAVE_ERROR, rc, 0, 0);
            worker.job = NO_JOB;
        }
        break;
    case OUTPUT: {
        uint32_t phase = jpeg(Z8_LUT_STEP, NULL);
        if (!work_current(&worker)) break;
        if (phase == Z8_LUT_OFF || phase == Z8_LUT_ERROR) {
            FxZ8LutProgress progress;
            rc = FX_STATE;
            if (jpeg(Z8_LUT_PROGRESS, &progress) == FX_OK && progress.phase == FX_Z8_LUT_ERROR)
                rc = (FxResult)progress.result;
            failed(rc);
            break;
        }
        uint32_t result = rear(REAR_WORKER_STEP, NULL);
        if (!work_current(&worker)) break;
        rc = result == REAR_STREAMING ? FX_OK : (FxResult)result;
        if (rc == FX_STATE || rc == FX_INTEGRITY || rc == FX_CLOSE || rc == FX_BUDGET)
            failed(rc);
        else if (!worker.rear_presented && rear(REAR_PRESENTED, NULL)) {
            worker.rear_presented = 1;
            /* READY already denotes a prepared LUT. Expose independent LCD
             * presentation, which may never occur in EVF-only operation. */
            status(worker.save_result == FX_OK ? PHOTO_READY : PHOTO_SAVE_ERROR, worker.save_result, 1, 1);
        }
        return result == REAR_STREAMING;
    }
    case CLEAN_ERROR:
        if (stop_jpeg() == FX_BUSY) break;
        rc = retire_rear(REAR_SWITCH);
        if (rc != FX_BUSY) {
            /* Keep retrying full release after stock fences the old banks. */
            worker.rear_retiring = rc == FX_OK;
            worker.job = NO_JOB;
        }
        break;
    case NO_JOB:
        if (worker.rear_retiring && retire_rear(REAR_RETIRE) == FX_OK && !worker.name[0] && worker.save_result == FX_OK)
            status(PHOTO_NO_LUT, FX_OK, 0, 0);
        break;
    default:
        break;
    }
    return 0;
}

static void media_unavailable_now(void) {
    (void)close_directory();
    clear_cache();
    worker.scan = NO_SCAN;
    memset(worker.category, 0, sizeof(worker.category));
    worker.media_fault = 0;
    if (!atomic_exchange(&media_unavailable, 1u)) worker.ticket = close_output();
    worker.name[0] = 0;
    worker.save_result = atomic_load(&retained) ? FX_CLOSE : FX_STALE;
    worker.job = CLEAN_ERROR;
    memset(&worker.page, 0, sizeof(worker.page));
    publish_page(atomic_load(&page_request));
    status(PHOTO_JOURNAL_ERROR, worker.save_result, 0, 0);
}

static int drain_owner(void) {
    (void)close_directory();
    clear_cache();
    worker.scan = NO_SCAN;
    memset(worker.category, 0, sizeof(worker.category));
    FxResult rc = stop_jpeg();
    if (rc == FX_BUSY) return 0;
    /* Rear's lifecycle owns its retirement after STOP. While early OFF
     * still holds the registry open, the worker can nudge the same fence. */
    if (live_drive()) (void)retire_rear(REAR_RETIRE);
    atomic_store(&drain_needed, 0);
    return 1;
}

static int begin_epoch(char drive, uint32_t io_epoch, uint32_t epoch) {
    atomic_store(&bound_io_epoch, 0);
    if (!drain_owner()) return 0;
    if (worker.epoch == epoch) {
        /* OFF cancellation opens a fresh I/O epoch without a registry event. */
        epoch = close_output();
        atomic_store(&restore_ticket, epoch);
    }
    if (!epoch) return 0;
    worker.epoch = epoch;
    worker.io_epoch = io_epoch;
    worker.drive = drive;
    worker.page_seen = atomic_load(&page_request) ? UINT32_MAX : 0;
    worker.job = NO_JOB;
    worker.ticket = epoch;
    worker.rear_wait = 0;
    worker.name[0] = 0;
    memset(&worker.page, 0, sizeof(worker.page));
    atomic_store(&drain_needed, 1);
    atomic_store(&media_unavailable, 0);
    if (atomic_load(&retained)) media_unavailable_now();
    else restore();
    atomic_store(&bound_io_epoch, io_epoch);
    return 1;
}

static int work_slice(char drive) {
    uint32_t io_epoch = fx_z8_lut_io_epoch(io_gate);
    if (!drive || !io_epoch) {
        atomic_store(&bound_io_epoch, 0);
        (void)drain_owner();
        return 0;
    }
    if (worker_card_drive() != drive) {
        if (!atomic_load(&media_unavailable)) media_unavailable_now();
        (void)drain_owner();
        return 0;
    }
    uint32_t epoch = atomic_load(&restore_ticket);
    if ((worker.epoch != epoch || worker.io_epoch != io_epoch) && !begin_epoch(drive, io_epoch, epoch)) return 0;

    uint32_t ticket = atomic_load(&permit) | 1u;
    Selection posted;
    if (!atomic_load(&media_unavailable) && snapshot(&selection_seq, selection_words, &posted, sizeof(posted)) &&
        posted.ticket == ticket && posted.ticket != worker.ticket && storage_current(&worker)) {
        worker.ticket = posted.ticket;
        memcpy(worker.name, posted.name, sizeof(worker.name));
        worker.restore = 0;
        worker.rear_wait = 0;
        worker.job = DRAIN;
        status(PHOTO_QUEUED, FX_OK, 0, 0);
    }
    int streaming = work_current(&worker) && !worker.media_fault && prepare_job();
    if (storage_current(&worker) && !worker.media_fault) directory_slice();
    else close_directory();
    if (worker.media_fault) media_unavailable_now();
    return streaming && !atomic_load(&media_unavailable);
}

int photo_worker_slice(void) {
    uint64_t state = atomic_load(&activity);
    char drive = state_drive(state);
    int drain = mode(state) == PARKED && atomic_load(&drain_needed);
    if ((state & WORKER_BUSY) || (!drive && !drain && mode(state) != ARMED && !crash_pending())) return 0;
    if (!atomic_compare_exchange_strong(&activity, &state, state | WORKER_BUSY)) return 0;
    photo_flight_watchdog();
    worker_step_heartbeat();
    crash_slice(drive);
    int streaming = io_gate && (drive || drain) && work_slice(drive);
    fx_z8_heartbeat_idle(FX_Z8_HB_WORKER);
    atomic_fetch_and(&activity, ~((uint64_t)WORKER_BUSY));
    return streaming;
}

static void worker_task(void *context) {
    (void)context;
    for (;;) {
        if (!photo_worker_slice()) (void)TASK_DELAY(live_drive() ? 2u : 20u);
    }
}

static int boot_path(const char *path, char drive) {
    static const char tail[] = ":\\FENESTRA\\boot-z8.flag";
    if (!path || (drive != 'A' && drive != 'B') || path[0] != drive) return 0;
    for (size_t i = 0; i < sizeof(tail); ++i)
        if (path[i + 1] != tail[i]) return 0;
    return 1;
}

uint64_t photo_open_arm(const char *path) {
    uint64_t state = atomic_load(&activity);
    if ((uint32_t)state != (ARMED | GUI_OPEN)) return 0;
    uint32_t io_epoch = fx_z8_lut_io_epoch(crash_gate());
    char drive = CARD_DRIVE();
    if (!io_epoch || !boot_path(path, drive) || atomic_load(&activity) != state) return 0;
    return (state >> 32) << 34 | ((uint64_t)io_epoch << 2) | (drive == 'A' ? DRIVE_A : DRIVE_B);
}

void photo_open_complete(uint64_t token) {
    if (!token) return;
    FxZ8LutIoGate *gate = crash_gate();
    uint32_t drive_state = (uint32_t)token & 3u;
    uint32_t io_epoch = (uint32_t)(token >> 2);
    uint64_t life = (token >> 34) << 32;
    char drive = state_drive(drive_state);
    if (!drive || CARD_DRIVE() != drive || fx_z8_lut_io_epoch(gate) != io_epoch) return;
    uint64_t expected = life | ARMED | GUI_OPEN;
    uint64_t claimed = life | drive_state | WORKER_BUSY | GUI_OPEN;
    if (!atomic_compare_exchange_strong(&activity, &expected, claimed)) return;
    if (fx_z8_lut_io_epoch(gate) != io_epoch) {
        expected = claimed;
        if (atomic_compare_exchange_strong(&activity, &expected, life | ARMED | GUI_OPEN)) return;
    } else {
        FxResult rc = FX_OK;
        if (!atomic_exchange(&task_attempted, 1))
            rc = TASK_START(&worker.task, worker_task, NULL, worker_stack, sizeof(worker_stack));
        else if (worker.task.phase != 2u) rc = FX_IO;
        if (rc != FX_OK) {
            expected = claimed;
            (void)atomic_compare_exchange_strong(&activity, &expected, life | WORKER_FAILED | WORKER_BUSY | GUI_OPEN);
        } else if (drive == 'B') atomic_store(&crash_mount_epoch, io_epoch);
    }
    atomic_fetch_and(&activity, ~((uint64_t)WORKER_BUSY));
}

static _Atomic uint32_t *const rate_table = (void *)(uintptr_t)0x40b51000;

static int rate_valid(void) {
    static const uint32_t expected[] = {0x52303443, 0x33305741, 0, 40000,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 140000};
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        uint32_t read = atomic_load(&rate_table[i]);
        if (i == 2 || i == 13) { if (read > 1) return 0; }
        else if (read != expected[i]) return 0;
    }
    return 1;
}

int photo_burst_rate(uint32_t row, int toggle) {
    if (row >= 2 || (toggle && !photo_gui_epoch()) || !rate_valid()) return -1;
    _Atomic uint32_t *slot = &rate_table[row ? 13 : 2];
    return (int)(toggle ? atomic_fetch_xor(slot, 1u) ^ 1u : atomic_load(slot));
}

static uint32_t pending(void *context) {
    (void)context;
    return (atomic_load(&activity) & WORKER_BUSY) || atomic_load(&directory_owned) ||
        (io_gate && (!live_drive() || !fx_z8_lut_io_epoch(io_gate)) && atomic_load(&drain_needed)) ||
        atomic_load(&retained) || crash_pending() || photo_ui_pending();
}

static uint64_t replace_lifecycle(uint32_t flags) {
    uint64_t old = atomic_load(&activity), next;
    do {
        if (((uint32_t)old & ~(uint32_t)WORKER_BUSY) == flags && !flags) return old;
        uint64_t serial = old >> 32;
        /* Startup tokens reserve 30 bits for applying this exact lifecycle. */
        if (serial < UINT32_C(0x3fffffff)) ++serial;
        else flags = PARKED;
        next = (serial << 32) | (old & WORKER_BUSY) | flags;
    } while (!atomic_compare_exchange_weak(&activity, &old, next));
    return old;
}

static FxResult event(void *context, uint32_t action) {
    (void)context;
    switch (action) {
    case FX_NATIVE_PACK_GUI_POLL:
        photo_ui_deactivate();
        return FX_OK;
    case FX_NATIVE_PACK_PUBLISH:
    case FX_NATIVE_PACK_RESUME:
        atomic_store(&crash_lifecycle, 0);
        atomic_store(&crash_mount_epoch, 0);
        atomic_fetch_and(&crash_flush_request, ~((uint64_t)CRASH_TERMINAL));
        photo_flight_reset_activity();
        if ((photo_feature_selection & 0x10u) && rate_valid()) {
            atomic_store(&rate_table[2], 0);
            atomic_store(&rate_table[13], 0);
        }
        if (photo_feature_selection & 0x400u) {
            uint32_t ticket = close_output();
            atomic_store(&restore_ticket, ticket);
            (void)replace_lifecycle((ticket ? ARMED : WORKER_FAILED) | GUI_OPEN);
        } else {
            fx_z8_lut_io_reopen(&burst_io, 0);
            (void)replace_lifecycle(ARMED | GUI_OPEN);
        }
        return FX_OK;
    case FX_NATIVE_PACK_STOP:
    case FX_NATIVE_PACK_FAULT:
    case FX_NATIVE_PACK_ABORT: {
        fx_z8_lut_io_close(crash_gate(), 1);
        uint64_t previous = replace_lifecycle(PARKED);
        if (previous & GUI_OPEN) (void)close_output();
        atomic_store(&bound_io_epoch, 0);
        if (action == FX_NATIVE_PACK_FAULT && !(atomic_fetch_or(&crash_lifecycle, CRASH_LOG_FAULTED) & CRASH_LOG_FAULTED)) {
            /* A cancelled Early OFF can reopen the gate between worker turns.
             * The returned lifecycle preserves the boot-confirmed drive B. */
            if (state_drive(previous) == 'B') {
                uint64_t io = __atomic_load_n(&crash_gate()->state, __ATOMIC_ACQUIRE);
                atomic_store(&crash_mount_epoch, (uint32_t)(io >> 32));
            }
            photo_flight_log(FX_Z8_FLIGHT_PACK_FAULT, photo_flight_core(), action, 0, 0);
            photo_crash_request(PHOTO_CRASH_FAULT);
        } else if (action == FX_NATIVE_PACK_STOP && !(atomic_fetch_or(&crash_lifecycle, CRASH_LOG_STOPPED) & CRASH_LOG_STOPPED)) {
            photo_flight_log(FX_Z8_FLIGHT_PACK_STOP, photo_flight_core(), action, 0, 0);
            if (photo_flight_had_activity() &&
                crash_current(__atomic_load_n(&crash_gate()->state, __ATOMIC_ACQUIRE),
                              atomic_load(&crash_mount_epoch), PHOTO_CRASH_STOP))
                photo_crash_request(PHOTO_CRASH_STOP);
        }
        if (action == FX_NATIVE_PACK_ABORT && atomic_load(&task_attempted)) return FX_BUSY;
        return pending(NULL) ? FX_BUSY : FX_OK;
    }
    default:
        return FX_ARGUMENT;
    }
}

FxResult fx_native_pack_entry(const FxNativePackHost *host, FxNativePackExports *out) {
    if (!host || !out || host->abi != FX_NATIVE_PACK_ABI || host->bytes < sizeof(*host)) return FX_ARGUMENT;
    if (photo_feature_selection & 0x400u) {
        uintptr_t jpeg_address = 0, rear_address = 0, gate_address = 0;
        if (!host->lookup ||
            host->lookup(host->context, FX_Z8_JPEG_LUT_WORKER, FX_Z8_JPEG_LUT_WORKER_VERSION, &jpeg_address) != FX_OK ||
            host->lookup(host->context, FX_Z8_REAR_LUT_WORKER, FX_Z8_REAR_LUT_WORKER_VERSION, &rear_address) != FX_OK ||
            host->lookup(host->context, FX_Z8_REAR_LUT_IO, FX_Z8_REAR_LUT_IO_VERSION, &gate_address) != FX_OK ||
            !jpeg_address || !rear_address || !gate_address || (gate_address & 7u)) return FX_INTEGRITY;
        jpeg = (FxZ8JpegLutSelected)jpeg_address;
        jpeg_caps = jpeg(Z8_LUT_CAPABILITIES, NULL);
        rear = (FxZ8JpegLutSelected)rear_address;
        io_gate = (FxZ8LutIoGate *)gate_address;
    }
    *out = (FxNativePackExports){.abi = FX_NATIVE_PACK_ABI, .bytes = sizeof(*out),
                                .context = &worker, .event = event, .pending = pending};
    return FX_OK;
}
