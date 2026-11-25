/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FENESTRA_FXLOG_H
#define FENESTRA_FXLOG_H

#include <stddef.h>
#include <stdint.h>

#define FXLOG_DIR UINT64_C(0x40bcf000)
#define FXLOG_ABI 1u
#define FXLOG_TICKS_PER_MS 24000u

struct fxlog_dir_slot {
    uint32_t state, abi;
    char id[16];
    uint64_t ring, heartbeat, build;
    uint32_t heartbeat_count;
    uint32_t reserved[3];
};

struct fxlog_directory {
    uint64_t magic;
    uint32_t abi, header_bytes, slot_bytes, slot_count, ticks_per_ms, reserved;
    uint64_t latest_ms;
    uint64_t reserved_tail[3];
    struct fxlog_dir_slot slots[32];
};

struct fxlog_ring_header {
    char magic[8];
    uint32_t entry_bytes, capacity;
    uint64_t head;
    uint32_t dropped, level_max;
    uint64_t reserved[4];
};

struct fxlog_entry {
    uint32_t seq, t_ms;
    uint64_t fmt;
    uint32_t a0, a1, a2;
    uint8_t level, core, nargs, flags;
};

struct fxlog_heartbeat {
    uint32_t stage, count, last_ms, core;
};

_Static_assert(offsetof(struct fxlog_directory, latest_ms) == 32 &&
               offsetof(struct fxlog_directory, slots) == 64 &&
               sizeof(struct fxlog_dir_slot) == 64, "fxlog directory ABI");
_Static_assert(sizeof(struct fxlog_ring_header) == 64 &&
               sizeof(struct fxlog_entry) == 32 &&
               sizeof(struct fxlog_heartbeat) == 16, "fxlog ring and heartbeat ABI");

static inline uint64_t fxlog_now_ms(void)
{
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks / FXLOG_TICKS_PER_MS;
}

static inline uint32_t fxlog_core(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 15u;
}

#define FXLOG_RW __attribute__((section(".fxlog.rw")))

/* Define once in the native translation unit that writes this plugin's log.
 * Link .fxlog.rw into plugin-owned RW storage and .rodata.fxlog into RO.
 */
