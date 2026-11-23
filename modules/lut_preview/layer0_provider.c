#include <stdint.h>
#include <stdatomic.h>

#include "fenestra_native_pack.h"
#include "fenestra_z8_photo_lut.h"
#include "fenestra_memory.h"
#include "layer0_color.h"
#include "layer0_provider.h"
#include "photo_workspace.h"
#include "jpeg_lut_foundation.h"
#include "smp.h"

#define FN(a,t) ((t)(uintptr_t)(a))
#define STOCK_ENQUEUE 0x40cd7ad8u
#define STOCK_DISPLAY 0x402b6d88u
#define STOCK_VSYNC 0x40cd9640u
#define STOCK_COMPLETE 0x40e9a570u
#define TO_CPU FN(JPEG_LUT_CPU_IMAGE_VIEW,uintptr_t (*)(uintptr_t))
#define TO_RAW FN(0x41186780u,uintptr_t (*)(uintptr_t))
#define CACHEABLE FN(0x41186810u,uint32_t (*)(uintptr_t))
#define CLEAN FN(JPEG_LUT_IMAGE_CLEAN,void (*)(uintptr_t,uint32_t,uint32_t))
#define CV_CLEAN FN(JPEG_LUT_IMAGE_CV_CLEAN,void (*)(const void *,uint32_t,uint32_t))
#define IMAGE_COPY FN(JPEG_LUT_IMAGE_COPY,void *(*)(void *,const void *,size_t))
#define SLEEP FN(JPEG_LUT_TASK_DELAY,int32_t (*)(uint32_t))
#define POOL_UNLOCK FN(0x40d2c660u,void (*)(uintptr_t))
#define VIEW_ID FN(0x40393f6cu,uint32_t (*)(uintptr_t))
#define VIEW_STATE FN(0x40393f28u,uint32_t (*)(uintptr_t))
#define READ_PROPERTY FN(0x4101553cu,uint32_t (*)(uint32_t,uint32_t *))

#define POOL UINT64_C(0xa03b6b538)
#define POOL_MUTEX (POOL+4u)
#define POOL_MAGIC UINT64_C(0x410439ac)
#define RAM_START UINT64_C(0xa00000000)
#define RAM_END UINT64_C(0xa40000000)
#define DISP UINT64_C(0xa02e70c98)
#define DISP_PROGRAMMED UINT64_C(0xa02e78c88)
#define PANEL_BANK UINT64_C(0xa02810470)
#define CURRENT_VIEW UINT64_C(0xa032280c8)
#define SHOOTING_VIEW UINT64_C(0xa0282dab8)
#define GUI_LV UINT64_C(0xa03229bc8)
#define STOCK_POWER_SYNC 0x40d60e94u
#define RECT(x,y,r,b) ((uint64_t)(x)|((uint64_t)(y)<<16)|((uint64_t)(r)<<32)|((uint64_t)(b)<<48))
#define DELTA UINT64_C(0x400000000)

#ifndef LAYER0_DEFAULT_DISPLAY_MODE
#define LAYER0_DEFAULT_DISPLAY_MODE 0
#endif
#ifndef LAYER0_DEFAULT_COPY_MODE
#define LAYER0_DEFAULT_COPY_MODE 0
#endif
#ifndef LAYER0_DEFAULT_SOURCE_LAG
#define LAYER0_DEFAULT_SOURCE_LAG 1
#endif
_Static_assert(LAYER0_DEFAULT_DISPLAY_MODE==0 || LAYER0_DEFAULT_DISPLAY_MODE==1,
               "display mode must be 0 or 1");
_Static_assert(LAYER0_DEFAULT_COPY_MODE>=0 && LAYER0_DEFAULT_COPY_MODE<=2,
               "copy mode must be 0, 1 or 2");
_Static_assert(LAYER0_DEFAULT_SOURCE_LAG>=0 && LAYER0_DEFAULT_SOURCE_LAG<=2,
               "source lag must be 0, 1 or 2");
_Static_assert(ATOMIC_INT_LOCK_FREE==2 && ATOMIC_LONG_LOCK_FREE==2 && ATOMIC_LLONG_LOCK_FREE==2,
               "lock-free hook counters and metadata");

/* Leave room for the cache operation or row band already in flight. */
enum { HISTORY=8, STEP_BURST_MS=220, STEP_MAX_FRAMES=12, STEP_IDLE_MS=40, COOP_SLICE_US=22000u,
       EVF_JOIN_US=800u };
enum { LCD_NEON_STEP_MS=100, LCD_NEON_STEP_FRAMES=7, LCD_NEON_FRAME_INTERVAL_MS=22,
       EVF_NEON_STEP_MS=485, EVF_NEON_STEP_FRAMES=30, LCD_HW_BANK_LIMIT=3 };
enum {
    BANKS=PHOTO_WORKSPACE_PREVIEW_BANKS, BANK_BYTES=PHOTO_WORKSPACE_PREVIEW_BANK_BYTES, CACHE_OFFSET=BANK_BYTES,
    GRADED_OFFSET=CACHE_OFFSET+REAR_CACHE, WORK_BYTES=PHOTO_WORKSPACE_PREVIEW_BYTES,
    LCD_Y_BYTES=REAR_W*REAR_H, EVF_W=1280u, EVF_H=852u, EVF_CANVAS_H=960u,
    SOURCE_UV_OFFSET=0x12c000, SOURCE_BLOCK_BYTES=0x1c2000,
    SOURCE_MAX_AGE_MS=250u, COPY_CHUNK_CACHED_ROWS=64u, COPY_CHUNK_UNCACHED_ROWS=16u,
    CACHE_CLEAR_CHUNK=256u*1024u,
    POOL_MAX_RECORDS=64u, POOL_NODE_BYTES=0x18u,
    DISP_STATE=0xc60, LAYER0_ENABLED=0x248, LAYER0_SHADOW_FLAG=0x44,
    DISPLAY_UPDATE_ENABLED=0x2ef, DISP_CROP_ORIGIN=0x2fc,
    DISP_CACHE_DESC=0xc78,
    DESC_Y=0x110, DESC_UV=0x118, MAX_AGE_MS=2000, STOP_DRAIN_MS=400,
    POWER_DRAIN_MS=100, POWER_DRAIN_TICKS=100, POWER_FRAME_DRAIN_MS=500,
    POWER_OFF_SYNC=10, POWER_CANCEL_SYNC=11
};
_Static_assert(GRADED_OFFSET+BANKS*BANK_BYTES==WORK_BYTES && WORK_BYTES==0xe8c000 &&
               WORK_BYTES<=PHOTO_WORKSPACE_MAX_BYTES, "workspace layout");
_Static_assert(LCD_Y_BYTES==698368 && EVF_W*EVF_CANVAS_H==SOURCE_UV_OFFSET &&
               BANK_BYTES==SOURCE_BLOCK_BYTES && !(LCD_Y_BYTES%128) && !(EVF_W*EVF_H%128),
               "plane dimensions and cache line alignment");

enum { LCD, EVF, NO_ROUTE, ROUTE_MASK=3u, ROUTE_EPOCH=4u };
typedef struct {
    uint32_t route, device, width, height, canvas, active_y_bytes, private_uv_offset;
    uint64_t bounds;
} Geometry;
static const Geometry geometry[NO_ROUTE]={
    [LCD]={0u,0u,REAR_W,REAR_H,REAR_W|(REAR_H<<16),LCD_Y_BYTES,LCD_Y_BYTES,
           RECT(0,0,REAR_W,REAR_H)},
    [EVF]={3u,2u,EVF_W,EVF_H,EVF_W|(EVF_CANVAS_H<<16),EVF_W*EVF_H,SOURCE_UV_OFFSET,
           RECT(0,54,EVF_W,54+EVF_H)}
};

