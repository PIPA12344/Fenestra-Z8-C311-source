#ifndef Z8_LAYER0_PROVIDER_H
#define Z8_LAYER0_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* Version 11 appends after the full version 10 layout. Samples may span a hook turn. */
typedef struct {
    _Atomic uint32_t version, bytes;
    _Atomic int32_t hw_current;
    _Atomic uint32_t active_banks_mask, disp_calls, disp_substituted;
    _Atomic uint32_t disp_kept_current, disp_stock_pass, disp_rejects;
    _Atomic uint32_t vsync_ticks, banks_retired;
    _Atomic uint32_t enqueue_seen, enqueue_copied, enqueue_skipped_busy;
    _Atomic uint32_t enqueue_rejected_fmt, enqueue_content_changed;
    _Atomic uint32_t copy_us, max_copy_us, grades, grade_us, max_grade_us, grade_timeouts;
    _Atomic uint32_t display_mode, copy_mode, last_latency_us, max_latency_us;
    _Atomic uint32_t last_interframe_ms, max_interframe_ms, distinct_sources, repeated_sources;
    _Atomic uint32_t errors;
    _Atomic int32_t last_result, pool_result;
    _Atomic uint32_t online, enabled, display_ready, mode_ok, hook_active, quarantined;
    _Atomic uint32_t lut_bytes, grid, surface_bytes, disp_shadow_flag, stock_baseline_confirmed;
    _Atomic uint32_t source_lag, mismatch_y, mismatch_uv, mismatch_meta, last_mismatch_row, step_bursts;
    _Atomic uint64_t last_captured_ms, last_presented_ms, last_source_stamp;
    _Atomic uint64_t last_sub_raw_y, last_stock_y, vsync_seq, last_graded_ms, last_presented_generation;
    _Atomic uint32_t sentinel_status, neon_active;
    _Atomic uint32_t active_route, lcd_substituted, evf_substituted, worker_cores, fps_milli, capture_aborts;
    _Atomic int32_t lcd_hw_current, evf_hw_current;
    _Atomic uint64_t evf_vsync_seq;
    _Atomic uint32_t helper_sentinel[3];
    uint32_t reserved_v9;
    _Atomic uint32_t wait_no_source, wait_no_bank, vsync_claim_misses, cross_route_retires;
    _Atomic uint32_t last_sleep_us, max_sleep_us;
    _Atomic uint32_t frame_us, max_frame_us, overlapped_frames, jpeg_pauses, deferred_joins;
} Layer0Metrics;

enum { LAYER0_EVIDENCE_COLS=32, LAYER0_EVIDENCE_ROWS=24, LAYER0_QUAD_BYTES=12 };
typedef struct {
    _Atomic uint32_t ready;
    uint32_t generation;
    uint64_t stamp;
    uint32_t copy_mode;
    uint32_t reserved;
    uint8_t quads[LAYER0_EVIDENCE_ROWS][LAYER0_EVIDENCE_COLS][LAYER0_QUAD_BYTES];
} Layer0Evidence;

_Static_assert(offsetof(Layer0Metrics,last_captured_ms)==200 &&
               offsetof(Layer0Metrics,sentinel_status)==264 &&
               offsetof(Layer0Metrics,neon_active)==268 && offsetof(Layer0Metrics,active_route)==272 &&
               offsetof(Layer0Metrics,lcd_hw_current)==296 && offsetof(Layer0Metrics,evf_vsync_seq)==304 &&
               offsetof(Layer0Metrics,helper_sentinel)==312 &&
               offsetof(Layer0Metrics,wait_no_source)==328 && offsetof(Layer0Metrics,frame_us)==352 &&
               sizeof(Layer0Metrics)==376,
               "version 11 metrics ABI");
_Static_assert(offsetof(Layer0Evidence,quads)==24 && sizeof(Layer0Evidence)==9240,
               "version 6 evidence ABI");

#endif
