/* SPDX-License-Identifier: GPL-3.0-only */
#include "photo_menu.h"

#define FLIGHT_BUSY (UINT64_C(1) << 63)
#define STALL_TICKS (UINT64_C(2000) * FX_Z8_TICKS_PER_MS)

typedef struct {
    _Atomic uint64_t seq, ticks;
    _Atomic uint32_t event_core, a, b, c;
} FlightRecord;

typedef struct {
    uint64_t ticks;
    uint32_t event_core, a, b, c;
} FlightSample;

_Static_assert(sizeof(FlightRecord) == 32 && offsetof(FlightRecord, event_core) == 16,
               "workbench flight recorder record ABI");
_Alignas(64) FlightRecord photo_flight_ring[PHOTO_FLIGHT_COUNT] __attribute__((section(".flight.records")));
_Atomic uint64_t photo_flight_head __attribute__((section(".flight.head")));
_Static_assert(sizeof(FxZ8Heartbeat) == 32 && offsetof(FxZ8Heartbeat, enter_tick) == 16 &&
               sizeof(PhotoWatchdogStatus) == 32, "workbench heartbeat ABI");
FxZ8Heartbeat photo_heartbeat_slots[FX_Z8_HB_COUNT] __attribute__((section(".flight.heartbeats")));
PhotoWatchdogStatus photo_watchdog_status __attribute__((section(".flight.watchdog")));

_Atomic uint64_t crash_flush_request;
static _Atomic uint64_t fallback_ready[2];
static _Atomic uint32_t flight_activity;
static FxZ8Heartbeat stalled[FX_Z8_HB_COUNT];
static uint32_t stalled_ms[FX_Z8_HB_COUNT];
static uint32_t stall_pending_slots;
static struct {
    uint64_t first, present;
    FlightSample rows[PHOTO_FLIGHT_COUNT];
    uint32_t reason;
} frozen;

static uint64_t counter(void) {
    uint64_t ticks;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(ticks));
    return ticks;
}

uint64_t photo_flight_ms(void) {
    return counter() / FX_Z8_TICKS_PER_MS;
}

uint16_t photo_flight_core(void) {
    uint64_t id;
    __asm__ volatile("mrs %0,mpidr_el1" : "=r"(id));
    return (uint16_t)((id & 15u) + 1u);
}

void photo_crash_request(uint32_t reason) {
    /* The serial prevents a concurrent request with the same reason from
     * disappearing when the worker acknowledges a completed write. */
    uint64_t request = atomic_load_explicit(&crash_flush_request, memory_order_relaxed), next;
    do {
        next = (request + (UINT64_C(1) << 32)) | reason;
    } while (!atomic_compare_exchange_weak_explicit(&crash_flush_request, &request, next,
                                                     memory_order_release, memory_order_relaxed));
}

int photo_flight_had_activity(void) {
    return atomic_load_explicit(&flight_activity, memory_order_acquire) != 0;
}

void photo_flight_reset_activity(void) {
    atomic_store_explicit(&flight_activity, 0, memory_order_release);
}

