#define _CRT_SECURE_NO_WARNINGS
/* SPDX-License-Identifier: GPL-3.0-only */
#include "fenestra_wlm_bundle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model contiguous allocations with pre-existing holes and allocator overhead.
 * The separate barriers stand in for stock allocations already live at startup.
 * Keep their bytes outside the reported usable part of the Nikon pool.
 * Exercise address-order first fit and best fit, an approximation of bin reuse. */
static const uint32_t holes[] = {
    30944, 144, 12240, 80, 640, 170528, 90016, 128, 54704,
    192, 256, 224, 64, 15568, 33632, 96, 176
};
enum { BLOCK_HEADER = 16, ALIGNMENT = 16, BLOCK_LIMIT = 256 };
typedef struct {
    uint32_t offset, span, requested;
} Block;
typedef struct {
    uint8_t *arena;
    uint32_t size, live, peak, committed, committed_peak;
    uint32_t failed_size, failed_free, failed_largest;
    size_t count;
    int best_fit;
    Block blocks[BLOCK_LIMIT];
} Heap;

static void insert(Heap *heap, size_t index, Block block)
{
    assert(heap->count < BLOCK_LIMIT);
    memmove(&heap->blocks[index + 1], &heap->blocks[index],
            (heap->count - index) * sizeof(Block));
    heap->blocks[index] = block;
    ++heap->count;
}

static void heap_init(Heap *heap, uint32_t usable)
{
    uint32_t end = 0;
    for (size_t i = 0; i < sizeof(holes) / sizeof(holes[0]); ++i) {
        end += holes[i];
        insert(heap, heap->count, (Block){end, ALIGNMENT, 0});
        end += ALIGNMENT;
        heap->size += ALIGNMENT;
    }
    assert(end < usable);
    heap->size += usable;
    heap->arena = malloc(heap->size);
    assert(heap->arena);
}

static uint32_t largest_free(const Heap *heap)
{
    uint32_t end = 0, largest = 0;
    for (size_t i = 0; i <= heap->count; ++i) {
        uint32_t start = i == heap->count ? heap->size : heap->blocks[i].offset;
        uint32_t gap = start - end;
        if (gap > largest)
            largest = gap;
        if (i < heap->count)
            end = start + heap->blocks[i].span;
    }
    largest &= ~(uint32_t)(ALIGNMENT - 1);
    return largest > BLOCK_HEADER ? largest - BLOCK_HEADER : 0;
}

static void *heap_allocate(void *context, uint32_t bytes)
{
    Heap *heap = context;
    uint32_t span = (bytes + BLOCK_HEADER + ALIGNMENT - 1) & ~(uint32_t)(ALIGNMENT - 1);
    uint32_t end = 0, offset = 0, smallest = UINT32_MAX;
    size_t chosen = heap->count + 1;
    assert(bytes && bytes < FX_WLM_FROZEN_BUDGET);
    for (size_t i = 0; i <= heap->count; ++i) {
        uint32_t start = i == heap->count ? heap->size : heap->blocks[i].offset;
        uint32_t gap = start - end;
        if (span <= gap && gap < smallest) {
            chosen = i;
            offset = end;
            smallest = gap;
            if (!heap->best_fit)
                break;
        }
        if (i < heap->count)
            end = start + heap->blocks[i].span;
    }
    if (chosen <= heap->count) {
        insert(heap, chosen, (Block){offset, span, bytes});
        heap->live += bytes;
        heap->committed += span;
        if (heap->live > heap->peak)
            heap->peak = heap->live;
        if (heap->committed > heap->committed_peak)
            heap->committed_peak = heap->committed;
        return heap->arena + offset + BLOCK_HEADER;
    }
    heap->failed_size = bytes;
    heap->failed_free = heap->size - (uint32_t)(sizeof(holes) / sizeof(holes[0])) * ALIGNMENT
                        - heap->committed;
    heap->failed_largest = largest_free(heap);
    return NULL;
}

static void heap_release(void *context, void *address)
{
    Heap *heap = context;
    for (size_t i = 0; i < heap->count; ++i) {
        Block block = heap->blocks[i];
        if (block.requested && address == heap->arena + block.offset + BLOCK_HEADER) {
            heap->committed -= block.span;
            heap->live -= block.requested;
            --heap->count;
            memmove(&heap->blocks[i], &heap->blocks[i + 1], (heap->count - i) * sizeof(Block));
            return;
        }
    }
    assert(0 && "free without a live allocation");
}

typedef struct {
    const char *path, *name;
    uint32_t bytes;
} File;
typedef struct {
    File files[32];
    uint32_t count, next, reads, largest_read;
} Directory;

static FxResult directory_open(void *context)
{
    Directory *directory = context;
    directory->next = 0;
    return FX_OK;
}

static FxResult directory_next(void *context, FxModuleDirectoryEntry *entry)
{
    Directory *directory = context;
    if (directory->next == directory->count)
        return FX_EMPTY;
    const File *file = &directory->files[directory->next++];
    memcpy(entry->name, file->name, strlen(file->name) + 1);
    entry->bytes = file->bytes;
    return FX_OK;
}

static FxResult directory_close(void *context)
{
    (void)context;
    return FX_OK;
}

static FxResult directory_read(void *context, const char *name, uint8_t *data,
                               uint32_t capacity, uint32_t *actual)
{
    Directory *directory = context;
    assert(directory->next > 0);
    const File *file = &directory->files[directory->next - 1];
    assert(!strcmp(name, file->name) && capacity == file->bytes + 1);
    ++directory->reads;
    if (capacity > directory->largest_read)
        directory->largest_read = capacity;
    FILE *stream = fopen(file->path, "rb");
    if (!stream)
        return FX_IO;
    *actual = (uint32_t)fread(data, 1, capacity, stream);
    int failed = ferror(stream);
    return fclose(stream) || failed ? FX_IO : FX_OK;
}

