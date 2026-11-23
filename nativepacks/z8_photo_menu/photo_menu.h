/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FX_Z8_PHOTO_MENU_H
#define FX_Z8_PHOTO_MENU_H

#include "fenestra.h"
#include "fenestra_z8_photo_lut.h"
#include <stdatomic.h>

enum { PHOTO_RECORD_V1_BYTES = 64, PHOTO_RECORD_BYTES = 96, PHOTO_FILES_PER_PAGE = 20,
       PHOTO_DIR_BYTES = 0x220, PHOTO_PATH_BYTES = 12 + FX_Z8_LUT_PATH_BYTES,
       PHOTO_CACHE_FILES = 512 };
enum { PHOTO_ROOT, PHOTO_FOLDER };
#define PHOTO_FLAT_FOLDER "~"
enum { PHOTO_BURST_PAGE = 0x5d5, PHOTO_LUT_PAGE = 0x5d6, PHOTO_LUT_ROW = 0x4800 };
enum { PHOTO_C30_ITEM = 0x3ccc, PHOTO_C120_ITEM = 0x3ccd };
enum { PHOTO_FIRST_TEXT = 0x1f40, PHOTO_TEXT_END = 0x1f4b };
enum { PHOTO_FLIGHT_COUNT = 64, PHOTO_CRASH_LOG_BYTES = 4096 };
enum {
    PHOTO_CRASH_BURST = 1u, PHOTO_CRASH_DEADLINE = 2u, PHOTO_CRASH_FAULT = 4u,
    PHOTO_CRASH_STALL = 8u, PHOTO_CRASH_STOP = 16u
};

typedef struct {
    uint32_t stall_latched, stall_slot, stall_stage, stall_ms;
    uint32_t write_attempts, write_successes, last_write_err, last_write_ms;
} PhotoWatchdogStatus;
extern PhotoWatchdogStatus photo_watchdog_status;
extern _Atomic uint64_t crash_flush_request;
void photo_crash_request(uint32_t reason);
uint64_t photo_flight_ms(void);
uint16_t photo_flight_core(void);
void photo_flight_log(uint16_t event, uint16_t core_flags, uint32_t a, uint32_t b, uint32_t c);
int photo_flight_had_activity(void);
void photo_flight_reset_activity(void);
void photo_flight_watchdog(void);
void photo_flight_flushed(uint32_t pending);
uint32_t photo_flight_format(char *buffer, uint32_t reason, uint32_t pending, uint64_t now,
                             const FxZ8JpegFlightMetrics *, const FxZ8RearFlightMetrics *);

typedef struct {
    uint64_t generation;
    char path[FX_Z8_LUT_PATH_BYTES];
} PhotoChoice;

typedef struct {
    void *context;
    int (*current)(void *);
    FxResult (*open)(void *, const char *, uint32_t, int32_t *);
    FxResult (*read)(void *, int32_t, void *, uint32_t, uint32_t *);
    FxResult (*write)(void *, int32_t, const void *, uint32_t, uint32_t *);
    FxResult (*close)(void *, int32_t);
    FxResult (*remove)(void *, const char *);
    FxResult (*rename)(void *, const char *, const char *);
} PhotoFileIO;

typedef struct {
    PhotoChoice choice;
    uint32_t slot;
} PhotoJournal;

typedef struct {
    uint32_t count, previous, next, serial, epoch;
    char names[PHOTO_FILES_PER_PAGE][FX_Z8_LUT_NAME_BYTES];
    uint32_t level, focus;
    char category[FX_Z8_LUT_NAME_BYTES];
} PhotoPage;

typedef struct {
    uint32_t count, opposite;
    int32_t direction;
    char cursor[FX_Z8_LUT_NAME_BYTES];
    char names[PHOTO_FILES_PER_PAGE + 1][FX_Z8_LUT_NAME_BYTES];
} PhotoCatalog;

typedef enum {
    PHOTO_NO_LUT, PHOTO_QUEUED, PHOTO_READING, PHOTO_PREPARING, PHOTO_READY,
    PHOTO_MISSING, PHOTO_LOAD_ERROR, PHOTO_SAVE_ERROR, PHOTO_CLEAR_SAVE_ERROR,
    PHOTO_WORKER_ERROR, PHOTO_JOURNAL_ERROR, PHOTO_CLEAR_RETIRING
} PhotoPhase;

typedef struct {
    uint32_t ticket, phase, reason, done, total;
} PhotoStatus;

uint32_t photo_crc32(const uint8_t *, size_t);
FxResult photo_choice_read(const uint8_t *, size_t, PhotoChoice *);
FxResult photo_choice_write(const PhotoChoice *, uint8_t [PHOTO_RECORD_BYTES]);
FxResult photo_journal_load(const PhotoFileIO *, char drive, PhotoJournal *);
FxResult photo_journal_save(const PhotoFileIO *, char drive, const char *path);
FxResult photo_file_exists(const PhotoFileIO *, char drive, const char *path);
void photo_card_path(char drive, const char *relative, char *out);
void photo_folder_path(const char *category, const char *name, char *out);
int photo_dir_name(const uint8_t [PHOTO_DIR_BYTES], const char *category, int folders,
                   char [FX_Z8_LUT_NAME_BYTES]);
int photo_name_compare(const char *, const char *);
void photo_catalog_start(PhotoCatalog *, int32_t direction, const char *cursor);
void photo_catalog_add(PhotoCatalog *, const char name[FX_Z8_LUT_NAME_BYTES]);
void photo_catalog_finish(const PhotoCatalog *, PhotoPage *);

int photo_burst_rate(uint32_t row, int toggle);
uint32_t photo_lut_select(const char *name, uint64_t gui_epoch);
void photo_lut_request_page(int32_t direction);
void photo_lut_enter_folder(const char *category, uint32_t root_focus);
void photo_lut_leave_folder(uint32_t root_focus);
void photo_lut_close_browser(void);
int photo_lut_page(PhotoPage *);
PhotoStatus photo_lut_status(void);
int photo_worker_slice(void);
uint64_t photo_gui_epoch(void);
void photo_burst_power(uint32_t cancel);
uint64_t photo_open_arm(const char *path);
void photo_open_complete(uint64_t token);

uint32_t photo_text_lookup(int32_t language, int32_t id, const uint16_t **text, uint32_t *bytes);
void photo_ui_deactivate(void);
uint32_t photo_ui_pending(void);
#endif
