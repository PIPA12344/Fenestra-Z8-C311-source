#define _POSIX_C_SOURCE 200809L
#define FX_JPEG_SMP_TEST 1
#include "smp.c"
#include "cube_frame.h"
#include "fenestra_z8_lut_release.h"
#include "fenestra_z8_photo_lut.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
typedef HANDLE Thread;
#define THREAD_RESULT DWORD WINAPI
static void sleep_tick(void) { Sleep(1); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_t Thread;
#define THREAD_RESULT void *
static void sleep_tick(void) {
    const struct timespec delay = {.tv_nsec = 1000000};
    (void)nanosleep(&delay, NULL);
}
#endif

static Task tasks[2];
static Thread threads[2];
static unsigned task_count, sem_count;
static unsigned fail_resume, task_deleted, sem_deleted;
static _Thread_local unsigned current_core = 1;
static atomic_uint stopped, held, rejected, consumed, overflow_wakes, dead_id, join_errors, backoffs;
static atomic_uint wake_count[2];
static unsigned quarantine_events;
static uint64_t quarantined_generation;

static THREAD_RESULT thread_task(void *context) {
    const Task *task = context;
    current_core = task->processor;
    ((void (*)(void *))task->entry)((void *)task->context);
    return 0;
}

unsigned jpeg_smp_test_core(void) { return current_core; }

int32_t jpeg_smp_test_task_create(const Task *task) {
    assert(task_count < 2 && task->processor == task_count + 2);
    assert(task->affinity == (1u << (task->processor - 1)));
    assert(task->priority == 22 && task->stack_bytes == 8192 && !task->stack && !task->attributes);
    tasks[task_count++] = *task;
    return (int32_t)task_count;
}

int32_t jpeg_smp_test_task_resume(int32_t id) {
    assert(id >= 1 && id <= 2);
    unsigned i = (unsigned)id - 1u;
    if (fail_resume == (unsigned)id) return -25;
#ifdef _WIN32
    threads[i] = CreateThread(NULL, 0, thread_task, &tasks[i], 0, NULL);
    assert(threads[i]);
#else
    assert(!pthread_create(&threads[i], NULL, thread_task, &tasks[i]));
#endif
    return 0;
}

int32_t jpeg_smp_test_task_delete(int32_t id) {
    assert((unsigned)id == fail_resume && !task_deleted);
    task_deleted = (unsigned)id;
    return 0;
}

int32_t jpeg_smp_test_task_delay(uint32_t ticks) {
    assert(ticks == 1);
    if (current_core == 1 && atomic_load(&join_errors)) return -25;
    if (current_core == atomic_load(&dead_id) + 1u && current_core != 1) return -25;
    sleep_tick();
    return atomic_load(&stopped) ? -1 : 0;
}

void jpeg_smp_test_backoff(void) {
    atomic_fetch_add(&backoffs, 1u);
    sleep_tick();
}

uint64_t jpeg_smp_test_ticks(void) {
#ifdef _WIN32
    LARGE_INTEGER reading, frequency;
    assert(QueryPerformanceCounter(&reading) && QueryPerformanceFrequency(&frequency));
    const uint64_t hz = (uint64_t)frequency.QuadPart, ticks = (uint64_t)reading.QuadPart;
    return ticks / hz * 24000000u + ticks % hz * 24000000u / hz;
#else
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 24000000u + (uint64_t)now.tv_nsec * 24u / 1000u;
#endif
}

void jpeg_smp_test_quarantine(unsigned slot, unsigned core, uint64_t lease) {
    assert(slot == 1 && core == 2 && lease > quarantined_generation);
    ++quarantine_events;
    quarantined_generation = lease;
}

unsigned jpeg_neon_sentinel(int32_t (*delay)(uint32_t)) {
    assert(current_core == 2 || current_core == 3);
    assert(!delay(1u));
    return current_core - 1u;
}

int32_t jpeg_smp_test_sem_create(const uint32_t *config) {
    assert(config[0] == 0 && config[1] == 0 && config[2] == 1 && sem_count < 2);
    return (int32_t)++sem_count;
}

int32_t jpeg_smp_test_sem_delete(int32_t id) {
    assert((unsigned)id == task_deleted && !sem_deleted);
    sem_deleted = (unsigned)id;
    return 0;
}

int32_t jpeg_smp_test_sem_signal(int32_t id) {
    assert(id >= 1 && id <= 2);
    if ((unsigned)id == atomic_load(&rejected)) return -1;
    if (!atomic_exchange(&wake_count[id - 1], 1u)) return 0;
    atomic_fetch_add(&overflow_wakes, 1u);
    return -43;
}

int32_t jpeg_smp_test_sem_wait(int32_t id) {
    assert(id >= 1 && id <= 2);
    while (!atomic_load(&stopped)) {
        if ((unsigned)id == atomic_load(&dead_id)) return -25;
        if (!atomic_load(&held) && atomic_exchange(&wake_count[id - 1], 0u)) {
            atomic_fetch_add(&consumed, 1u);
            return 0;
        }
        sleep_tick();
    }
    return -1;
}

typedef struct {
    JpegSmpJob smp;
    atomic_uint entered, completed, release, retired;
    unsigned stall_until_backoffs;
} Job;

static void grade(void *context, unsigned stripe, unsigned neon) {
    Job *job = context;
    assert(current_core >= 2 && current_core <= 3 && neon == current_core - 1u);
    assert(stripe == current_core - 1u);
    assert(!atomic_load(&job->retired));
    atomic_fetch_add(&job->entered, 1u);
    while (!atomic_load(&job->release) && !jpeg_smp_cancelled(&job->smp)) sleep_tick();
    if (jpeg_smp_cancelled(&job->smp)) {
        /* Transiently stall while holding this callback's frame reference. */
        for (unsigned i = 0; i < 200 && atomic_load(&backoffs) < job->stall_until_backoffs; ++i)
            sleep_tick();
        assert(atomic_load(&backoffs) >= job->stall_until_backoffs);
        sleep_tick();
    }
    assert(!atomic_load(&job->retired));
    atomic_fetch_add(&job->completed, 1u);
}

static int abort_now(void *context) { return context != NULL; }

static void await_value(const atomic_uint *value, unsigned expected) {
    for (unsigned i = 0; i < 200 && atomic_load(value) != expected; ++i) sleep_tick();
    assert(atomic_load(value) == expected);
}

enum { FRAME_WIDTH = 190, FRAME_PITCH = 192, FRAME_HEIGHT = 5504, ABORT_ROWS = 5 };
typedef uint8_t Planes[2][FRAME_HEIGHT][FRAME_PITCH];
static _Alignas(64) Planes input, original, expected, rendered;
static _Alignas(64) uint32_t cache[CUBE_FRAME_CACHE_WORDS];
static JpegCube cube;

typedef struct {
    JpegSmpJob smp;
    JpegSmpRows work;
    atomic_uint first_claims, fast_second_claims, fast_done, partial, deadline, finished;
    atomic_uint claims[FRAME_HEIGHT];
    unsigned char written[FRAME_HEIGHT];
    uint32_t rows[3], participants;
    int abort_mid_chunk;
} Frame;

static void render_row(Planes *output, unsigned row) {
    uint8_t *y = (*output)[0][row], *uv = (*output)[1][row];
    (void)jpeg_cube_cache_row_scalar(&cube, cache, input[0][row], input[1][row], y, uv, FRAME_WIDTH);
    memcpy(y + FRAME_WIDTH, input[0][row] + FRAME_WIDTH, FRAME_PITCH - FRAME_WIDTH);
    memcpy(uv + FRAME_WIDTH, input[1][row] + FRAME_WIDTH, FRAME_PITCH - FRAME_WIDTH);
}

static void grade_frame(void *context, unsigned stripe, unsigned neon) {
    (void)neon;
    Frame *frame = context;
    assert(stripe < 3);
    uint32_t first, end, chunks = 0;
    while (!jpeg_smp_cancelled(&frame->smp) && jpeg_smp_claim_rows(&frame->work, &first, &end)) {
        assert(!(first & 1u) && end <= frame->work.height);
        for (uint32_t row = first; row < end; ++row) assert(!atomic_fetch_add(&frame->claims[row], 1u));
        if (++chunks == 1u) {
            atomic_fetch_add(&frame->first_claims, 1u);
            await_value(&frame->first_claims, frame->participants);
            /* Model a core stalled inside its first chunk while the two
             * or one faster workers keep claiming. Synchronize their second
             * claims so the test doesn't depend on host scheduling luck. */
            if (!frame->abort_mid_chunk && stripe == 2)
                await_value(&frame->fast_done, frame->participants - 1u);
        } else if (chunks == 2u && stripe != 2) {
            atomic_fetch_add(&frame->fast_second_claims, 1u);
            await_value(&frame->fast_second_claims, frame->participants - 1u);
        }
        if (frame->abort_mid_chunk) end = first + ABORT_ROWS;
        for (uint32_t row = first; row < end && !jpeg_smp_cancelled(&frame->smp); ++row) {
            render_row(&rendered, row);
            frame->written[row] = 1;
            ++frame->rows[stripe];
        }
        if (frame->abort_mid_chunk) {
            atomic_fetch_add(&frame->partial, 1u);
            await_value(&frame->partial, frame->participants);
            for (unsigned i = 0; i < 200 && !jpeg_smp_cancelled(&frame->smp); ++i) sleep_tick();
            assert(jpeg_smp_cancelled(&frame->smp));
            sleep_tick(); /* Still active until acknowledged by join. */
            break;
        }
    }
    if (stripe != 2) atomic_fetch_add(&frame->fast_done, 1u);
    atomic_fetch_add(&frame->finished, 1u);
}

static void prepare_frame(void) {
    jpeg_cube_init(&cube);
    cube.grid = 2;
    cube.count = 8;
    cube.low[0] = 70000;
    cube.high[2] = 900000;
    for (unsigned i = 0; i < 8; ++i) {
        cube.rgb[i][0] = (i & 4u) ? 65000u : 0u;
        cube.rgb[i][1] = (i & 2u) ? 9000u : 62000u;
        cube.rgb[i][2] = (i & 1u) ? 64000u : 2000u;
    }
    assert(jpeg_cube_finish(&cube));
    for (unsigned row = 0; row < FRAME_HEIGHT; ++row) {
        for (unsigned x = 0; x < FRAME_PITCH; ++x) {
            input[0][row][x] = (uint8_t)(x + 7u * (x >> 3));
            /* Different chunks can collide in the cache with different tags. */
            input[1][row][x] = (uint8_t)(((x * 3u) & 63u) | (row / 2u << 6u));
        }
    }
    memcpy(original, input, sizeof(input));
}

static void frame_parity(uint32_t height, uint32_t chunk, uint32_t tail, int offload) {
    assert(jpeg_smp_chunk_rows(height) == chunk);
    memset(cache, 0, sizeof(cache));
    for (uint32_t row = 0; row < height; ++row) render_row(&expected, row);
    memset(cache, 0, sizeof(cache));
    memset(rendered, 0xa5, sizeof(rendered));
    Frame parallel = {.work={.next_row=0, .height=height, .chunk_rows=chunk},
                      .participants=offload ? 2u : 3u};
    assert(jpeg_smp_dispatch(1, 2, &parallel.smp, grade_frame, &parallel));
    if (!offload) grade_frame(&parallel, 0, 0);
    assert(jpeg_smp_join(abort_now, NULL) == FX_OK);
    assert(atomic_load(&parallel.finished) == parallel.participants);
    assert(parallel.rows[2] == chunk && parallel.rows[1] > chunk);
    assert(offload ? parallel.rows[0] == 0 : parallel.rows[0] > chunk);
    assert(parallel.rows[0] + parallel.rows[1] + parallel.rows[2] == height);
    assert((parallel.rows[0] % chunk) + (parallel.rows[1] % chunk) == tail);
    for (uint32_t row = 0; row < FRAME_HEIGHT; ++row) {
        unsigned graded = row < height;
        assert(atomic_load(&parallel.claims[row]) == graded && parallel.written[row] == graded);
    }
    for (unsigned plane = 0; plane < 2; ++plane)
        assert(!memcmp(expected[plane], rendered[plane], height * FRAME_PITCH));
    assert(!memcmp(original, input, sizeof(input)));
    printf("PASS dynamic height %u chunk %u tail %u, hook/helper/helper rows %u/%u/%u\n",
           height, chunk, tail, parallel.rows[0], parallel.rows[1], parallel.rows[2]);
}

static int frame_deadline(void *context) {
    Frame *frame = context;
    return atomic_load(&frame->deadline) != 0u;
}

static void frame_timeout(void) {
    enum { CHUNK = 64 };
    Frame partial = {.work={.next_row=0, .height=FRAME_HEIGHT, .chunk_rows=CHUNK},
                     .participants=2, .abort_mid_chunk=1};
    memset(rendered, 0xa5, sizeof(rendered));
    assert(jpeg_smp_dispatch(1, 2, &partial.smp, grade_frame, &partial));
    await_value(&partial.partial, 2);
    atomic_store(&partial.deadline, 1u);
    assert(jpeg_smp_join(frame_deadline, &partial) == FX_INTERRUPTED);
    assert(atomic_load(&partial.work.next_row) == 2u * CHUNK);
    assert(atomic_load(&partial.finished) == 2 && partial.rows[0] == 0);
    for (unsigned row = 0; row < FRAME_HEIGHT; ++row) {
        unsigned claimed = row < 2u * CHUNK;
        unsigned written = claimed && row % CHUNK < ABORT_ROWS;
        assert(atomic_load(&partial.claims[row]) == claimed && partial.written[row] == written);
        if (!written) {
            for (unsigned p = 0; p < 2; ++p)
                for (unsigned x = 0; x < FRAME_PITCH; ++x) assert(rendered[p][row][x] == 0xa5);
        }
    }
    assert(!memcmp(original, input, sizeof(input)));
    puts("PASS dynamic mid-chunk timeout, join and output bounds");
}

static Job stuck;

static void ignore_cancel(void *context, unsigned slot, unsigned neon) {
    Job *job = context;
    assert(slot == 1 && neon == 1);
    atomic_store(&job->entered, 1u);
    /* The helper has claimed its lease and ignores cancellation until the
     * test releases it, after the timed-out producer has moved on. */
    while (!atomic_load(&job->release)) sleep_tick();
    assert(jpeg_smp_cancelled(&job->smp));
    atomic_store(&job->completed, 1u);
}

static void permanent_quarantine(void) {
    assert(jpeg_smp_dispatch(1, 1, &stuck.smp, ignore_cancel, &stuck));
    await_value(&stuck.entered, 1u);
    uint64_t start = jpeg_smp_test_ticks();
    assert(jpeg_smp_join(abort_now, &stuck) == FX_TIMEOUT);
    uint64_t elapsed = jpeg_smp_test_ticks() - start;
    assert(elapsed >= 480000u && elapsed < 2400000u);
    assert((atomic_load(&worker[0].phase) & PHASE_MASK) == QUARANTINED);
    assert(!jpeg_smp_available(1) && !atomic_load(&stuck.completed));
    assert(!jpeg_smp_quarantine_drained());
    assert(quarantine_events == 1 && quarantined_generation == stuck.smp.generation);

    Job further = {0};
    assert(jpeg_smp_dispatch(1, 0, &further.smp, grade, &further));
    assert(further.smp.generation > stuck.smp.generation);
    assert(jpeg_smp_join(abort_now, NULL) == FX_OK);
    assert(jpeg_smp_cancelled(&stuck.smp));
    atomic_store(&stuck.release, 1u);
    await_value(&stuck.completed, 1u);
    for (unsigned i = 0; i < 200 && !jpeg_smp_quarantine_drained(); ++i) sleep_tick();
    assert(jpeg_smp_quarantine_drained() && !jpeg_smp_available(1));
    printf("PASS ACTIVE helper quarantined after %llu ms while preserving its lease\n",
           (unsigned long long)(elapsed / 24000u));
}

static void join_thread(unsigned i) {
#ifdef _WIN32
    assert(WaitForSingleObject(threads[i], 10000) == WAIT_OBJECT_0);
    CloseHandle(threads[i]);
#else
    assert(!pthread_join(threads[i], NULL));
#endif
}

static uint32_t release_mode, self_timer;

uint32_t fx_z8_lut_test_release_read(uintptr_t address) {
    if (address == FX_Z8_LUT_RELEASE_MODE_ADDRESS) return release_mode;
    assert(address == FX_Z8_LUT_SELF_TIMER_ADDRESS);
    return self_timer;
}

static void burst_admission_and_off(void) {
    for (release_mode = 0; release_mode < 2; ++release_mode) {
        assert(fx_z8_lut_jpeg_release());
        assert(fx_z8_lut_single_frame() == (release_mode == 0));
    }
    assert(!fx_z8_lut_jpeg_release());
    release_mode = UINT32_MAX;
    assert(!fx_z8_lut_jpeg_release());
    release_mode = 1;
    self_timer = 1;
    assert(!fx_z8_lut_jpeg_release());

    FxZ8LutIoGate gate = {.state = UINT64_C(1) << 32 | 1u};
    assert(fx_z8_lut_io_take(&gate, 1));
    for (unsigned view = 0; view < 4; ++view) assert(fx_z8_lut_frame_take(&gate, 1));
    fx_z8_lut_io_close(&gate, 0);
    assert(!fx_z8_lut_io_epoch(&gate) && !fx_z8_lut_frame_take(&gate, 1));
    for (unsigned views = 4; views > 0; --views) {
        assert(fx_z8_lut_frame_count(&gate) == views);
        fx_z8_lut_frame_drop(&gate);
    }
    assert(!fx_z8_lut_frame_count(&gate) && (gate.state & FX_Z8_LUT_IO_CARD) == 2);
    fx_z8_lut_io_drop(&gate);
    fx_z8_lut_io_reopen(&gate, 1);
    assert(fx_z8_lut_io_epoch(&gate) == 2);
    assert(!fx_z8_lut_frame_take(&gate, 1) && fx_z8_lut_frame_take(&gate, 2));
    fx_z8_lut_io_close(&gate, 0);
    fx_z8_lut_io_reopen(&gate, 1);
    assert(fx_z8_lut_frame_take(&gate, 3));
    fx_z8_lut_frame_drop(&gate);  /* A pre-OFF lease can retire in the reopened epoch. */
    assert(fx_z8_lut_frame_count(&gate) == 1);
    fx_z8_lut_frame_drop(&gate);
    assert(gate.state == (UINT64_C(3) << 32 | 1u));
    puts("PASS burst release admission and refcounted OFF frame leases");
}

static FxResult await_poll(void) {
    for (unsigned i = 0; i < 200; ++i) {
        FxResult result = jpeg_smp_poll(abort_now, NULL);
        if (result != FX_BUSY) return result;
        sleep_tick();
    }
    return FX_TIMEOUT;
}

static void burst_reuse(void) {
    uint64_t last = generation;
    unsigned initial_overflows = atomic_load(&overflow_wakes), completed = 0;
    Job jobs[20] = {0};
    for (unsigned shot = 0; shot < 20; ++shot) {
        Job *job = &jobs[shot];
        current_core = shot % 2u + 1u;
        unsigned selected = current_core == 1u ? 2u : 1u;
        unsigned cancel_queued = shot % 4u == 0u;
        if (cancel_queued) atomic_store(&held, 1u);
        assert(jpeg_smp_dispatch(current_core, selected, &job->smp, grade, job));
        assert(job->smp.generation == ++last);
        assert(jpeg_smp_poll(abort_now, NULL) == FX_BUSY);
        if (cancel_queued) {
            jpeg_smp_cancel(&job->smp);
            assert(jpeg_smp_poll(abort_now, NULL) == FX_INTERRUPTED);
            assert(!atomic_load(&job->entered));
        } else {
            atomic_store(&held, 0u);
            await_value(&job->entered, selected);
            atomic_store(&job->release, 1u);
            assert(await_poll() == FX_OK && atomic_load(&job->completed) == selected);
            completed += selected;
        }
        atomic_store(&job->retired, 1u);
        assert(!active && !current_job && jpeg_smp_quarantine_drained());
    }
    current_core = 1u;
    assert(atomic_load(&overflow_wakes) >= initial_overflows + 5u);
    assert(completed == 20u && !quarantine_events);
    puts("PASS 20 alternating-core jobs, stale binary wakes, cancellation and descriptor drain");
}

static void relative_paths(void) {
    const char longest[] = "LUTS\\abcdefghijklmnopqrstuvwxyz\\12345678901234567890123456.cube";
    _Static_assert(sizeof(longest) == FX_Z8_LUT_PATH_BYTES, "longest valid path");
    _Static_assert(sizeof(FxZ8LutSelection) == 36, "legacy selection ABI");
    assert(fx_z8_lut_path(longest));
    char unterminated[FX_Z8_LUT_PATH_BYTES];
    memcpy(unterminated, longest, sizeof(unterminated));
    unterminated[sizeof(unterminated) - 1u] = 'x';
    assert(!fx_z8_lut_path(unterminated));
    assert(fx_z8_lut_path("Warm.CUBE"));
    assert(fx_z8_lut_path("luts\\Foo_01-2\\Warm.CuBe"));
    assert(fx_z8_lut_category("abcdefghijklmnopqrstuvwxyz"));
    assert(!fx_z8_lut_category("abcdefghijklmnopqrstuvwxyz0"));
    assert(!fx_z8_lut_category(".."));
    const char *invalid[] = {"", "LUTS", "\\Warm.cube", "C:\\Warm.cube", "LUTS/Warm.cube",
                             "LUTS\\\\Warm.cube", "LUTS\\..\\Warm.cube", "LUTS\\Foo\\Warm.cube\\",
                             "LUTS\\Foo\\a\\Warm.cube", "LUTS\\Foo\\Warm..cube", "LUTS\\Fo\x80\\Warm.cube",
                             "LUTS\\abcdefghijklmnopqrstuvwxyz0\\a.cube", "123456789012345678901234567.cube"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!fx_z8_lut_path(invalid[i]));
    }
    puts("PASS bounded LUT path ABI and legacy filename validation");
}