static int add_file(Directory *directory, const char *path)
{
    if (directory->count == sizeof(directory->files) / sizeof(directory->files[0]))
        return 0;
    const char *name = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\')
            name = p + 1;
    }
    if (strlen(name) >= sizeof(((FxModuleDirectoryEntry *)0)->name))
        return 0;
    FILE *stream = fopen(path, "rb");
    if (!stream)
        return 0;
    int seek = fseek(stream, 0, SEEK_END);
    long bytes = ftell(stream);
    if (fclose(stream) || seek || bytes <= 0 || bytes > (long)FX_WLM_BUNDLE_LIMIT)
        return 0;
    directory->files[directory->count++] = (File){path, name, (uint32_t)bytes};
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 7 || strlen(argv[3]) != 64)
        return 2;
    uint32_t usable = (uint32_t)strtoul(argv[1], NULL, 10);
    if (usable < 1000000u || usable > FX_WLM_FROZEN_BUDGET)
        return 2;
    uint8_t target[32];
    for (size_t i = 0; i < sizeof(target); ++i) {
        char hex[] = {argv[3][i * 2], argv[3][i * 2 + 1], 0};
        target[i] = (uint8_t)strtoul(hex, NULL, 16);
    }
    uint64_t required = strtoull(argv[4], NULL, 16);
    const char *fit = argv[5];
    if (strcmp(fit, "first-fit") && strcmp(fit, "best-fit"))
        return 2;
    Directory directory = {0};
    for (int i = 6; i < argc; ++i) {
        if (!add_file(&directory, argv[i])) {
            fprintf(stderr, "could not stat input %s\n", argv[i]);
            return 3;
        }
    }
    Heap heap = {.best_fit = !strcmp(fit, "best-fit")};
    heap_init(&heap, usable);
    uint32_t initial_largest = largest_free(&heap);
    FxModuleDirectory io = {
        &directory, directory_open, directory_next, directory_close, directory_read,
    };
    FxFrozenModules frozen = {0};
    FxResult scan = fx_wlm_frozen_scan(&frozen, &io, target, FX_SDK_CAPS,
                                      (FxIndexMemory){&heap, heap_allocate, heap_release});
    uint32_t scan_peak = heap.peak, scan_committed_peak = heap.committed_peak;
    uint32_t retained = heap.live, scan_largest = largest_free(&heap);
    uint32_t frozen_bytes = frozen.bytes + frozen.inventory.index.bytes + frozen.packages.bytes;
    assert(frozen_bytes == retained);
    uint32_t frontends = 0;
    for (uint32_t i = 0; i < frozen.inventory.count; ++i)
        frontends += frozen.inventory.entries[i].bytes;

    FxWlmNativePlan plan = {0};
    FxResult composed = FX_STATE, sealed = FX_STATE;
    uint32_t after_compose = retained, after_release = retained;
    if (scan == FX_OK) {
        heap.peak = retained;
        heap.committed_peak = heap.committed;
        composed = fx_wlm_native_compose(frozen.native_views, frozen.native_count, target,
                                         required, (FxWlmNativeMemory){&heap, heap_allocate, heap_release},
                                         &plan);
        after_compose = heap.live;
        assert(after_compose == retained + plan.bytes);
        if (composed == FX_OK) {
            fx_wlm_frozen_release_native(&frozen);
            after_release = heap.live;
            sealed = fx_wlm_frozen_seal(&frozen);
            const FxModuleInventory *inventory = fx_wlm_frozen_inventory(&frozen);
            assert(sealed == FX_OK && inventory && inventory->count == directory.reads);
            FILE *output = fopen(argv[2], "wb");
            if (!output)
                return 4;
            size_t written = fwrite(plan.package, 1, plan.bytes, output);
            if (fclose(output) || written != plan.bytes)
                return 4;
            heap_release(&heap, plan.package);
        }
    }
    printf("{\"fit\":\"%s\",\"pool_bytes\":%u,\"frozen_budget\":%u,\"initial_largest_free\":%u,"
           "\"scan_result\":%u,\"scan_files\":%u,\"scan_native_components\":%u,"
           "\"scan_peak\":%u,\"scan_peak_with_overhead\":%u,\"scan_retained\":%u,"
           "\"scan_largest_free\":%u,\"largest_read\":%u,\"frontend_bytes\":%u,"
           "\"compose_result\":%u,\"compose_combined_peak\":%u,\"compose_peak_with_overhead\":%u,"
           "\"compose_retained\":%u,\"retained_after_release_native\":%u,"
           "\"failed_allocation\":%u,\"failed_free\":%u,\"failed_largest_free\":%u,"
           "\"plan_bytes\":%u,\"selection\":%u,\"seal_result\":%u}\n",
           fit, usable, FX_WLM_FROZEN_BUDGET, initial_largest, (unsigned)scan, directory.reads,
           frozen.native_count, scan_peak, scan_committed_peak, retained, scan_largest,
           directory.largest_read, frontends, (unsigned)composed, heap.peak,
           heap.committed_peak, after_compose, after_release, heap.failed_size, heap.failed_free,
           heap.failed_largest, plan.bytes, plan.selection, (unsigned)sealed);
    free(heap.arena);
    return 0;
}