#define FXLOG_DEFINE(id_str, selector_index, capacity, heartbeat_slots, build_id, level_max) \
    _Static_assert(sizeof("" id_str) <= 17, "fxlog plugin ID is at most 16 bytes"); \
    _Static_assert((selector_index) >= 0 && (selector_index) < 32, "invalid fxlog selector"); \
    _Static_assert((capacity) > 0 && ((capacity) & ((capacity) - 1)) == 0, \
                   "fxlog capacity must be a power of two"); \
    _Static_assert((heartbeat_slots) >= 0, "invalid fxlog heartbeat count"); \
    static struct { \
        struct fxlog_ring_header header; \
        struct fxlog_entry entries[(capacity)]; \
    } fxlog_ring FXLOG_RW __attribute__((aligned(64))) = { \
        .header = { "FXRING01", sizeof(struct fxlog_entry), (capacity), 0, 0, (level_max), {0} }, \
    }; \
    static struct fxlog_heartbeat fxlog_heartbeats[(heartbeat_slots) ? (heartbeat_slots) : 1] FXLOG_RW; \
    static uint32_t fxlog_registered FXLOG_RW; \
    static __attribute__((noinline, unused)) void fxlog_update_directory(uint64_t now_ms) \
    { \
        static const char id[16] = id_str; \
        struct fxlog_directory *const dir = (struct fxlog_directory *)(uintptr_t)FXLOG_DIR; \
        if (__atomic_load_n(&fxlog_registered, __ATOMIC_RELAXED) != 2u) { \
            /* Little-endian "FXLOGDIR". A missing directory leaves local storage usable. */ \
            if (__atomic_load_n(&dir->magic, __ATOMIC_RELAXED) != UINT64_C(0x524944474f4c5846) || \
                __atomic_load_n(&dir->abi, __ATOMIC_RELAXED) != FXLOG_ABI) \
                return; \
            uint32_t expected = 0; \
            if (__atomic_compare_exchange_n(&fxlog_registered, &expected, 1, 0, \
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { \
                struct fxlog_dir_slot *const slot = &dir->slots[(selector_index)]; \
                __atomic_store_n(&slot->state, 1, __ATOMIC_RELAXED); \
                slot->abi = FXLOG_ABI; \
                for (size_t i = 0; i < sizeof(slot->id); ++i) \
                    slot->id[i] = id[i]; \
                slot->ring = (uintptr_t)&fxlog_ring; \
                slot->heartbeat = (heartbeat_slots) ? (uintptr_t)fxlog_heartbeats : 0; \
                slot->build = (build_id); \
                slot->heartbeat_count = (heartbeat_slots); \
                __atomic_store_n(&slot->state, 2, __ATOMIC_RELEASE); \
                __atomic_store_n(&fxlog_registered, 2, __ATOMIC_RELAXED); \
            } \
        } \
        __atomic_store_n(&dir->latest_ms, now_ms, __ATOMIC_RELAXED); \
    } \
    static __attribute__((noinline, unused)) void fxlog_record(uint8_t level, const char *fmt, \
                                                             uint8_t nargs, uint32_t a0, \
                                                             uint32_t a1, uint32_t a2) \
    { \
        const uint64_t now_ms = fxlog_now_ms(); \
        const uint64_t head = __atomic_fetch_add(&fxlog_ring.header.head, 1, __ATOMIC_RELAXED); \
        struct fxlog_entry *const entry = &fxlog_ring.entries[head & ((capacity) - 1)]; \
        __atomic_store_n(&entry->seq, 0, __ATOMIC_RELAXED); \
        /* Invalidate the previous record before its payload becomes visible. */ \
        __asm__ volatile("dmb ishst" ::: "memory"); \
        entry->t_ms = (uint32_t)now_ms; \
        entry->fmt = (uintptr_t)fmt; \
        entry->a0 = a0; \
        entry->a1 = a1; \
        entry->a2 = a2; \
        entry->level = level; \
        entry->core = (uint8_t)fxlog_core(); \
        entry->nargs = nargs; \
        entry->flags = 0; \
        __atomic_store_n(&entry->seq, (uint32_t)(head + 1), __ATOMIC_RELEASE); \
        fxlog_update_directory(now_ms); \
    } \
    static inline __attribute__((unused)) void fxlog_beat(uint32_t slot, uint32_t stage) \
    { \
        if (slot >= (uint32_t)(heartbeat_slots)) \
            return; \
        const uint64_t now_ms = fxlog_now_ms(); \
        struct fxlog_heartbeat *const beat = &fxlog_heartbeats[slot]; \
        __atomic_store_n(&beat->stage, stage, __ATOMIC_RELAXED); \
        __atomic_store_n(&beat->last_ms, (uint32_t)now_ms, __ATOMIC_RELAXED); \
        __atomic_store_n(&beat->core, fxlog_core(), __ATOMIC_RELAXED); \
        __atomic_fetch_add(&beat->count, 1, __ATOMIC_RELEASE); \
        fxlog_update_directory(now_ms); \
    }

#define FXLOG_WRITE(log_level, format_text, ...) do { \
    static const char fxlog_fmt[] __attribute__((section(".rodata.fxlog"))) = "" format_text; \
    if ((log_level) <= __atomic_load_n(&fxlog_ring.header.level_max, __ATOMIC_RELAXED)) { \
        const uint32_t fxlog_args[] = { 0 __VA_OPT__(,) __VA_ARGS__, 0, 0, 0 }; \
        enum { fxlog_nargs = sizeof(fxlog_args) / sizeof(fxlog_args[0]) - 4 }; \
        _Static_assert(fxlog_nargs <= 3, "fxlog accepts up to three arguments"); \
        fxlog_record((log_level), fxlog_fmt, fxlog_nargs, fxlog_args[1], fxlog_args[2], fxlog_args[3]); \
    } \
} while (0)

#define FXLOG_E(fmt, ...) FXLOG_WRITE(0u, fmt __VA_OPT__(,) __VA_ARGS__)
#define FXLOG_W(fmt, ...) FXLOG_WRITE(1u, fmt __VA_OPT__(,) __VA_ARGS__)
#define FXLOG_I(fmt, ...) FXLOG_WRITE(2u, fmt __VA_OPT__(,) __VA_ARGS__)
#define FXLOG_D(fmt, ...) FXLOG_WRITE(3u, fmt __VA_OPT__(,) __VA_ARGS__)

#endif /* FENESTRA_FXLOG_H */