static uint64_t ticks(void) {
    uint64_t value;
    __asm__ volatile("isb; mrs %0,cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
static uint64_t frequency(void) {
    uint64_t value;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(value));
    return value;
}

static uint64_t tick_ms(uint64_t t) {
    uint64_t hz=frequency();
    return hz && hz<=UINT32_MAX ? (t/hz)*1000u+((t%hz)*1000u)/hz : UINT64_MAX;
}

static uint64_t now_ms(void) { return tick_ms(ticks()); }

static uint32_t elapsed_us(uint64_t start,uint64_t end) {
    uint64_t elapsed=end-start, hz=frequency();
    if (!hz || elapsed>UINT64_MAX/1000000u) return UINT32_MAX;
    uint64_t us=elapsed*1000000u/hz;
    return us>UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static uint32_t duration_us(uint64_t start) { return elapsed_us(start,ticks()); }

Layer0Metrics photo_rear_metrics __attribute__((used,retain))={
    .version=11, .bytes=sizeof(Layer0Metrics), .hw_current=-1,
    .active_route=UINT32_MAX, .lcd_hw_current=-1, .evf_hw_current=-1,
    .display_mode=LAYER0_DEFAULT_DISPLAY_MODE, .copy_mode=LAYER0_DEFAULT_COPY_MODE,
    .source_lag=LAYER0_DEFAULT_SOURCE_LAG, .last_mismatch_row=UINT32_MAX
};
Layer0Evidence photo_rear_evidence __attribute__((used,retain));
typedef struct {
    uint32_t version, bytes;
    _Atomic uint32_t off_calls, cancel_calls, before_mask, after_mask, elapsed_us, result;
} RearPowerMetrics;
RearPowerMetrics photo_rear_power __attribute__((used,retain))={.version=1,.bytes=sizeof(RearPowerMetrics)};
#define M photo_rear_metrics
#define COUNT(field) ((void)atomic_fetch_add_explicit(&M.field,1u,memory_order_relaxed))

enum BankState { BANK_FREE, BANK_WORKING, BANK_READY, BANK_IN_HW, BANK_RETIRING };
typedef uint64_t ImageWord __attribute__((may_alias));
typedef struct {
    uint8_t *cpu;
    uint64_t captured_ms, captured_tick, stamp, generation;
    uint32_t copy_mode, ticket;
} Stage;
typedef struct {
    _Atomic uint64_t seq;
    _Atomic uintptr_t y, uv;
    _Atomic uint64_t stamp, captured_ms, captured_tick;
    _Atomic uint32_t ticket;
} FrameMeta;
typedef struct {
    _Atomic uint32_t state;
    _Atomic uint32_t ticket;
    _Atomic uint64_t generation;
    uintptr_t raw_y, cpu_y;
    uint64_t captured_ms, captured_tick, replaced_at_vsync, replaced_at_ms, replaced_other_vsync;
} Bank;
typedef struct {
    FrameMeta history[HISTORY];
    _Atomic uint64_t history_head;
    uint64_t history_next, last_tried_gen;
    /* Owned by display_claim. The sequence itself advances in the IRQ hook. */
    uint64_t last_off_vsync, last_present_tick, presented_generation, jpeg_kept_ms;
    uint32_t off_streak, baseline_seen, last_present_ticket;
} RouteState;
typedef struct {
    _Atomic uint32_t busy, display_claim, power_closing, ticket, layout_ticket, quarantine_mask;
    uint32_t cached_banks; /* Owned by display_claim. */
    uint32_t cache_ready;
    uintptr_t neon_task;
    unsigned neon_status;
    uint64_t last_neon_frame_ms, evf_slice_start;
    uint64_t stop_started_ms;
    uint32_t closing, parsed, stop_started, grade_quarantined;
    FxZ8JpegLutSelected jpeg;
    _Atomic(const uint32_t *) permit;
    _Atomic uint32_t output_ticket, io_epoch;
    FxResult load_error;
    JpegCube *look;
    PhotoWorkspace pool;
    PhotoWorkspaceView view;
    uint8_t *stage_cpu;
    Bank *pending_bank;
    unsigned late_helper_lease;
    RouteState route[NO_ROUTE];
    Bank bank[BANKS];
} RearState;

static RearState rear={.ticket=NO_ROUTE,.layout_ticket=NO_ROUTE};
static FxZ8LutIoGate rear_io;

static uint8_t u8(uintptr_t p) { return *(const volatile uint8_t *)p; }
static uint16_t u16(uintptr_t p) { return *(const volatile uint16_t *)p; }
static uint32_t u32(uintptr_t p) { return *(const volatile uint32_t *)p; }
static uint64_t u64(uintptr_t p) { return *(const volatile ImageWord *)p; }
static void store64(uintptr_t p,uint64_t value) { *(volatile ImageWord *)p=value; }
static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void timed(_Atomic uint32_t *last,_Atomic uint32_t *max,uint32_t value) {
    atomic_store_explicit(last,value,memory_order_relaxed);
    uint32_t previous=atomic_load_explicit(max,memory_order_relaxed);
    while (previous<value &&
           !atomic_compare_exchange_weak_explicit(max,&previous,value,memory_order_relaxed,memory_order_relaxed)) {}
}

static int preview_delay(uint64_t *slice_start) {
    uint64_t start=ticks();
    int32_t result=SLEEP(1u);
    *slice_start=ticks();
    timed(&M.last_sleep_us,&M.max_sleep_us,elapsed_us(start,*slice_start));
    return result==0;
}

static int preview_checkpoint(uint64_t *slice_start) {
    return duration_us(*slice_start)<COOP_SLICE_US || preview_delay(slice_start);
}

static int claim(_Atomic uint32_t *state,uint32_t prior,uint32_t next) {
    return atomic_compare_exchange_strong(state,&prior,next);
}

static int visible(unsigned index) {
    const Geometry *g=&geometry[index];
    uint32_t bank=u32(PANEL_BANK);
    return (bank==index || (index==EVF && bank==2u)) && u64(CURRENT_VIEW)==SHOOTING_VIEW &&
           u32(GUI_LV+4u)==g->canvas && u64(GUI_LV+8u)==g->bounds && u32(PANEL_BANK)==bank;
}

static uint32_t observe_route(void) {
    uint32_t prior=atomic_load(&rear.ticket);
    for (unsigned retry=0;retry<2u;++retry) {
        unsigned index=visible(LCD) ? LCD : visible(EVF) ? EVF : NO_ROUTE;
        if ((prior&ROUTE_MASK)==index) return prior;
        uint32_t next=((prior&~(uint32_t)ROUTE_MASK)+ROUTE_EPOCH)|index;
        if (atomic_compare_exchange_strong(&rear.ticket,&prior,next)) {
            fx_z8_photo_flight(FX_Z8_FLIGHT_PREVIEW_ROUTE_SWITCH,(uint16_t)jpeg_smp_core(),prior&ROUTE_MASK,index,next);
            return next;
        }
    }
    return prior;
}

static int worker_layout(RearState *s,uint32_t ticket) {
    unsigned index=ticket&ROUTE_MASK;
    uint32_t selector=UINT32_MAX;
    int ok=index<NO_ROUTE && visible(index) &&
           VIEW_ID(SHOOTING_VIEW)==3u && VIEW_STATE(SHOOTING_VIEW)==1u &&
           READ_PROPERTY(0x06000003u,&selector)==1u && selector==0u &&
           observe_route()==ticket;
    atomic_store(&s->layout_ticket,ok ? ticket : NO_ROUTE);
    atomic_store(&M.mode_ok,ok!=0);
    atomic_store(&M.active_route,ok ? geometry[index].route : UINT32_MAX);
    return ok && !s->jpeg(Z8_LUT_CAPTURE,0);
}

static int output_open(const RearState *s) {
    if (!atomic_load(&M.enabled)) return 0;
    uint32_t epoch=atomic_load_explicit(&s->io_epoch,memory_order_relaxed);
    if (!epoch || fx_z8_lut_io_epoch(&rear_io)!=epoch) return 0;
    FxZ8LutOutput output={
        atomic_load_explicit(&s->permit,memory_order_relaxed),
        atomic_load_explicit(&s->output_ticket,memory_order_relaxed)
    };
    return output.permit && fx_z8_lut_output_open(&output);
}

static int armed(void) {
    return atomic_load(&M.online) && output_open(&rear) && atomic_load(&M.display_ready) &&
           !atomic_load(&rear.power_closing);
}

static int fresh(uint64_t now,uint64_t captured) {
    return now!=UINT64_MAX && now>=captured && now-captured<MAX_AGE_MS;
}

__attribute__((used,retain)) uint32_t photo_rear_set_display_mode(uint32_t mode) {
    if (mode>1) return FX_ARGUMENT;
    if (mode && atomic_load(&M.quarantined)) return FX_STATE;
    atomic_store(&M.display_mode,mode);
    return FX_OK;
}

__attribute__((used,retain)) uint32_t photo_rear_set_copy_mode(uint32_t mode) {
    if (mode>2u) return FX_ARGUMENT;
    if (!claim(&rear.busy,0,1)) return FX_BUSY;
    if (atomic_exchange(&M.copy_mode,mode)!=mode)
        atomic_store(&photo_rear_evidence.ready,0);
    atomic_store(&rear.busy,0);
    return FX_OK;
}

__attribute__((used,retain)) uint32_t photo_rear_set_source_lag(uint32_t lag) {
    if (lag>2u) return FX_ARGUMENT;
    if (!claim(&rear.busy,0,1)) return FX_BUSY;
    if (atomic_exchange(&M.source_lag,lag)!=lag)
        atomic_store(&photo_rear_evidence.ready,0);
    atomic_store(&rear.busy,0);
    return FX_OK;
}

static int mapped_plane(uintptr_t raw,uintptr_t cpu,uint32_t bytes) {
    uintptr_t end=(uintptr_t)bytes-1u;
    return !(raw&63u) && cpu==raw+DELTA &&
           TO_CPU(raw)==cpu && TO_CPU(raw+end)==cpu+end &&
           TO_RAW(cpu)==raw && TO_RAW(cpu+end)==raw+end &&
           !CACHEABLE(raw) && !CACHEABLE(raw+end) && CACHEABLE(cpu) && CACHEABLE(cpu+end);
}

static int source_planes(const Geometry *g,uintptr_t y,uintptr_t uv) {
    return !(y&511u) && y>=RAM_START &&
           y<=RAM_END-SOURCE_BLOCK_BYTES && uv==y+SOURCE_UV_OFFSET &&
           mapped_plane(y,y+DELTA,g->active_y_bytes) && mapped_plane(uv,uv+DELTA,g->active_y_bytes/2u);
}

static int POOL_TRYLOCK(uintptr_t mutex) {
    /* C3.11 forwards W1 to the kernel poll as its timeout. */
    return FN(0x40d2c698u,int (*)(uintptr_t,uint32_t))(mutex,0u);
}

static int in_pool_ram(uintptr_t p,uint64_t bytes) {
    return !(p&7u) && p>=RAM_START && bytes<=RAM_END-RAM_START && p<=RAM_END-bytes;
}

typedef struct {
    uintptr_t record, owner;
    uint32_t slot;
} PoolOwner;

/* Pool 0's owner records can move between queues under the stock mutex. */
static __attribute__((minsize,noinline)) int pool_owns_source(uintptr_t y,PoolOwner *identity) {
    if (u32(POOL)!=0u || u64(POOL+0x10u)!=POOL_MAGIC) return 0;
    uint32_t a=u32(POOL+0x30u), b=u32(POOL+0x70u);
    if (a>POOL_MAX_RECORDS || b>POOL_MAX_RECORDS-a || a+b==0u) return 0;

    PoolOwner found={0};
    for (unsigned offset=0x20u;offset<=0x60u;offset+=0x40u) {
        uintptr_t q=POOL+offset, head=u64(q), tail=u64(q+8u);
        uint32_t count=offset==0x20u ? a : b;
        if (!count) {
            if (head || tail) return 0;
            continue;
        }
        uint32_t cap=u32(q+0x14u);
        uintptr_t backing=u64(q+0x18u), node=head, prev=tail;
        uint64_t extent=(uint64_t)cap*POOL_NODE_BYTES;
        if (count>cap || !in_pool_ram(backing,extent)) return 0;
        for (unsigned i=0;i<count;++i) {
            if ((i && node==head) || node<backing || node-backing>=extent ||
                (node-backing)%POOL_NODE_BYTES || u64(node)!=prev) return 0;
            uintptr_t rec=u64(node+0x10u);
            if (!in_pool_ram(rec,0x28u)) return 0;
            uintptr_t owner=u64(rec+8u);
            if (!in_pool_ram(owner,0x10u)) return 0;
            if (u64(owner)==y) {
                if (found.owner || u64(rec+0x20u)!=y || u32(owner+8u)<SOURCE_BLOCK_BYTES) return 0;
                found=(PoolOwner){rec,owner,u32(rec+0x18u)};
            }
            prev=node;
            node=u64(node+8u);
        }
        if (prev!=tail || node!=head) return 0;
    }
    if (!found.owner || (identity->owner &&
        (identity->owner!=found.owner || identity->record!=found.record || identity->slot!=found.slot))) return 0;
    *identity=found;
    return 1;
}

static int packet_matches(const Geometry *g,uintptr_t p,uintptr_t rect) {
    return p && rect && !((p|rect)&7u) && u8(p)==1 && u64(p+8u)==2 &&
           u16(p+0x10u)==g->width && u16(p+0x12u)==g->width &&
           u16(p+0x18u)==g->width && u16(p+0x1au)==g->height && u32(p+0x1cu)==0 &&
           u64(rect)==g->bounds;
}

static int spot_matches_row_range(uintptr_t raw,const uint8_t *stage,unsigned first,unsigned count,
                                  unsigned rows,unsigned width,uint32_t seed,unsigned *bad_row) {
    for (unsigned i=first;i<first+count;++i) {
        unsigned left=0, right=width-8u;
        if (i && i!=rows-1u) {
            uint32_t phase=(i*UINT32_C(2654435761))^seed;
            left=((phase>>12)&63u)*8u;
            right=width/2u+((phase>>22)&63u)*8u;
        }
        if (u64(raw+left)!=*(const ImageWord *)(const void *)(stage+left) ||
            u64(raw+right)!=*(const ImageWord *)(const void *)(stage+right)) {
            *bad_row=i;
            return 0;
        }
        raw+=width;
        stage+=width;
    }
    return 1;
}

static uint64_t read_stamp(uintptr_t p) {
    return u32(p)|((uint64_t)u32(p+4u)<<32);
}

static void record_meta(uint32_t route,uintptr_t p,uintptr_t rect,uintptr_t stamp,uintptr_t cb) {
    if ((route!=0u && route!=3u) || cb!=STOCK_COMPLETE || !armed()) return;
    COUNT(enqueue_seen);
    uint32_t ticket=observe_route();
    unsigned index=ticket&ROUTE_MASK;
    if (index>=NO_ROUTE || geometry[index].route!=route) return;
    const Geometry *g=&geometry[index];
    if (!packet_matches(g,p,rect) || !stamp || (stamp&3u)) {
        COUNT(enqueue_rejected_fmt);
        return;
    }
    uintptr_t y=u64(p+0x20u), uv=u64(p+0x28u);
    if (!source_planes(g,y,uv)) {
        COUNT(enqueue_rejected_fmt);
        return;
    }
    uint64_t source_stamp=read_stamp(stamp), tick=ticks();
    uint64_t captured_ms=tick_ms(tick);
    /* The stock route has one producer. Retire clears the ring with admission closed
     * and no active hooks. A slot's invalidation precedes all field writes. */
    RouteState *r=&rear.route[index];
    uint64_t gen=++r->history_next;
    FrameMeta *slot=&r->history[gen&(HISTORY-1u)];
    atomic_store_explicit(&slot->seq,0u,memory_order_release);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&slot->y,y,memory_order_relaxed);
    atomic_store_explicit(&slot->uv,uv,memory_order_relaxed);
    atomic_store_explicit(&slot->stamp,source_stamp,memory_order_relaxed);
    atomic_store_explicit(&slot->captured_ms,captured_ms,memory_order_relaxed);
    atomic_store_explicit(&slot->captured_tick,tick,memory_order_relaxed);
    atomic_store_explicit(&slot->ticket,ticket,memory_order_relaxed);
    atomic_store_explicit(&slot->seq,gen,memory_order_release);
    atomic_store_explicit(&r->history_head,gen,memory_order_release);
}

uint32_t fx_layer0_enqueue_hook(uint32_t route,const void *pkt,const void *rect,const void *stamp,void *cb) {
    atomic_fetch_add(&M.hook_active,1u);
    record_meta(route,(uintptr_t)pkt,(uintptr_t)rect,(uintptr_t)stamp,(uintptr_t)cb);
    uint32_t rc=FN(STOCK_ENQUEUE,uint32_t (*)(uint32_t,const void *,const void *,const void *,void *))
        (route,pkt,rect,stamp,cb);
    atomic_fetch_sub(&M.hook_active,1u);
    return rc;
}

static int layer_matches(const Geometry *g,uintptr_t d) {
    return u32(d+8u)==4 && u8(d+0x14u)==1 && u16(d+0x16u)==g->width &&
           u16(d+0x18u)==g->height && u16(d+0x1au)==g->width && u16(d+0x1cu)==g->width;
}

static int display_running(unsigned index) {
    /* Stock 402b6d88 accepts devices 0 and 2 when 402ce608 reports
     * bank 0 and 1 respectively. Bank 2 can be seen during EVF setup. */
    return u32(DISP+DISP_STATE)==3 && u32(PANEL_BANK)==index;
}

static int update_enabled(unsigned index) {
    /* 402cdfa0 can return success before writing any planes if the enable
     * byte is off. Its separate top-level state accepts exactly 3, 5 or 6. */
    uint32_t status=u32(DISP);
    return display_running(index) && u8(DISP+DISPLAY_UPDATE_ENABLED)==1 &&
           (status==3 || status==5 || status==6);
}

static int any_update_enabled(void) { return update_enabled(LCD) || update_enabled(EVF); }

static int safe_crop(const Geometry *g,uintptr_t desc) {
    /* 402d13f0 adds pitch times top to both programmed plane bases.
     * 402cdfa0 updates that origin from the descriptor only when flagged. */
    uintptr_t crop=u8(desc+0x1eu) ? desc+0x20u : DISP+DISP_CROP_ORIGIN;
    return u32(crop)==0 && u16(crop+4u)<=g->width && u16(crop+6u)<=g->height &&
           (g->route==0u || (u8(desc+0x28u) && u16(desc+0x2au)==0u && u16(desc+0x2cu)==54u &&
                            u16(desc+0x2eu)==g->width && u16(desc+0x30u)==54u+g->height));
}

static int newest_ready(uint32_t ticket,uint64_t newer_than) {
    int best=-1;
    for (unsigned i=0;i<BANKS;++i) {
        if (atomic_load(&rear.bank[i].state)!=BANK_READY || atomic_load(&rear.bank[i].ticket)!=ticket) continue;
        uint64_t generation=atomic_load(&rear.bank[i].generation);
        if (generation>newer_than) {
            newer_than=generation;
            best=(int)i;
        }
    }
    return best;
}

static int bank_address(const Bank *bank,uintptr_t y,uintptr_t uv) {
    return y-bank->raw_y<BANK_BYTES || y-bank->cpu_y<BANK_BYTES ||
           uv-bank->raw_y<BANK_BYTES || uv-bank->cpu_y<BANK_BYTES;
}

static int references_leased_bank(uintptr_t y,uintptr_t uv) {
    uint32_t leased=atomic_load(&M.active_banks_mask);
    for (unsigned i=0;i<BANKS;++i)
        if ((leased&(1u<<i)) && bank_address(&rear.bank[i],y,uv)) return 1;
    return 0;
}

static void refresh_cached_banks(void) {
    /* 402d1030 can clear the pending flag without erasing the saved planes.
     * Keep only those allocations pinned while the live output uses spare banks. */
    uintptr_t y=u64(DISP+DISP_CACHE_DESC+DESC_Y), uv=u64(DISP+DISP_CACHE_DESC+DESC_UV);
    uint32_t leased=atomic_load(&M.active_banks_mask), cached=0;
    for (unsigned i=0;i<BANKS;++i) {
        if (!(leased&(1u<<i)) || !bank_address(&rear.bank[i],y,uv)) continue;
        cached|=1u<<i;
        atomic_store(&rear.bank[i].state,BANK_IN_HW);
    }
    rear.cached_banks=cached;
}

static uint32_t hardware_pending(void) {
    return atomic_load(&M.active_banks_mask);
}

static _Atomic uint64_t *vsync_seq(unsigned index) { return index==LCD ? &M.vsync_seq : &M.evf_vsync_seq; }
static _Atomic int32_t *hw_current(unsigned index) { return index==LCD ? &M.lcd_hw_current : &M.evf_hw_current; }

/* Called with display_claim held. Keep the workspace pinned under the resident
 * pack. A live stock replacement must establish a fresh fence before reuse. */
static void retain_hardware(void) {
    uint32_t leased=atomic_load(&M.active_banks_mask);
    atomic_store(&M.display_ready,0);
    atomic_store(&M.stock_baseline_confirmed,0);
    for (unsigned i=0;i<NO_ROUTE;++i) {
        rear.route[i].off_streak=0;
        rear.route[i].baseline_seen=0;
    }
    unsigned mask=0;
    for (unsigned i=0;i<BANKS;++i) {
        if (!(leased&(1u<<i))) continue;
        mask|=1u<<(atomic_load(&rear.bank[i].ticket)&ROUTE_MASK);
        atomic_store(&rear.bank[i].state,BANK_IN_HW);
    }
    atomic_store(&rear.quarantine_mask,mask);
    atomic_store(&M.quarantined,mask!=0);
}

/* Called only under the nonblocking display claim. A VSync observed during the
 * stock call cannot count as a retire fence. Capture the sequence on return. */
static void replace_banks(unsigned index,int keep,uint64_t seq,uint64_t other_ms,uint64_t other_seq) {
    for (unsigned i=0;i<BANKS;++i) {
        Bank *bank=&rear.bank[i];
        if ((int)i==keep || (rear.cached_banks&(1u<<i)) || (atomic_load(&bank->ticket)&ROUTE_MASK)!=index) continue;
        unsigned state=atomic_load(&bank->state);
        if (state==BANK_IN_HW) {
            bank->replaced_at_vsync=seq;
            bank->replaced_at_ms=other_ms;
            bank->replaced_other_vsync=other_seq;
            atomic_store(&bank->state,BANK_RETIRING);
        } else if (state==BANK_RETIRING && other_ms && !bank->replaced_at_ms) {
            /* An older local fence can stop advancing as this route goes dark. */
            bank->replaced_at_ms=other_ms;
            bank->replaced_other_vsync=other_seq;
        }
    }
    atomic_store(hw_current(index),keep);
    rear.route[index].off_streak=0;
}

static void pin_hardware(unsigned index) {
    atomic_fetch_or(&M.active_banks_mask,1u<<index);
    atomic_store(&rear.bank[index].state,BANK_IN_HW);
}

static void record_present(unsigned index,const Bank *bank,uint64_t now) {
    if (now==UINT64_MAX) return;
    RouteState *r=&rear.route[index];
    uint64_t present_tick=ticks();
    uint32_t ticket=atomic_load(&bank->ticket);
    if (r->last_present_tick && r->last_present_ticket==ticket) {
        uint32_t gap=duration_us(r->last_present_tick);
        timed(&M.last_interframe_ms,&M.max_interframe_ms,gap/1000u);
        atomic_store(&M.fps_milli,gap ? 1000000000u/gap : 0u);
    } else atomic_store(&M.fps_milli,0u);
    r->last_present_ticket=ticket;
    r->last_present_tick=present_tick;
    atomic_store(&M.last_presented_ms,now);
    timed(&M.last_latency_us,&M.max_latency_us,duration_us(bank->captured_tick));
    atomic_store(&M.last_sub_raw_y,bank->raw_y);
    r->presented_generation=atomic_load(&bank->generation);
    atomic_store(&M.last_presented_generation,r->presented_generation);
}

static uint32_t display_original(uint32_t device,void *desc) {
    return FN(STOCK_DISPLAY,uint32_t (*)(uint32_t,void *))(device,desc);
}

static int route_banks(unsigned index) {
    unsigned count=0;
    uint32_t leased=atomic_load(&M.active_banks_mask);
    for (unsigned i=0;i<BANKS;++i)
        if ((leased&(1u<<i)) && (atomic_load(&rear.bank[i].ticket)&ROUTE_MASK)==index) ++count;
    return (int)count;
}

/* The caller owns display_claim and has checked the stock suspend cache. */
static void retire_banks(unsigned active_index,uint64_t active_seq,uint64_t now) {
    uint32_t retained=atomic_load(&rear.quarantine_mask);
    int fence_allowed=!(retained&(1u<<active_index)) ||
                      (atomic_load(&M.online) && rear.route[active_index].baseline_seen);
    for (unsigned i=0;i<BANKS;++i) {
        Bank *bank=&rear.bank[i];
        if ((rear.cached_banks&(1u<<i)) || atomic_load(&bank->state)!=BANK_RETIRING) continue;
        unsigned index=atomic_load(&bank->ticket)&ROUTE_MASK;
        if (index==active_index) {
            if (!fence_allowed || active_seq<bank->replaced_at_vsync ||
                active_seq-bank->replaced_at_vsync<2u) continue;
        } else {
            int replaced=!(retained&(1u<<index)) ||
                         (atomic_load(&M.online) && rear.route[index].baseline_seen);
            if (!replaced || !bank->replaced_at_ms || now==UINT64_MAX ||
                now<bank->replaced_at_ms || now-bank->replaced_at_ms<100u ||
                active_seq<bank->replaced_other_vsync || active_seq-bank->replaced_other_vsync<4u) continue;
            COUNT(cross_route_retires);
        }
        atomic_fetch_and(&M.active_banks_mask,~(1u<<i));
        atomic_store(&bank->state,BANK_FREE);
        COUNT(banks_retired);
    }
    for (unsigned index=0;retained && index<NO_ROUTE;++index) {
        if ((retained&(1u<<index)) && !route_banks(index)) retained&=~(1u<<index);
    }
    if (retained!=atomic_load(&rear.quarantine_mask)) {
        atomic_store(&rear.quarantine_mask,retained);
        atomic_store(&M.quarantined,retained!=0);
    }
}

static void replace_sink(unsigned index,int keep,uint32_t ticket) {
    uint64_t seq=atomic_load(vsync_seq(index));
    uint64_t now=(ticket&ROUTE_MASK)==index && visible(index) ? now_ms() : 0;
    replace_banks(index,keep,seq,0,0);
    replace_banks(index^1u,-1,atomic_load(vsync_seq(index^1u)),now==UINT64_MAX ? 0 : now,seq);
}

static int can_substitute(unsigned index,uint32_t ticket,uintptr_t d) {
    return armed() && !atomic_load(&M.quarantined) && atomic_load(&M.display_mode)==1 &&
           atomic_load(&rear.layout_ticket)==ticket && (ticket&ROUTE_MASK)==index &&
           visible(index) && update_enabled(index) &&
           safe_crop(&geometry[index],d) && !rear.jpeg(Z8_LUT_CAPTURE,0);
}

static int keep_current(RouteState *r,const Bank *bank,uint64_t now) {
    if (fx_z8_lut_frame_count(&rear_io)) {
        r->jpeg_kept_ms=now;
        return now!=UINT64_MAX;
    }
    return fresh(now,bank->captured_ms) ||
           (r->jpeg_kept_ms && now>=r->jpeg_kept_ms && now-r->jpeg_kept_ms<=SOURCE_MAX_AGE_MS);
}

__attribute__((minsize))
uint32_t fx_layer0_display_hook(uint32_t device,void *layer_desc) {
    atomic_fetch_add(&M.hook_active,1u);
    COUNT(disp_calls);
    uintptr_t d=(uintptr_t)layer_desc;
    if ((device!=0u && device!=2u) || !d || (d&7u) || u32(d)!=0) {
        COUNT(disp_stock_pass);
        COUNT(disp_rejects);
        uint32_t rc=display_original(device,layer_desc);
        atomic_fetch_sub(&M.hook_active,1u);
        return rc;
    }
    unsigned index=device==0u ? LCD : EVF;
    const Geometry *g=&geometry[index];
    RouteState *r=&rear.route[index];
    uint64_t orig_y=u64(d+DESC_Y), orig_uv=u64(d+DESC_UV);
    atomic_store(&M.last_stock_y,orig_y);
    int tracking=claim(&rear.display_claim,0,1);
    int format=layer_matches(g,d);
    int running=tracking && format && update_enabled(index);
    uint32_t ticket=observe_route();
    int previous=-1, selected=-1;
    uint64_t now=UINT64_MAX;
    if (running && hardware_pending()) {
        refresh_cached_banks();
        /* A VSync increments its sequence before trying the claim. Service a
         * missed retirement before checking the hardware bank limit. */
        now=now_ms();
        retire_banks(index,atomic_load(vsync_seq(index)),
                     (ticket&ROUTE_MASK)==index ? now : UINT64_MAX);
    }
    if (running && can_substitute(index,ticket,d)) {
        if (now==UINT64_MAX) now=now_ms();
        previous=atomic_load(hw_current(index));
        /* EVF can submit before its next VSync retires the oldest bank.
         * Keep one bank available for the other route after a panel change. */
        int limit=index==LCD ? LCD_HW_BANK_LIMIT : BANKS-1;
        int ready=route_banks(index)<limit ? newest_ready(ticket,r->presented_generation) : -1;
        if (ready>=0 && claim(&rear.bank[ready].state,BANK_READY,BANK_WORKING)) {
            Bank *bank=&rear.bank[ready];
            if (atomic_load(&bank->ticket)==ticket && atomic_load(&bank->generation)>r->presented_generation &&
                fresh(now,bank->captured_ms)) selected=ready;
            else atomic_store(&bank->state,BANK_FREE);
        }
        /* If the old route retains four leases, a stock frame allows this
         * route's sole remaining bank to retire and cycle on its own VSync. */
        if (selected<0 && previous>=0 && route_banks(index^1u)<BANKS-1 &&
            atomic_load(&rear.bank[previous].ticket)==ticket && keep_current(r,&rear.bank[previous],now))
            selected=previous;
    }

    /* Recheck admission just before replacing the stack descriptor. If stock
     * later enters its suspend cache path, quarantine the escaped allocation. */
    if (selected>=0 && (observe_route()!=ticket || !can_substitute(index,ticket,d) ||
                        !(selected==previous ? keep_current(r,&rear.bank[selected],now_ms()) :
                                               fresh(now_ms(),rear.bank[selected].captured_ms)))) {
        if (selected!=previous) atomic_store(&rear.bank[selected].state,BANK_FREE);
        selected=-1;
    }
    if (selected>=0) {
        Bank *bank=&rear.bank[selected];
        atomic_store(&M.stock_baseline_confirmed,0);
        r->baseline_seen=0;
        pin_hardware((unsigned)selected);
        store64(d+DESC_Y,bank->raw_y);
        store64(d+DESC_UV,bank->raw_y+g->private_uv_offset);
    }
    uint32_t rc=display_original(device,layer_desc);
    if (selected>=0) {
        store64(d+DESC_Y,orig_y);
        store64(d+DESC_UV,orig_uv);
    }

    if (tracking) {
        int confirmed=running && (rc&1u) && update_enabled(index) && u64(DISP_PROGRAMMED)==d+0x100u;
        if (hardware_pending()) refresh_cached_banks();
        if (selected>=0) {
            Bank *bank=&rear.bank[selected];
            if (confirmed) {
                replace_sink(index,selected,ticket);
                atomic_store(&M.hw_current,selected);
                if (selected!=previous) {
                    r->jpeg_kept_ms=0;
                    record_present(index,bank,now_ms());
                    COUNT(disp_substituted);
                    if (index==LCD) COUNT(lcd_substituted);
                    else COUNT(evf_substituted);
                } else COUNT(disp_kept_current);
            } else {
                /* The original helper can fail after one or both address
                 * writes. Keep this candidate and all previously owned banks. */
                r->off_streak=0;
                COUNT(disp_rejects);
            }
        } else {
            COUNT(disp_stock_pass);
            if (confirmed && orig_y && orig_uv && !references_leased_bank(orig_y,orig_uv) &&
                (!atomic_load(&M.quarantined) || atomic_load(&M.online))) {
                replace_sink(index,-1,ticket);
                r->baseline_seen=1;
                rear.route[index^1u].baseline_seen=1;
                atomic_store(&M.hw_current,-1);
                atomic_store(&M.stock_baseline_confirmed,1);
            }
            if (!running && visible(index)) COUNT(disp_rejects);
        }
        atomic_store(&rear.display_claim,0u);
    } else {
        COUNT(disp_stock_pass);
        COUNT(disp_rejects);
    }
    atomic_fetch_sub(&M.hook_active,1u);
    return rc;
}

void fx_layer0_vsync_hook(uint32_t route) {
    atomic_fetch_add(&M.hook_active,1u);
    if (route==0u || route==3u) {
        unsigned index=route==0u ? LCD : EVF;
        RouteState *r=&rear.route[index];
        uint64_t seq=atomic_fetch_add(vsync_seq(index),1u)+1u;
        COUNT(vsync_ticks);
        if (claim(&rear.display_claim,0,1)) {
            atomic_store(&M.disp_shadow_flag,u8(DISP+LAYER0_SHADOW_FLAG));
            if (atomic_load(&M.active_banks_mask)) {
                refresh_cached_banks();
                int retained=(atomic_load(&rear.quarantine_mask)&(1u<<index))!=0;
                if (!retained && (u32(DISP+DISP_STATE)!=3 || u32(DISP+LAYER0_ENABLED)==0)) {
                    r->off_streak=r->last_off_vsync+1u==seq ? r->off_streak+1u : 1u;
                    r->last_off_vsync=seq;
                    /* Two successive off observations establish the stop
                     * boundary. A further full two VSyncs fence bank reuse. */
                    if (r->off_streak>=2u) {
                        int32_t current=atomic_load(hw_current(index));
                        replace_banks(index,-1,seq,0,0);
                        if (current>=0 && atomic_load(&M.hw_current)==current) atomic_store(&M.hw_current,-1);
                    }
                } else r->off_streak=0;
                retire_banks(index,seq,visible(index) ? now_ms() : UINT64_MAX);
            }
            atomic_store(&rear.display_claim,0);
        } else COUNT(vsync_claim_misses);
    }
    FN(STOCK_VSYNC,void (*)(uint32_t))(route);
    atomic_fetch_sub(&M.hook_active,1u);
}

__attribute__((minsize,noinline))
static FxResult prepare_look(RearState *s,const FxZ8LutOutput *output) {
    if (!fx_z8_lut_output_valid(output)) return FX_ARGUMENT;
    uint32_t epoch=fx_z8_lut_io_epoch(&rear_io);
    if (!epoch || !fx_z8_lut_output_closed(output)) return FX_STALE;
    /* Keep JPEG selection available after a rear helper loses its lease. */
    if (s->grade_quarantined) return FX_OK;
    if (s->closing) return FX_BUSY;
    if (s->parsed)
        return atomic_load(&s->permit)==output->permit && atomic_load(&s->output_ticket)==output->ticket &&
            atomic_load(&s->io_epoch)==epoch
            ? FX_OK : FX_BUSY;
    if (!s->look) {
        s->look=FN(JPEG_LUT_HEAP_ALLOC,void *(*)(uint32_t,uint32_t))(sizeof(*s->look),0);
        if (!s->look) return FX_BUDGET;
    }
    FxZ8LutCopy copy={.cube=s->look,.cube_bytes=sizeof(*s->look)};
    FxResult result=(FxResult)s->jpeg(Z8_LUT_COPY,&copy);
    if (result!=FX_OK) return result;
    if (!atomic_load(&M.online) || fx_z8_lut_io_epoch(&rear_io)!=epoch || !fx_z8_lut_output_closed(output)) return FX_STALE;
    atomic_store(&M.lut_bytes,copy.file_bytes);
    atomic_store(&M.grid,s->look->grid);
    atomic_store(&photo_rear_evidence.ready,0);
    s->parsed=1;
    s->cache_ready=0;
    atomic_store_explicit(&s->permit,output->permit,memory_order_relaxed);
    atomic_store_explicit(&s->output_ticket,output->ticket,memory_order_relaxed);
    atomic_store_explicit(&s->io_epoch,epoch,memory_order_relaxed);
    atomic_store(&M.enabled,1);
    return FX_OK;
}

__attribute__((minsize,noinline))
static FxResult prepare(RearState *s,uint64_t *slice_start) {
    if (atomic_load(&M.quarantined)) return FX_BUSY;
    if (!s->parsed || s->load_error!=FX_OK) return s->parsed ? s->load_error : FX_STATE;
    if (!s->view.cpu) {
        atomic_store(&M.surface_bytes,WORK_BYTES);
        int status=photo_workspace_acquire(&s->pool,WORK_BYTES);
        atomic_store(&M.pool_result,status);
        if (status!=PHOTO_WS_OK) return status==PHOTO_WS_BUSY ? FX_BUSY : FX_BUDGET;
        status=photo_workspace_pin(&s->pool,&s->view);
        atomic_store(&M.pool_result,status);
        if (status!=PHOTO_WS_OK) {
            s->load_error=FX_STATE;
            return FX_STATE;
        }
        s->stage_cpu=s->view.cpu;
        for (unsigned i=0;i<BANKS;++i) {
            Bank *bank=&s->bank[i];
            uintptr_t cpu=(uintptr_t)(s->view.cpu+GRADED_OFFSET+i*BANK_BYTES);
            bank->raw_y=TO_RAW(cpu);
            bank->cpu_y=cpu;
            if (!mapped_plane(bank->raw_y,bank->cpu_y,BANK_BYTES)) {
                s->load_error=FX_INTEGRITY;
                return FX_INTEGRITY;
            }
            /* The stock EVF packet addresses active row zero. Margin rows
             * are on the panel canvas; only unused backing tails get padding.
             * Compact LCD output ends before the EVF luma tail begins. */
            unsigned y_bytes=EVF_W*EVF_H;
            unsigned pad=SOURCE_UV_OFFSET-y_bytes;
            fx_ram_zero((void *)(cpu+y_bytes),pad);
            memset((void *)(cpu+SOURCE_UV_OFFSET+y_bytes/2u),128,pad/2u);
            CV_CLEAN((const void *)(cpu+y_bytes),pad,0);
            CV_CLEAN((const void *)(cpu+SOURCE_UV_OFFSET+y_bytes/2u),pad/2u,0);
            barrier();
            if (!preview_checkpoint(slice_start)) {
                s->load_error=FX_STATE;
                return FX_STATE;
            }
        }
    }
    if (!atomic_load(&M.online)) return FX_STATE;
    if (!s->cache_ready) {
        /* A retained workspace can still hold entries from the previous LUT. */
        for (unsigned offset=0;offset<REAR_CACHE;offset+=CACHE_CLEAR_CHUNK) {
            fx_ram_zero(s->view.cpu+CACHE_OFFSET+offset,CACHE_CLEAR_CHUNK);
            if (!preview_checkpoint(slice_start)) return FX_STATE;
        }
        if (!preview_delay(slice_start)) return FX_STATE;
        s->cache_ready=1;
    }
    if (!atomic_load(&M.online)) return FX_STATE;
    atomic_store(&M.display_ready,1u);
    return FX_OK;
}

static int source_generation_current(const RouteState *r,const FrameMeta *slot,uint64_t gen) {
    return atomic_load_explicit(&slot->seq,memory_order_acquire)==gen &&
           atomic_load_explicit(&r->history_head,memory_order_acquire)-gen<=4u;
}

typedef struct {
    uintptr_t y,uv;
    const FrameMeta *slot;
} SourcePlanes;

static int read_source_frame(RearState *s,uint32_t ticket,Stage *snapshot,SourcePlanes *source) {
    unsigned index=ticket&ROUTE_MASK;
    RouteState *r=&s->route[index];
    uint64_t head=atomic_load_explicit(&r->history_head,memory_order_acquire);
    uint32_t lag=atomic_load(&M.source_lag);
    if (head<=lag) return 0;
    uint64_t gen=head-lag;
    if (gen<=r->last_tried_gen) return 0;
    r->last_tried_gen=gen;

    const FrameMeta *slot=&r->history[gen&(HISTORY-1u)];
    if (atomic_load_explicit(&slot->seq,memory_order_acquire)!=gen) goto metadata_changed;
    uintptr_t y=atomic_load_explicit(&slot->y,memory_order_relaxed);
    uintptr_t uv=atomic_load_explicit(&slot->uv,memory_order_relaxed);
    uint64_t stamp=atomic_load_explicit(&slot->stamp,memory_order_relaxed);
    uint64_t captured_ms=atomic_load_explicit(&slot->captured_ms,memory_order_relaxed);
    uint64_t captured_tick=atomic_load_explicit(&slot->captured_tick,memory_order_relaxed);
    uint32_t source_ticket=atomic_load_explicit(&slot->ticket,memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&slot->seq,memory_order_acquire)!=gen ||
        atomic_load_explicit(&r->history_head,memory_order_acquire)-gen>3u || source_ticket!=ticket)
        goto metadata_changed;
    uint64_t now=now_ms();
    if (now==UINT64_MAX || now<captured_ms || now-captured_ms>SOURCE_MAX_AGE_MS) return 0;
    if (!source_planes(&geometry[index],y,uv)) return 0;
    *source=(SourcePlanes){y,uv,slot};
    *snapshot=(Stage){.cpu=s->stage_cpu,.captured_ms=captured_ms,.captured_tick=captured_tick,
                      .stamp=stamp,.generation=gen,.copy_mode=atomic_load(&M.copy_mode),.ticket=ticket};
    return 1;

metadata_changed:
    COUNT(mismatch_meta);
    COUNT(enqueue_content_changed);
    return 0;
}

