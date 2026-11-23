#include "photo_workspace.h"
#include <stddef.h>

#define ALLOC ((uintptr_t (*)(uint32_t,uint32_t,uint32_t))(uintptr_t)0x4104d2dc)
#define FREE ((uint32_t (*)(uintptr_t))(uintptr_t)0x4104d600)
#define TO_CPU ((uintptr_t (*)(uintptr_t))(uintptr_t)0x41186718)
#define TO_RAW ((uintptr_t (*)(uintptr_t))(uintptr_t)0x41186780)
#define CACHEABLE ((uint32_t (*)(uintptr_t))(uintptr_t)0x41186810)
#define CLEAN ((void (*)(uintptr_t,uint32_t,uint32_t))(uintptr_t)0x40d2b9e4)
#define INVALIDATE ((void (*)(uintptr_t,uint32_t,uint32_t))(uintptr_t)0x40d2b95c)
#define REGIONS ((uintptr_t)0xa0497d030ULL)
#define DELTA ((uintptr_t)0x400000000ULL)

enum {
    EMPTY, BUILDING=1U<<24, CANCEL_BUILD=2U<<24, OPEN=3U<<24, CLOSING=4U<<24,
    RELEASING=5U<<24, QUARANTINED=6U<<24, PAD=512
};
#define PINS UINT32_C(0xffffff)