void photo_flight_log(uint16_t event, uint16_t core_flags, uint32_t a, uint32_t b, uint32_t c) {
    uint64_t ticks = counter();
    uint64_t seq = atomic_fetch_add_explicit(&photo_flight_head, 1, memory_order_relaxed) + 1u;
    FlightRecord *slot = &photo_flight_ring[(seq - 1u) % PHOTO_FLIGHT_COUNT];
    uint64_t old = atomic_load_explicit(&slot->seq, memory_order_relaxed);
    /* A preempted owner can cost an overwrite of this slot. Never wait on it,
     * and never let a late owner overwrite a newer completed publication. */
    if (seq && seq < FLIGHT_BUSY && old < seq &&
        atomic_compare_exchange_weak_explicit(&slot->seq, &old, seq | FLIGHT_BUSY,
                                              memory_order_acq_rel, memory_order_relaxed)) {
        atomic_store_explicit(&slot->ticks, ticks, memory_order_relaxed);
        atomic_store_explicit(&slot->event_core, event | ((uint32_t)core_flags << 16), memory_order_relaxed);
        atomic_store_explicit(&slot->a, a, memory_order_relaxed);
        atomic_store_explicit(&slot->b, b, memory_order_relaxed);
        atomic_store_explicit(&slot->c, c, memory_order_relaxed);
        atomic_store_explicit(&slot->seq, seq, memory_order_release);
        if (((event >= FX_Z8_FLIGHT_JPEG_MAIN_ENTER && event <= FX_Z8_FLIGHT_JPEG_YIELD) ||
             event == FX_Z8_FLIGHT_STALL || event == FX_Z8_FLIGHT_SMP_QUARANTINE) &&
            !atomic_load_explicit(&flight_activity, memory_order_relaxed))
            atomic_store_explicit(&flight_activity, 1, memory_order_release);
    }
    if (event == FX_Z8_FLIGHT_JPEG_FALLBACK) {
        /* Resource saturation and a deadline have independent log budgets.
         * Routine contention is recorded in RAM without issuing card I/O. */
        if (core_flags & (FX_Z8_FLIGHT_DEADLINE | FX_Z8_FLIGHT_BURST_FALLBACK)) {
            unsigned deadline = (core_flags & FX_Z8_FLIGHT_DEADLINE) != 0;
            _Atomic uint64_t *ready = &fallback_ready[deadline];
            uint64_t earliest = atomic_load_explicit(ready, memory_order_relaxed);
            if (ticks >= earliest &&
                atomic_compare_exchange_strong_explicit(ready, &earliest, ticks + 10000u * FX_Z8_TICKS_PER_MS,
                                                        memory_order_relaxed, memory_order_relaxed))
                photo_crash_request(deadline ? PHOTO_CRASH_DEADLINE : PHOTO_CRASH_BURST);
        }
    }
}

static int sample(uint64_t seq, FlightSample *out) {
    const FlightRecord *slot = &photo_flight_ring[(seq - 1u) % PHOTO_FLIGHT_COUNT];
    if (atomic_load_explicit(&slot->seq, memory_order_acquire) != seq) return 0;
    out->ticks = atomic_load_explicit(&slot->ticks, memory_order_relaxed);
    out->event_core = atomic_load_explicit(&slot->event_core, memory_order_relaxed);
    out->a = atomic_load_explicit(&slot->a, memory_order_relaxed);
    out->b = atomic_load_explicit(&slot->b, memory_order_relaxed);
    out->c = atomic_load_explicit(&slot->c, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&slot->seq, memory_order_relaxed) == seq;
}

static void freeze_ring(uint32_t reason) {
    uint64_t last = atomic_load_explicit(&photo_flight_head, memory_order_acquire);
    if (last >= FLIGHT_BUSY) last = FLIGHT_BUSY - 1u;
    frozen.first = last > PHOTO_FLIGHT_COUNT ? last - PHOTO_FLIGHT_COUNT + 1u : 1u;
    frozen.present = 0;
    for (uint64_t seq = frozen.first; seq <= last; ++seq) {
        unsigned row = (unsigned)(seq - frozen.first);
        if (sample(seq, &frozen.rows[row])) frozen.present |= UINT64_C(1) << row;
    }
    frozen.reason = reason;
}

static const char *const event_names[] = {
    "UNKNOWN",
    "JMAIN", "JQUEUE", "JGRADE_IN", "JGRADE_OUT", "JENC_IN", "JENC_OUT", "JFALLBACK", "JYIELD",
    "ROUTE", "CAP_ABORT", "POWER_OFF", "PACK_FAULT", "PACK_STOP", "STALL", "SMP_QUAR"
};