static __attribute__((minsize,noinline)) int copy_band(const Stage *stage,uintptr_t plane,unsigned width,unsigned first,
                     unsigned count,unsigned rows,int chroma) {
    const Geometry *g=&geometry[stage->ticket&ROUTE_MASK];
    uintptr_t raw=plane+first*width, read=raw;
    uint8_t *dst=stage->cpu+(chroma ? g->private_uv_offset : 0u)+first*width;
    if (stage->copy_mode!=1u) {
        read+=DELTA;
        CLEAN(read,count*width,0);
    }
    barrier();
    IMAGE_COPY(dst,(const void *)read,count*width);
    barrier();
    unsigned bad_row;
    uint32_t seed=(uint32_t)stage->stamp^(uint32_t)(stage->stamp>>32);
    if (spot_matches_row_range(raw,dst,first,count,rows,width,seed,&bad_row)) return 1;
    if (chroma) COUNT(mismatch_uv);
    else COUNT(mismatch_y);
    atomic_store(&M.last_mismatch_row,(chroma ? g->height : 0u)+bad_row);
    return 0;
}

static __attribute__((minsize,noinline)) int copy_source_frame(RearState *s,const Stage *stage,const SourcePlanes *source,
                             int use_helpers) {
    uint32_t ticket=stage->ticket;
    unsigned index=ticket&ROUTE_MASK;
    const Geometry *g=&geometry[index];
    unsigned width=g->width, height=g->height;
    RouteState *r=&s->route[index];
    uint64_t gen=stage->generation, captured_ms=stage->captured_ms;
    uint64_t copy_start=ticks();
    unsigned chunk_rows=stage->copy_mode==1u ? COPY_CHUNK_UNCACHED_ROWS : COPY_CHUNK_CACHED_ROWS;
    int paired=index==EVF && use_helpers && stage->copy_mode!=1u;
    PoolOwner owner={0};
    unsigned total=paired ? height : height+height/2u;
    for (unsigned row=0;row<total;) {
        if (use_helpers && fx_z8_lut_frame_count(&rear_io)) return 0;
        if (!worker_layout(s,ticket) || !armed() || !source_generation_current(r,source->slot,gen))
            goto metadata_changed;
        uint64_t now=now_ms();
        if (now==UINT64_MAX || now<captured_ms || now-captured_ms>SOURCE_MAX_AGE_MS) return 0;
        if (POOL_TRYLOCK(POOL_MUTEX)!=1) {
            COUNT(enqueue_skipped_busy);
            return 0;
        }
        if (!pool_owns_source(source->y,&owner) || !source_generation_current(r,source->slot,gen)) {
            POOL_UNLOCK(POOL_MUTEX);
            goto metadata_changed;
        }

        int chroma=row>=height;
        unsigned first=chroma ? row-height : row;
        unsigned rows=chroma ? height/2u : height;
        unsigned count=rows-first<chunk_rows ? rows-first : chunk_rows;
        int matches=copy_band(stage,chroma ? source->uv : source->y,width,first,count,rows,chroma);
        if (matches && paired)
            matches=copy_band(stage,source->uv,width,first/2u,count/2u,height/2u,1);
        POOL_UNLOCK(POOL_MUTEX);
        if (!matches) goto source_changed;
        if (!source_generation_current(r,source->slot,gen)) goto metadata_changed;
        row+=count;
        fx_z8_heartbeat_progress(FX_Z8_HB_REAR_STEP);
    }
    if (!worker_layout(s,ticket) || !armed() || !source_generation_current(r,source->slot,gen))
        goto metadata_changed;

    timed(&M.copy_us,&M.max_copy_us,duration_us(copy_start));
    atomic_store(&M.last_captured_ms,captured_ms);
    COUNT(enqueue_copied);
    return 1;

metadata_changed:
    COUNT(mismatch_meta);
source_changed:
    COUNT(enqueue_content_changed);
    return 0;
}