static uint32_t u32(uintptr_t p) { return *(const volatile uint32_t *)p; }
static uint64_t u64(uintptr_t p) { return *(const volatile uint64_t *)p; }
static int object(uintptr_t p) {
    return !(p&7U) && p>=0xa00000000ULL && p<=0xa40000000ULL-0x80U;
}
static void barrier(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* This is the contiguous attribute table used by the stock conversions. The
 * actual C3.11 capture has 24 populated entries. Do not assume a transform
 * inferred from the two endpoints covers an interior attribute boundary. */
static int one_region(uintptr_t start,uintptr_t end,uint32_t kind) {
    for (unsigned i=0;i<23;i++) {
        uintptr_t row=REGIONS+i*16U;
        if (start>=u64(row) && end<u64(row+16U))
            return u32(row+8U)==kind;
    }
    return 0;
}

static int mapped(uintptr_t raw,uint32_t n) {
    uintptr_t a=raw-64U, b=raw+n+63U, c=a+DELTA, d=b+DELTA;
    return one_region(a,b,1) && one_region(c,d,0) &&
           TO_CPU(a)==c && TO_CPU(b)==d && TO_RAW(c)==a && TO_RAW(d)==b &&
           !CACHEABLE(a) && !CACHEABLE(b) && CACHEABLE(c) && CACHEABLE(d);
}

/* Both scratch lines are inside this allocation's private padding, outside
 * the payload. Use the original cache-maintenance path in both directions.
 * Observe a change made only through the owner-backed raw address before
 * writing through the cached view. Restore the original padding contents. */
static int verify_line(uintptr_t raw) {
    uintptr_t cpu=raw+DELTA;
    volatile uint64_t *r=(volatile uint64_t *)raw;
    volatile uint64_t *c=(volatile uint64_t *)cpu;
    uint64_t original=*r, first=original^UINT64_C(0x75612cc68e2975b1);
    *r=first;
    barrier();
    INVALIDATE(cpu,64,0);
    barrier();
    int pass=*c==first;
    *r=original;
    barrier();
    INVALIDATE(cpu,64,0);
    barrier();
    if (!pass || *c!=original) return 0;
    *c=first;
    CLEAN(cpu,64,0);
    barrier();
    pass=*r==first;
    *r=original;
    barrier();
    INVALIDATE(cpu,64,0);
    barrier();
    return pass && *c==original;
}

static int give(PhotoWorkspace *ws) {
    if (ws->cpu) {
        uint32_t span=(ws->bytes+63U)&~63U;
        CLEAN(ws->cpu,span,0);
        INVALIDATE(ws->cpu,span,0);
        barrier();
    }
    if (ws->handle && FREE(ws->handle)!=1) {
        atomic_store(&ws->state,QUARANTINED);
        return PHOTO_WS_RETAINED;
    }
    ws->handle=ws->cpu=0;
    ws->bytes=0;
    atomic_store(&ws->state,EMPTY);
    return PHOTO_WS_OK;
}

int photo_workspace_acquire(PhotoWorkspace *ws,uint32_t bytes) {
    if (!ws || !bytes || bytes>PHOTO_WORKSPACE_MAX_BYTES) return PHOTO_WS_ARGUMENT;
    uint32_t expected=EMPTY, span=(bytes+63U)&~63U;
    if (!atomic_compare_exchange_strong(&ws->state,&expected,BUILDING))
        return expected==QUARANTINED ? PHOTO_WS_RETAINED : PHOTO_WS_BUSY;
    uintptr_t handle=ALLOC(0,0x6b,span+3U*PAD);
    if (!handle) { atomic_store(&ws->state,EMPTY); return PHOTO_WS_MEMORY; }
    ws->handle=handle;
    /* Quarantine any nonzero malformed result. Never dereference or free an
     * untyped handle or use somebody else's image as a fallback. */
    uintptr_t owner=object(handle) && u32(handle+0x10)==0x24 ? u64(handle+8) : 0;
    if (!object(owner) || u64(owner+0x10)!=handle) {
        atomic_store(&ws->state,QUARANTINED);
        return PHOTO_WS_RETAINED;
    }
    uintptr_t base=u64(owner);
    uint32_t capacity=u32(owner+8);
    int result=PHOTO_WS_MEMORY;
    if (capacity && capacity<=0x20000000U &&
        base>=0x840000000ULL && base<=0x8c0000000ULL-capacity) {
        uintptr_t raw=(base+2U*PAD-1U)&~(uintptr_t)(PAD-1U);
        if (raw+span+PAD<=base+capacity) {
            result=PHOTO_WS_MAPPING;
            if (mapped(raw,span) && verify_line(raw-64U) && verify_line(raw+span)) {
                ws->cpu=raw+DELTA;
                ws->bytes=bytes;
                INVALIDATE(ws->cpu,span,0);
                barrier();
                expected=BUILDING;
                if (atomic_compare_exchange_strong(&ws->state,&expected,OPEN)) return PHOTO_WS_OK;
                result=PHOTO_WS_BUSY; /* A concurrent STOP latched while building. */
            }
        }
    }
    atomic_store(&ws->state,RELEASING);
    return give(ws)==PHOTO_WS_OK ? result : PHOTO_WS_RETAINED;
}

int photo_workspace_pin(PhotoWorkspace *ws,PhotoWorkspaceView *view) {
    if (!ws || !view) return PHOTO_WS_ARGUMENT;
    *view=(PhotoWorkspaceView){0};
    uint32_t s=atomic_load(&ws->state);
    while ((s&~PINS)==OPEN && (s&PINS)!=PINS) {
        if (atomic_compare_exchange_weak(&ws->state,&s,s+1)) {
            view->cpu=(uint8_t *)ws->cpu;
            view->bytes=ws->bytes;
            return PHOTO_WS_OK;
        }
    }
    return s==QUARANTINED ? PHOTO_WS_RETAINED : PHOTO_WS_BUSY;
}

int photo_workspace_unpin(PhotoWorkspace *ws) {
    if (!ws) return PHOTO_WS_ARGUMENT;
    uint32_t s=atomic_load(&ws->state);
    while (((s&~PINS)==OPEN || (s&~PINS)==CLOSING) && (s&PINS)) {
        if (atomic_compare_exchange_weak(&ws->state,&s,s-1)) return PHOTO_WS_OK;
    }
    return PHOTO_WS_ARGUMENT;
}

int photo_workspace_release(PhotoWorkspace *ws) {
    if (!ws) return PHOTO_WS_ARGUMENT;
    uint32_t s=atomic_load(&ws->state);
    for (;;) {
        if (s==EMPTY) return PHOTO_WS_OK;
        if (s==QUARANTINED) return PHOTO_WS_RETAINED;
        if (s==BUILDING) {
            if (atomic_compare_exchange_weak(&ws->state,&s,CANCEL_BUILD)) return PHOTO_WS_BUSY;
        } else if ((s&~PINS)==OPEN) {
            uint32_t closing=CLOSING|(s&PINS);
            if (atomic_compare_exchange_weak(&ws->state,&s,closing)) s=closing;
        } else if (s==CLOSING) {
            if (atomic_compare_exchange_weak(&ws->state,&s,RELEASING)) return give(ws);
        } else return PHOTO_WS_BUSY;
    }
}