static int heartbeat_sample(unsigned slot, FxZ8Heartbeat *out) {
    const FxZ8Heartbeat *hb = &photo_heartbeat_slots[slot];
    uint64_t entered = __atomic_load_n(&hb->enter_tick, __ATOMIC_ACQUIRE);
    if (entered & FX_Z8_HEARTBEAT_BUSY) return 0;
    out->stage = __atomic_load_n(&hb->stage, __ATOMIC_RELAXED);
    out->cap_num = __atomic_load_n(&hb->cap_num, __ATOMIC_RELAXED);
    out->core = __atomic_load_n(&hb->core, __ATOMIC_RELAXED);
    out->aux = __atomic_load_n(&hb->aux, __ATOMIC_RELAXED);
    out->enter_tick = entered;
    out->progress_tick = __atomic_load_n(&hb->progress_tick, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&hb->enter_tick, __ATOMIC_RELAXED) == entered;
}

static uint32_t elapsed_ms(uint64_t now, uint64_t then) {
    uint64_t ms = now >= then ? (now - then) / FX_Z8_TICKS_PER_MS : 0;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

void photo_flight_watchdog(void) {
    uint64_t now = counter();
    uint32_t newly_stalled = 0;
    for (unsigned slot = 0; slot < FX_Z8_HB_COUNT; ++slot) {
        FxZ8Heartbeat hb;
        if (!heartbeat_sample(slot, &hb) || !hb.stage || hb.enter_tick == stalled[slot].enter_tick ||
            now < hb.progress_tick || now - hb.progress_tick < STALL_TICKS) continue;
        stalled[slot] = hb;
        stalled_ms[slot] = elapsed_ms(now, hb.progress_tick);
        newly_stalled |= 1u << slot;
        photo_watchdog_status.stall_slot = slot;
        photo_watchdog_status.stall_stage = hb.stage;
        photo_watchdog_status.stall_ms = stalled_ms[slot];
        photo_flight_log(FX_Z8_FLIGHT_STALL, (uint16_t)hb.core, slot, hb.stage, hb.cap_num);
    }
    if (newly_stalled) {
        if (!stall_pending_slots) freeze_ring(PHOTO_CRASH_STALL);
        photo_watchdog_status.stall_latched |= newly_stalled;
        stall_pending_slots |= newly_stalled;
        photo_crash_request(PHOTO_CRASH_STALL);
    }
}

void photo_flight_flushed(uint32_t pending) {
    frozen.reason = 0;
    if (pending & PHOTO_CRASH_STALL) stall_pending_slots = 0;
}

typedef struct { char *next, *end; } FlightText;

static void append(FlightText *text, const char *s) {
    while (*s && text->next < text->end) *text->next++ = *s++;
}

static void number(FlightText *text, uint64_t value, unsigned base, unsigned width) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char reverse[20];
    unsigned n = 0;
    do {
        reverse[n++] = digits[value % base];
        value /= base;
    } while (value || n < width);
    while (n && text->next < text->end) *text->next++ = reverse[--n];
}

static void counter_value(FlightText *text, const char *label, uint32_t value) {
    append(text, label);
    number(text, value, 10, 0);
}

/* The fixed row prefix packs a relative sequence, low uptime word and core.
 * Unsigned base-36 payloads occupy at most seven digits each. */