static Bank *output_bank(RearState *s,uint32_t ticket) {
    for (unsigned i=0;i<BANKS;++i)
        if (claim(&s->bank[i].state,BANK_FREE,BANK_WORKING)) return &s->bank[i];

    /* Freeze display selection for the count and claim together. Otherwise
     * display could take the newer READY bank and leave us stealing the last. */
    if (!claim(&s->display_claim,0,1)) return NULL;
    Bank *oldest=NULL;
    unsigned ready_current=0;
    int oldest_stale=0;
    for (unsigned i=0;i<BANKS;++i) {
        Bank *bank=&s->bank[i];
        if (atomic_load(&bank->state)!=BANK_READY) continue;
        int stale=atomic_load(&bank->ticket)!=ticket;
        if (!stale) ++ready_current;
        if (!oldest || stale>oldest_stale || (stale==oldest_stale && bank->captured_tick<oldest->captured_tick)) {
            oldest=bank;
            oldest_stale=stale;
        }
    }
    Bank *claimed=(oldest_stale || ready_current>=2) && claim(&oldest->state,BANK_READY,BANK_WORKING) ? oldest : NULL;
    atomic_store(&s->display_claim,0);
    return claimed;
}

static void evidence(const Stage *stage,const Bank *bank,uint64_t generation) {
    Layer0Evidence *out=&photo_rear_evidence;
    const Geometry *g=&geometry[stage->ticket&ROUTE_MASK];
    if ((atomic_load(&out->ready) && out->reserved==g->route) || stage->copy_mode!=atomic_load(&M.copy_mode)) return;
    atomic_store(&out->ready,0u);
    unsigned width=g->width, height=g->height;
    const uint8_t *src=stage->cpu, *dst=(const uint8_t *)bank->cpu_y;
    for (unsigned j=0;j<LAYER0_EVIDENCE_ROWS;++j) {
        unsigned row=(j*(height-2u)/(LAYER0_EVIDENCE_ROWS-1u))&~1u;
        for (unsigned i=0;i<LAYER0_EVIDENCE_COLS;++i) {
            unsigned col=(i*(width-2u)/(LAYER0_EVIDENCE_COLS-1u))&~1u;
            unsigned at=row*width+col, uv=g->private_uv_offset+(row/2u)*width+col;
            uint8_t *q=out->quads[j][i];
            q[0]=src[at];q[1]=src[at+1u];q[2]=src[at+width];q[3]=src[at+width+1u];
            q[4]=src[uv];q[5]=src[uv+1u];
            q[6]=dst[at];q[7]=dst[at+1u];q[8]=dst[at+width];q[9]=dst[at+width+1u];
            q[10]=dst[uv];q[11]=dst[uv+1u];
        }
    }
    out->generation=(uint32_t)generation;
    out->stamp=stage->stamp;
    out->copy_mode=stage->copy_mode;
    out->reserved=g->route;
    atomic_store(&out->ready,1u);
}