int main(int argc, char **argv) {
    relative_paths();
    (void)argv;
#ifdef _WIN32
    assert(timeBeginPeriod(1) == TIMERR_NOERROR);
#endif
    if (argc == 2) {
        fail_resume = 2;
        current_core = 2;
        jpeg_smp_begin(2);
        current_core = 1;
        assert(task_deleted == 2 && sem_deleted == 2 && jpeg_smp_available(1) == 1);
        Job single = {0};
        assert(jpeg_smp_dispatch(1, 1, &single.smp, grade, &single));
        atomic_store(&single.release, 1u);
        assert(jpeg_smp_join(abort_now, NULL) == FX_OK && atomic_load(&single.completed) == 1);
        atomic_store(&stopped, 1u);
        join_thread(0);
        puts("PASS dormant task and semaphore cleanup after resume rejection");
#ifdef _WIN32
        assert(timeEndPeriod(1) == TIMERR_NOERROR);
#endif
        return 0;
    }
    burst_admission_and_off();
    jpeg_smp_begin(1);
    jpeg_smp_begin(1);
    assert(jpeg_smp_available(0) == 0 && jpeg_smp_available(1) == 2 && jpeg_smp_available(2) == 1);

    Job normal = {0};
    assert(jpeg_smp_dispatch(1, 2, &normal.smp, grade, &normal));
    await_value(&normal.entered, 2);
    atomic_store(&normal.release, 1u);
    assert(jpeg_smp_join(abort_now, NULL) == FX_OK && atomic_load(&normal.completed) == 2);
    assert(jpeg_smp_neon_status(2) == 1 && jpeg_smp_neon_status(3) == 2);

    Job running = {.stall_until_backoffs = 4};
    assert(jpeg_smp_dispatch(1, 2, &running.smp, grade, &running));
    await_value(&running.entered, 2);
    atomic_store(&join_errors, 1u);
    assert(jpeg_smp_join(abort_now, &running) == FX_INTERRUPTED);
    assert(atomic_load(&running.completed) == 2);
    assert(atomic_load(&backoffs) >= running.stall_until_backoffs);
    atomic_store(&join_errors, 0u);
    atomic_store(&running.retired, 1u);

    Job queued = {0};
    atomic_store(&held, 1u);
    assert(jpeg_smp_dispatch(1, 2, &queued.smp, grade, &queued));
    assert(jpeg_smp_join(abort_now, &queued) == FX_INTERRUPTED);
    atomic_store(&queued.retired, 1u);
    assert(atomic_load(&wake_count[0]) == 1 && atomic_load(&wake_count[1]) == 1);
    Job fresh = {0};
    assert(jpeg_smp_dispatch(1, 2, &fresh.smp, grade, &fresh));
    assert(atomic_load(&overflow_wakes) == 2);
    atomic_store(&fresh.release, 1u);
    atomic_store(&held, 0u);
    assert(jpeg_smp_join(abort_now, NULL) == FX_OK && atomic_load(&fresh.completed) == 2);
    assert(!atomic_load(&queued.entered));

    Job partial = {0};
    atomic_store(&rejected, 2u);
    assert(!jpeg_smp_dispatch(1, 2, &partial.smp, grade, &partial));
    assert(jpeg_smp_join(abort_now, NULL) == FX_INTERRUPTED);
    assert(atomic_load(&partial.entered) == atomic_load(&partial.completed));
    atomic_store(&partial.retired, 1u);
    await_value(&consumed, 7);
    atomic_store(&rejected, 0u);

    Job moved = {0};
    current_core = 2;
    assert(jpeg_smp_dispatch(2, 1, &moved.smp, grade, &moved));
    atomic_store(&moved.release, 1u);
    assert(jpeg_smp_join(abort_now, NULL) == FX_OK && atomic_load(&moved.completed) == 1);

    current_core = 1;
    Job missed = {0};
    atomic_store(&held, 1u);
    assert(jpeg_smp_dispatch(1, 2, &missed.smp, grade, &missed));
    jpeg_smp_revoke_queued(); /* The hook already claimed every frame chunk. */
    assert(jpeg_smp_join(abort_now, NULL) == FX_OK);
    atomic_store(&missed.retired, 1u);
    assert(jpeg_smp_available(1) == 2 && !atomic_load(&missed.entered));
    atomic_store(&held, 0u);
    puts("PASS timeout waits for active quiescence, completed frame revokes stalled wakeups");

    prepare_frame();
    frame_parity(5504, 64, 0, 1);
    frame_parity(1792, 64, 0, 1);
    frame_parity(1080, 32, 24, 1);
    frame_parity(1795, 64, 3, 0);
    frame_timeout();
    burst_reuse();

    Job dying = {0};
    atomic_store(&held, 1u);
    assert(jpeg_smp_dispatch(1, 2, &dying.smp, grade, &dying));
    atomic_store(&dead_id, 2u);
    join_thread(1);
    atomic_store(&held, 0u);
    atomic_store(&dying.release, 1u);
    assert(jpeg_smp_join(abort_now, NULL) == FX_INTERRUPTED);
    assert(jpeg_smp_available(1) == 1);
    Job survivor = {0};
    assert(jpeg_smp_dispatch(1, 1, &survivor.smp, grade, &survivor));
    atomic_store(&survivor.release, 1u);
    assert(jpeg_smp_join(abort_now, NULL) == FX_OK && atomic_load(&survivor.completed) == 1);

    permanent_quarantine();
    atomic_store(&stopped, 1u);
    join_thread(0);
    assert((atomic_load(&worker[0].phase) & PHASE_MASK) == QUARANTINED_DRAINED);
    puts("PASS SMP dispatch, failed delay, stale wake, dead helper exclusion and 1-vs-3 parity");
#ifdef _WIN32
    assert(timeEndPeriod(1) == TIMERR_NOERROR);
#endif
    return 0;
}