uint32_t photo_flight_format(char *buffer, uint32_t reason, uint32_t pending, uint64_t now,
                             const FxZ8JpegFlightMetrics *jpeg, const FxZ8RearFlightMetrics *rear) {
    _Static_assert(1110u + PHOTO_FLIGHT_COUNT * (11u + 10u + 3u * 8u + 1u) < PHOTO_CRASH_LOG_BYTES,
                   "maximum crash record text fits the bounce buffer");
    FlightText text = {buffer, buffer + PHOTO_CRASH_LOG_BYTES};
    if (frozen.reason != reason) freeze_ring(reason);
    append(&text, "FXCRASH build=Z8-C311-v4-FR2 reason=");
    append(&text, reason == PHOTO_CRASH_FAULT ? "FAULT" :
                  reason == PHOTO_CRASH_STOP ? "STOP" :
                  reason == PHOTO_CRASH_STALL ? "STALL" :
                  reason == PHOTO_CRASH_DEADLINE ? "DEADLINE" : "BURST_FALLBACK");
    append(&text, " uptime_ms=");
    number(&text, now, 10, 0);
    counter_value(&text, "\nJPEG contention=", jpeg->contention_passes);
    counter_value(&text, " fallbacks=", jpeg->fallbacks);
    counter_value(&text, " wait_ms=", jpeg->wait_ms);
    counter_value(&text, " queue_full=", jpeg->queue_full);
    counter_value(&text, " deadline_ms=", jpeg->deadline_ms);
    counter_value(&text, " active_exposures=", jpeg->active_exposures);
    counter_value(&text, "\nREAR active_route=", rear->active_route);
    counter_value(&text, " grade_us=", rear->grade_us);
    counter_value(&text, " worker_cores=", rear->worker_cores);
    counter_value(&text, " neon_active=", rear->neon_active);
    counter_value(&text, " capture_aborts=", rear->capture_aborts);
    counter_value(&text, " timeouts=", rear->timeouts);
    append(&text, "\nHB: slot,stage,core,cap,stuck_ms,aux");
    uint64_t hb_now = counter();
    for (unsigned slot = 0; slot < FX_Z8_HB_COUNT; ++slot) {
        FxZ8Heartbeat hb;
        int latched = (pending & PHOTO_CRASH_STALL) && ((stall_pending_slots >> slot) & 1u);
        /* A delayed retry can outlive the stall itself. Preserve the stage
         * and capture that provoked the pending incident. */
        if (latched) hb = stalled[slot];
        else if (!heartbeat_sample(slot, &hb)) {
            counter_value(&text, " ", slot);
            append(&text, ",busy");
            continue;
        }
        counter_value(&text, " ", slot);
        counter_value(&text, ",", hb.stage);
        counter_value(&text, ",", hb.core);
        counter_value(&text, ",", hb.cap_num);
        uint32_t age = latched ? stalled_ms[slot] : hb.stage ? elapsed_ms(hb_now, hb.progress_tick) : 0u;
        counter_value(&text, ",", age);
        counter_value(&text, ",", hb.aux);
    }
    counter_value(&text, "\nWD latched=", photo_watchdog_status.stall_latched);
    counter_value(&text, " slot=", photo_watchdog_status.stall_slot);
    counter_value(&text, " stage=", photo_watchdog_status.stall_stage);
    counter_value(&text, " stall_ms=", photo_watchdog_status.stall_ms);
    counter_value(&text, " attempts=", photo_watchdog_status.write_attempts);
    counter_value(&text, " successes=", photo_watchdog_status.write_successes);
    counter_value(&text, " last_err=", photo_watchdog_status.last_write_err);
    counter_value(&text, " last_ms=", photo_watchdog_status.last_write_ms);
    append(&text, "\nseq_base=");
    number(&text, frozen.first, 10, 0);
    append(&text, "\nseq+ms32+core(hex) event a b c(base36)\n");
    for (unsigned i = 0; i < PHOTO_FLIGHT_COUNT; ++i) {
        if (!(frozen.present & (UINT64_C(1) << i))) continue;
        const FlightSample *row = &frozen.rows[i];
        unsigned event = row->event_core & 0xffffu;
        if (event >= sizeof(event_names) / sizeof(event_names[0])) event = 0;
        number(&text, i, 16, 2);
        number(&text, (uint32_t)(row->ticks / FX_Z8_TICKS_PER_MS), 16, 8);
        number(&text, (row->event_core >> 16) & 15u, 16, 1);
        append(&text, event_names[event]);
        append(&text, " ");
        number(&text, row->a, 36, 0);
        append(&text, " ");
        number(&text, row->b, 36, 0);
        append(&text, " ");
        number(&text, row->c, 36, 0);
        append(&text, "\n");
    }
    return text.next == text.end ? 0u : (uint32_t)(text.next - buffer);
}