static unsigned rear_neon_status(RearState *s) {
    /* Same stock TASK_DELAY admission checks as the JPEG helpers. */
    if (!jpeg_smp_can_schedule()) return JPEG_NEON_UNAVAILABLE;
    const volatile uintptr_t *pcb;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(pcb));
    uintptr_t task=*pcb;
    if (!task) return JPEG_NEON_UNAVAILABLE;
    if (task!=s->neon_task) {
        s->neon_status=layer0_neon_sentinel(SLEEP);
        s->neon_task=task;
    }
    return s->neon_status;
}

static int rear_neon_allowed(void) {
    uint64_t cpacr, daif;
    __asm__ volatile("mrs %0, cpacr_el1; mrs %1, daif" : "=r"(cpacr), "=r"(daif));
    return (cpacr & (UINT64_C(1)<<20)) && !(daif & 0x80u);
}

typedef struct {
    JpegSmpJob smp;
    JpegSmpRows rows;
    Stage stage;
    const JpegCube *look;
    uint32_t *cache;
    Layer0Frame frame;
    unsigned helpers;
    int reserve_first;
    _Atomic uint32_t used;
    _Atomic int result;
} PreviewGrade;

/* STEP owns this descriptor; a timeout pins it and all its buffers until reboot. */
static PreviewGrade preview_job;

static int preview_abort(void *context) {
    PreviewGrade *job=context;
    if (atomic_load_explicit(&job->result,memory_order_acquire)!=FX_OK) return 1;
    int capture=rear.jpeg(Z8_LUT_CAPTURE,0)!=0;
    int reason=FX_OK;
    if (capture || !armed() || !visible(job->stage.ticket&ROUTE_MASK) ||
        atomic_load(&rear.ticket)!=job->stage.ticket ||
        atomic_load(&rear.layout_ticket)!=job->stage.ticket) reason=FX_EMPTY;
    else if (job->helpers && fx_z8_lut_frame_count(&rear_io)) reason=FX_STALE;
    else if (!fresh(now_ms(),job->stage.captured_ms)) reason=FX_TIMEOUT;
    if (reason==FX_OK) return 0;
    int expected=FX_OK;
    if (atomic_compare_exchange_strong(&job->result,&expected,reason) && capture) {
        COUNT(capture_aborts);
        fx_z8_photo_flight(FX_Z8_FLIGHT_PREVIEW_CAPTURE_ABORT,(uint16_t)jpeg_smp_core(),
                           job->stage.ticket,job->stage.generation,(uint32_t)job->stage.stamp);
    }
    return 1;
}

static int preview_grade_checkpoint(PreviewGrade *job) {
    return !jpeg_smp_cancelled(&job->smp) && !preview_abort(job);
}

static void preview_grade_rows(void *context,unsigned slot,unsigned neon_status) {
    PreviewGrade *job=context;
    unsigned heartbeat=FX_Z8_HB_REAR_STEP+slot;
    fx_z8_heartbeat_enter(heartbeat,FX_Z8_STAGE_REAR_GRADE,0,job->stage.ticket);
    int reserved=job->reserve_first;
    uint32_t first=slot*job->rows.chunk_rows, end=first+job->rows.chunk_rows;
    while (preview_grade_checkpoint(job) && (reserved || jpeg_smp_claim_rows(&job->rows,&first,&end))) {
        reserved=0;
        atomic_fetch_or_explicit(&job->used,1u<<slot,memory_order_relaxed);
        /* Claim 32 luma rows, then poll capture before each half of the
         * band. Even boundaries leave each 2x2 NV12 quad to one writer. */
        for (uint32_t row=first;row<end;) {
            if (!preview_grade_checkpoint(job)) goto done;
            uint32_t limit=end-row>16u ? row+16u : end;
            if (neon_status==JPEG_NEON_PASS && rear_neon_allowed()) {
                atomic_store_explicit(&M.neon_active,1u,memory_order_relaxed);
                layer0_grade_nv12_planes_neon(job->look,job->cache,&job->frame,row,limit);
            } else layer0_grade_nv12_planes(job->look,job->cache,&job->frame,row,limit);
            row=limit;
        }
        if (!preview_grade_checkpoint(job)) break;
        unsigned width=job->frame.width;
        CV_CLEAN(job->frame.dst_y+first*width,(end-first)*width,0);
        if (!preview_grade_checkpoint(job)) break;
        CV_CLEAN(job->frame.dst_uv+first/2u*width,(end-first)/2u*width,0);
        barrier();
        fx_z8_heartbeat_progress(heartbeat);
    }
done:
    if (slot) fx_z8_heartbeat_idle(heartbeat);
}

static void release_helpers(void) {
    fx_z8_lut_helpers_drop(&rear_io,FX_Z8_LUT_HELPER_REAR);
}

static void quarantine_grade(RearState *s) {
    s->grade_quarantined=1;
    s->late_helper_lease=1;
    atomic_store(&M.enabled,0);
    atomic_store(&M.display_ready,0);
}

static FxResult finish_late_grade(RearState *s) {
    if (s->pending_bank) {
        FxResult joined=jpeg_smp_poll(preview_abort,&preview_job);
        if (joined==FX_BUSY) return FX_BUSY;
        if (joined==FX_TIMEOUT) {
            quarantine_grade(s);
            COUNT(grade_timeouts);
        } else {
            atomic_store(&s->pending_bank->state,BANK_FREE);
            release_helpers();
        }
        s->pending_bank=NULL;
    }
    if (s->late_helper_lease && jpeg_smp_quarantine_drained()) {
        release_helpers();
        s->late_helper_lease=0;
    }
    return FX_OK;
}

static __attribute__((minsize,noinline))
FxResult grade(RearState *s,const Stage *stage,const SourcePlanes *source,Bank *bank,unsigned neon,
               unsigned core,unsigned helpers) {
    const Geometry *g=&geometry[stage->ticket&ROUTE_MASK];
    uint8_t *output=(uint8_t *)bank->cpu_y;
    PreviewGrade *job=&preview_job;
    job->helpers=helpers;
    job->reserve_first=(stage->ticket&ROUTE_MASK)==EVF && core==1u && helpers==2u;
    /* Each participant gets one 32-row band before stealing from the
     * shared cursor, including helpers whose semaphore wake arrives late. */
    atomic_store(&job->rows.next_row,job->reserve_first ? 96u : 0u);
    job->rows.height=g->height;
    job->rows.chunk_rows=32u;
    job->stage=*stage;
    job->look=s->look;
    job->cache=(uint32_t *)(void *)(s->view.cpu+CACHE_OFFSET);
    job->frame=(Layer0Frame){stage->cpu,stage->cpu+g->private_uv_offset,
                            output,output+g->private_uv_offset,g->width,g->height};
    atomic_store(&job->used,0);
    atomic_store(&job->result,FX_OK);
    atomic_store(&M.neon_active,0u);
    if (preview_abort(job)) return (FxResult)atomic_load(&job->result);
    /* Validate the complete private snapshot before waking either helper.
     * An early semaphore wake can now always read its assigned band. */
    if (!copy_source_frame(s,stage,source,helpers!=0u)) return FX_STALE;
    if (preview_abort(job)) return (FxResult)atomic_load(&job->result);
    uint64_t start=ticks();
    if (jpeg_smp_dispatch(core,helpers,&job->smp,preview_grade_rows,job))
        preview_grade_rows(job,0u,neon);
    /* Reserved work stays with its owner until join or cancellation. Other
     * frames can revoke wakes as soon as all their rows have been claimed. */
    fx_z8_heartbeat_enter(FX_Z8_HB_REAR_STEP,FX_Z8_STAGE_REAR_JOIN,0,(uint32_t)job->smp.generation);
    if (!job->reserve_first) jpeg_smp_revoke_queued();
    uint64_t join_start=ticks();
    FxResult joined;
    while ((joined=jpeg_smp_poll(preview_abort,job))==FX_BUSY && duration_us(join_start)<EVF_JOIN_US)
        __asm__ volatile("yield" ::: "memory");
    timed(&M.grade_us,&M.max_grade_us,duration_us(start));
    if (joined==FX_BUSY) {
        jpeg_smp_cancel(&job->smp);
        s->pending_bank=bank;
        COUNT(deferred_joins);
        return FX_BUSY;
    }
    if (joined==FX_TIMEOUT) {
        quarantine_grade(s);
        return FX_TIMEOUT;
    }
    if (preview_abort(job)) return (FxResult)atomic_load(&job->result);
    uint32_t used=atomic_load(&job->used);
    if (joined!=FX_OK || atomic_load(&job->rows.next_row)<g->height ||
        (job->reserve_first && used!=7u)) return FX_EMPTY;
    if (!worker_layout(s,stage->ticket) || !armed()) return FX_EMPTY;
    uint64_t now=now_ms();
    if (!fresh(now,stage->captured_ms)) return FX_TIMEOUT;
    if (preview_abort(job)) return (FxResult)atomic_load(&job->result);
    uint64_t generation=stage->generation;
    bank->captured_ms=stage->captured_ms;
    bank->captured_tick=stage->captured_tick;
    atomic_store(&bank->generation,generation);
    atomic_store(&bank->ticket,stage->ticket);
    evidence(stage,bank,generation);
    if (!atomic_load(&M.grades) || atomic_load(&M.last_source_stamp)!=stage->stamp)
        COUNT(distinct_sources);
    else COUNT(repeated_sources);
    atomic_store(&M.last_source_stamp,stage->stamp);
    atomic_store(&M.last_graded_ms,now);
    if (preview_abort(job)) return (FxResult)atomic_load(&job->result);
    atomic_store(&M.worker_cores,(used&1u)+((used>>1)&1u)+((used>>2)&1u));
    atomic_store(&bank->state,BANK_READY);
    COUNT(grades);
    return FX_OK;
}

static __attribute__((minsize,noinline)) uint32_t make_frame(RearState *s) {
    uint64_t entered=ticks(), slice_start=entered;
    uint32_t ticket=observe_route(), index=ticket&ROUTE_MASK;
    if (index==EVF && s->evf_slice_start) slice_start=s->evf_slice_start;
    s->evf_slice_start=0;
    if (s->jpeg(Z8_LUT_CAPTURE,0)) {
        COUNT(jpeg_pauses);
        return FX_EMPTY;
    }
    if (!worker_layout(s,ticket) || !preview_checkpoint(&slice_start)) return FX_EMPTY;
    RouteState *r=&s->route[index];
    FxResult rc=prepare(s,&slice_start);
    if (rc!=FX_OK) return rc;

    unsigned status=rear_neon_status(s);
    atomic_store_explicit(&M.sentinel_status,status,memory_order_relaxed);
    int neon=status==JPEG_NEON_PASS && rear_neon_allowed();
    if (!neon) atomic_store_explicit(&M.neon_active,0,memory_order_relaxed);
    int evf_neon=index==EVF && neon;
    unsigned turn_ms=evf_neon ? EVF_NEON_STEP_MS : neon ? LCD_NEON_STEP_MS : STEP_BURST_MS;
    unsigned turn_frames=evf_neon ? EVF_NEON_STEP_FRAMES : neon ? LCD_NEON_STEP_FRAMES : STEP_MAX_FRAMES;
    COUNT(step_bursts);
    uint64_t last_activity_ms=now_ms(), burst_start=evf_neon ? tick_ms(entered) : last_activity_ms;
    uint64_t last_seen_head=atomic_load_explicit(&r->history_head,memory_order_acquire);
    unsigned graded=0;
    int turn_expired=0;
    while (graded<turn_frames) {
        uint64_t now=now_ms();
        if (now==UINT64_MAX || now<burst_start) break;
        if (now-burst_start>=turn_ms) {
            turn_expired=1;
            break;
        }
        if (s->jpeg(Z8_LUT_CAPTURE,0)) {
            COUNT(jpeg_pauses);
            break;
        }
        if (!worker_layout(s,ticket) || !armed()) {
            rc=FX_EMPTY;
            break;
        }
        uint64_t head=atomic_load_explicit(&r->history_head,memory_order_acquire);
        uint32_t lag=atomic_load(&M.source_lag);
        if (head!=last_seen_head) {
            last_seen_head=head;
            last_activity_ms=now;
        }
        if (head<=lag || head-lag<=r->last_tried_gen) {
            COUNT(wait_no_source);
            if (now>=last_activity_ms && now-last_activity_ms>=STEP_IDLE_MS) break;
            if (!preview_delay(&slice_start)) break;
            continue;
        }
        /* Preserve the LCD pacing. EVF grading follows new source frames. */
        if (index==LCD && neon && now>=s->last_neon_frame_ms &&
            now-s->last_neon_frame_ms<LCD_NEON_FRAME_INTERVAL_MS) {
            if (!preview_delay(&slice_start)) break;
            continue;
        }
        Bank *bank=output_bank(s,ticket);
        if (!bank) {
            COUNT(wait_no_bank);
            if (!preview_delay(&slice_start)) break;
            continue;
        }
        uint32_t epoch=atomic_load(&s->io_epoch);
        unsigned core=jpeg_smp_core(), helpers=0;
        /* ARM capture owns the helpers while its JPEG frame lease is active.
         * A Core-1-only preview still pins the open power/I/O epoch. */
        if (fx_z8_lut_helpers_take(&rear_io,epoch,FX_Z8_LUT_HELPER_REAR)) {
            jpeg_smp_begin(core);
            helpers=jpeg_smp_available(core);
            if (!helpers) release_helpers();
        }
        if (!helpers && !fx_z8_lut_io_take(&rear_io,epoch)) {
            atomic_store(&bank->state,BANK_FREE);
            rc=FX_EMPTY;
            break;
        }
        int arm_capture=!helpers && fx_z8_lut_frame_count(&rear_io);
        for (unsigned i=0;i<3u;++i)
            atomic_store(&M.helper_sentinel[i],jpeg_smp_neon_status(i+1u));
        Stage snapshot;
        SourcePlanes source;
        if (index==LCD && neon) s->last_neon_frame_ms=now;
        uint64_t frame_start=ticks();
        fx_z8_heartbeat_enter(FX_Z8_HB_REAR_STEP,FX_Z8_STAGE_REAR_COPY,0,ticket);
        rc=read_source_frame(s,ticket,&snapshot,&source) ?
            grade(s,&snapshot,&source,bank,neon ? JPEG_NEON_PASS : JPEG_NEON_UNAVAILABLE,core,helpers) : FX_STALE;
        fx_z8_heartbeat_enter(FX_Z8_HB_REAR_STEP,FX_Z8_STAGE_WORKER_STEP,0,ticket);
        if (!helpers) fx_z8_lut_io_drop(&rear_io);
        else if (!s->pending_bank && !s->late_helper_lease) release_helpers();
        if (rc==FX_STALE) {
            atomic_store(&bank->state,BANK_FREE);
            rc=FX_OK;
            last_activity_ms=now_ms();
            if (!preview_delay(&slice_start)) break;
            continue;
        }
        if (rc==FX_TIMEOUT) COUNT(grade_timeouts);
        if (rc!=FX_OK) {
            if (rc==FX_EMPTY)
                r->last_tried_gen=atomic_load_explicit(&r->history_head,memory_order_acquire);
            if (!s->pending_bank && !s->grade_quarantined) atomic_store(&bank->state,BANK_FREE);
            break;
        }
        timed(&M.frame_us,&M.max_frame_us,duration_us(frame_start));
        ++graded;
        last_activity_ms=now_ms();
        arm_capture|=!helpers && fx_z8_lut_frame_count(&rear_io);
        if (!(evf_neon && !arm_capture ? preview_checkpoint(&slice_start) : preview_delay(&slice_start))) break;
    }
    if (rc!=FX_OK && rc!=FX_EMPTY) return rc;
    int live=armed() && visible(index) && atomic_load(&s->ticket)==ticket;
    if (evf_neon && graded && (turn_expired || graded==turn_frames) && live && update_enabled(index) &&
        atomic_load(&s->layout_ticket)==ticket && !s->jpeg(Z8_LUT_CAPTURE,0)) {
        /* The caller services housework immediately, then resumes. Preserve
         * the compute slice across the boundary even without the outer sleep. */
        s->evf_slice_start=slice_start;
        return REAR_STREAMING;
    }
    return graded || live ? FX_OK : FX_EMPTY;
}

static int display_drained(RearState *s) {
    /* A display hook may have admitted a bank just before power_closing.
     * Synchronize with that publisher before accepting an empty mask. */
    if (!claim(&s->display_claim,0,1)) return 0;
    int drained=!hardware_pending() && !atomic_load(&M.quarantined);
    atomic_store(&s->display_claim,0u);
    return drained;
}

static int io_pending(void) {
    return (__atomic_load_n(&rear_io.state,__ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_REFS)!=0;
}

__attribute__((minsize))
void fx_layer0_power_sync_hook(uint32_t command,uint64_t payload) {
    if (command==POWER_OFF_SYNC) fx_z8_lut_io_close(&rear_io,0);
    atomic_fetch_add(&M.hook_active,1u);
    if (command==POWER_OFF_SYNC) {
        atomic_store(&rear.power_closing,1u);
        atomic_fetch_add(&photo_rear_power.off_calls,1u);
        uint32_t mask=atomic_load(&M.active_banks_mask);
        atomic_store(&photo_rear_power.before_mask,mask);
        fx_z8_photo_flight(FX_Z8_FLIGHT_PREVIEW_POWER_OFF,(uint16_t)jpeg_smp_core(),
                           command,mask,fx_z8_lut_frame_count(&rear_io));
        uint64_t start=ticks(), entered=tick_ms(start);
        int drained=display_drained(&rear);
        /* The stock sender posts BSShutdown only after the synchronous
         * notification returns. Keep that call in its original task and let
         * the existing stock display and VSync callbacks drain the banks. */
        for (unsigned i=0;i<POWER_DRAIN_TICKS && (!drained || io_pending());i++) {
            uint64_t now=now_ms();
            if (entered==UINT64_MAX || now==UINT64_MAX || now<entered || now-entered>=POWER_DRAIN_MS ||
                (!io_pending() && (!any_update_enabled() || atomic_load(&M.quarantined))) || SLEEP(1u)) break;
            drained=display_drained(&rear);
        }
        /* A stalled JPEG owner keeps its frame reference and private memory.
         * Let stock proceed after the grace period; the closed gate prevents
         * new leases, and the JPEG retire path still refuses to free live ones. */
        if (__atomic_load_n(&rear_io.state,__ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_FRAME) {
            atomic_store(&photo_rear_power.result,FX_BUSY);
            uint64_t grace=ticks();
            for (unsigned i=0;i<POWER_FRAME_DRAIN_MS &&
                 (__atomic_load_n(&rear_io.state,__ATOMIC_ACQUIRE) & FX_Z8_LUT_IO_FRAME);++i)
                if (duration_us(grace)>=1000u*POWER_FRAME_DRAIN_MS || SLEEP(1u)) break;
            drained=display_drained(&rear);
        }
        atomic_store(&photo_rear_power.elapsed_us,duration_us(start));
        atomic_store(&photo_rear_power.after_mask,atomic_load(&M.active_banks_mask));
        atomic_store(&photo_rear_power.result,drained && !io_pending() ? FX_OK : FX_BUSY);
    }
    FN(STOCK_POWER_SYNC,void (*)(uint32_t,uint64_t))(command,payload);
    if (command==POWER_CANCEL_SYNC) {
        atomic_fetch_add(&photo_rear_power.cancel_calls,1u);
        fx_z8_lut_io_reopen(&rear_io,1);
        if (fx_z8_lut_io_epoch(&rear_io)) atomic_store(&rear.power_closing,0u);
    }
    atomic_fetch_sub(&M.hook_active,1u);
}

__attribute__((minsize,noinline))
static FxResult retire_workspace(RearState *s,int allow_retained) {
    s->closing=1;
    atomic_store(&M.enabled,0);
    atomic_store(&M.display_ready,0);
    atomic_store(&M.mode_ok,0);
    atomic_store(&s->layout_ticket,NO_ROUTE);
    s->evf_slice_start=0;
    uint64_t now=now_ms();
    if (!s->stop_started) {
        s->stop_started_ms=now;
        s->stop_started=1;
    }
    if (!claim(&s->display_claim,0,1)) return FX_BUSY;
    if (atomic_load(&M.hook_active)) {
        atomic_store(&s->display_claim,0);
        return FX_BUSY;
    }
    int held=hardware_pending()!=0;
    if (held) {
        int expired=now==UINT64_MAX || s->stop_started_ms==UINT64_MAX || now<s->stop_started_ms ||
                    now-s->stop_started_ms>=STOP_DRAIN_MS;
        /* Suspension cannot supply a replacement frame. A switch or resident
         * shutdown can retire the LUT while the hardware keeps the workspace. */
        if (!allow_retained || (!atomic_load(&M.quarantined) && !atomic_load(&s->power_closing) &&
                                any_update_enabled() && !expired)) {
            atomic_store(&s->display_claim,0);
            return FX_BUSY;
        }
        if (!atomic_load(&M.quarantined) || !atomic_load(&M.online)) retain_hardware();
    }
    if (s->grade_quarantined) {
        atomic_store(&s->display_claim,0);
        return FX_OK;
    }
    for (unsigned i=0;i<NO_ROUTE;++i) {
        RouteState *r=&s->route[i];
        atomic_store(&r->history_head,0);
        r->history_next=0;
        r->last_tried_gen=0;
        r->last_present_tick=0;
        r->presented_generation=0;
        fx_ram_zero(r->history,sizeof(r->history));
    }
    atomic_store(&M.active_route,UINT32_MAX);
    atomic_store(&M.fps_milli,0u);
    atomic_store(&M.last_presented_generation,0);
    uint32_t leased=atomic_load(&M.active_banks_mask);
    for (unsigned i=0;i<BANKS;++i)
        if (!(leased&(1u<<i))) atomic_store(&s->bank[i].state,BANK_FREE);
    /* Later hooks still dispatch stock. The hardware hold can clear between
     * calls, but an ACK never unpins or frees a workspace with live planes. */
    atomic_store(&s->display_claim,0);
    if (!held) {
        if (s->view.cpu) {
            if (photo_workspace_unpin(&s->pool)!=PHOTO_WS_OK) return FX_CLOSE;
            s->view=(PhotoWorkspaceView){0};
            s->stage_cpu=NULL;
        }
        int result=photo_workspace_release(&s->pool);
        atomic_store(&M.pool_result,result);
        if (result!=PHOTO_WS_OK) return result==PHOTO_WS_BUSY ? FX_BUSY : FX_CLOSE;
    }
    if (s->look) {
        FN(JPEG_LUT_HEAP_FREE,void (*)(void *))(s->look);
        s->look=NULL;
    }
    s->parsed=0;
    s->cache_ready=0;
    s->load_error=FX_OK;
    s->closing=0;
    s->stop_started=0;
    return FX_OK;
}

/* One native worker owns selection, grading and explicit clear. A power
 * event can revoke admission while a call is running, then retry cleanup. */
static __attribute__((minsize)) uint32_t worker(uint32_t op,void *data) {
    if (op==REAR_FLIGHT_METRICS) {
        if (!data) return FX_ARGUMENT;
        *(FxZ8RearFlightMetrics *)data=(FxZ8RearFlightMetrics){
            atomic_load_explicit(&M.active_route,memory_order_relaxed),
            atomic_load_explicit(&M.grade_us,memory_order_relaxed),
            atomic_load_explicit(&M.worker_cores,memory_order_relaxed),
            atomic_load_explicit(&M.neon_active,memory_order_relaxed),
            atomic_load_explicit(&M.capture_aborts,memory_order_relaxed),
            atomic_load_explicit(&M.grade_timeouts,memory_order_relaxed)
        };
        return FX_OK;
    }
    if ((op!=REAR_READY && op!=REAR_CLEAR && op!=REAR_RETIRE && op!=REAR_SWITCH &&
         op!=REAR_PRESENTED && op!=REAR_WORKER_STEP) ||
        (op==REAR_READY ? !data : data!=0)) return FX_ARGUMENT;
    RearState *s=&rear;
    if (op==REAR_PRESENTED) {
        uint32_t ticket=observe_route();
        unsigned index=ticket&ROUTE_MASK;
        if (index>=NO_ROUTE || !armed() || atomic_load(&s->layout_ticket)!=ticket) return 0;
        int current=atomic_load(hw_current(index));
        return current>=0 && atomic_load(&s->bank[current].ticket)==ticket &&
               atomic_load(&M.last_presented_generation)!=0;
    }
    if (!claim(&s->busy,0,1)) return FX_BUSY;
    uint32_t result;
    if (finish_late_grade(s)==FX_BUSY) result=FX_BUSY;
    else if (!atomic_load(&M.online)) result=FX_STATE;
    else if (op==REAR_READY) result=prepare_look(s,data);
    else if (op==REAR_CLEAR) {
        /* A JPEG prepare can be in flight before the first rear copy. */
        atomic_store(&M.enabled,0);
        FxResult jpeg=(FxResult)s->jpeg(Z8_LUT_STOP,0);
        FxResult display=retire_workspace(s,0);
        result=display!=FX_OK ? display : jpeg;
    } else if (op==REAR_RETIRE || op==REAR_SWITCH) result=retire_workspace(s,op==REAR_SWITCH);
    else if (output_open(s)) {
        fx_z8_heartbeat_enter(FX_Z8_HB_REAR_STEP,FX_Z8_STAGE_WORKER_STEP,0,atomic_load(&s->ticket));
        result=make_frame(s);
        fx_z8_heartbeat_idle(FX_Z8_HB_REAR_STEP);
    } else result=FX_EMPTY;
    atomic_store(&M.last_result,result==REAR_STREAMING ? FX_OK : (int32_t)result);
    if (result!=FX_OK && result!=REAR_STREAMING && result!=FX_BUSY && result!=FX_EMPTY && result!=FX_STALE)
        COUNT(errors);
    atomic_store(&s->busy,0);
    return result;
}

static uint32_t pending(void *ctx) {
    RearState *s=ctx;
    /* Retained display memory has no EL0/session references. STOP and RESUME
     * still wait for executing software and the native pack keeps the pin. */
    if (claim(&s->busy,0,1)) {
        (void)finish_late_grade(s);
        atomic_store(&s->busy,0);
    }
    return atomic_load(&s->busy) || atomic_load(&M.hook_active) || io_pending() ||
           (hardware_pending() && !atomic_load(&M.quarantined));
}

__attribute__((minsize))
static FxResult event(void *ctx,uint32_t e) {
    RearState *s=ctx;
    if (e==FX_NATIVE_PACK_GUI_POLL) {
        if ((observe_route()&ROUTE_MASK)==NO_ROUTE) {
            atomic_store(&s->layout_ticket,NO_ROUTE);
            atomic_store(&M.mode_ok,0);
            atomic_store(&M.active_route,UINT32_MAX);
        }
        return FX_OK;
    }
    if (e==FX_NATIVE_PACK_PUBLISH || e==FX_NATIVE_PACK_RESUME) {
        atomic_store(&s->power_closing,0u);
        atomic_store(&M.online,1u);
        fx_z8_lut_io_reopen(&rear_io,0);
        return FX_OK;
    }
    if (e!=FX_NATIVE_PACK_STOP && e!=FX_NATIVE_PACK_FAULT && e!=FX_NATIVE_PACK_ABORT) return FX_ARGUMENT;
    fx_z8_lut_io_close(&rear_io,1);
    atomic_store(&M.online,0);
    atomic_store(&M.enabled,0);
    atomic_store(&M.display_ready,0);
    if (!claim(&s->busy,0,1)) return FX_BUSY;
    /* ABORT can release the pack itself, so only a resident STOP/FAULT may
     * acknowledge while holding hardware-owned memory. */
    FxResult rc=finish_late_grade(s);
    if (rc==FX_OK)
        rc=e==FX_NATIVE_PACK_ABORT && s->grade_quarantined ? FX_TIMEOUT : retire_workspace(s,e!=FX_NATIVE_PACK_ABORT);
    atomic_store(&M.last_result,(int32_t)rc);
    atomic_store(&s->busy,0);
    return rc;
}

static const FxNativePackSymbol exports[]={
    {FX_Z8_REAR_LUT_WORKER,FX_Z8_REAR_LUT_WORKER_VERSION,FX_NATIVE_PACK_SYMBOL_FUNCTION,(uintptr_t)worker},
    {FX_Z8_REAR_LUT_IO,FX_Z8_REAR_LUT_IO_VERSION,FX_NATIVE_PACK_SYMBOL_DATA,(uintptr_t)&rear_io}
};

/* These are the immutable C3.11 and foundation binding contracts. */
#include "layer0_stock_prefixes.h"

__attribute__((minsize))
FxResult fx_native_pack_entry(const FxNativePackHost *host,FxNativePackExports *out) {
    if (!host || !out || host->abi!=FX_NATIVE_PACK_ABI || host->bytes<sizeof(*host) || !host->lookup)
        return FX_ARGUMENT;
    for (unsigned i=0;i<sizeof(jpeg_lut_bindings)/sizeof(jpeg_lut_bindings[0]);++i)
        if (memcmp((const void *)jpeg_lut_bindings[i].address,jpeg_lut_bindings[i].prefix,32)) return FX_INTEGRITY;
    for (unsigned i=0;i<sizeof(layer0_bindings)/sizeof(layer0_bindings[0]);++i)
        if (memcmp((const void *)layer0_bindings[i].address,layer0_bindings[i].prefix,32)) return FX_INTEGRITY;
    uintptr_t link=0;
    FxResult result=host->lookup(host->context,FX_Z8_JPEG_LUT_WORKER,FX_Z8_JPEG_LUT_WORKER_VERSION,&link);
    if (result!=FX_OK || !link) return FX_CAPABILITY;
    rear.jpeg=(FxZ8JpegLutSelected)link;
    *out=(FxNativePackExports){.abi=FX_NATIVE_PACK_ABI,.bytes=sizeof(*out),.symbol_count=sizeof(exports)/sizeof(exports[0]),
        .symbols=exports,.context=&rear,.event=event,.pending=pending};
    return FX_OK;
}
