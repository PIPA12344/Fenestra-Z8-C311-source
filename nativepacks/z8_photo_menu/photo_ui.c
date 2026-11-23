/* SPDX-License-Identifier: GPL-3.0-only */
#include "photo_menu.h"
#include "fenestra_memory.h"
#include <stdatomic.h>

#define FN(address, type) ((type)(uintptr_t)(address))
#define CURRENT_PAGE FN(0x403dbdb4, void *(*)(void))
#define FIND_WIDGET FN(0x4039e810, void *(*)(uint32_t, void *))
#define FIND_CHILD FN(0x4050a6f0, void *(*)(void *, uint32_t, uint32_t))
#define DRAW_FENCE FN(0x402e76d4, void (*)(void))
#define DIRTY FN(0x403f62b4, void (*)(void *, uint32_t))
#define DYNAMIC FN(0x403f621c, void (*)(void *, uint32_t))
#define SET_TEXT FN(0x403f2920, void (*)(const char *, uint32_t, uint32_t))
#define SET_WIDE FN(0x403f2a58, void (*)(const uint16_t *, uint32_t, uint32_t))
#define BIND_TEXT FN(0x4031e178, int (*)(void *, uint32_t, uint32_t))
#define LANGUAGE FN(0x4032db54, uint32_t (*)(void))
#define TEXT_FONT FN(0x4032e19c, uint32_t (*)(uint32_t, uint32_t))
#define ROW_COUNT FN(0x403afed4, uint32_t (*)(void *))
#define ROW_DISABLE FN(0x403b1ed0, void (*)(void *, uint32_t, uint32_t))
#define ROW_FOCUS FN(0x403aff78, void (*)(void *, uint32_t, uint32_t))
#define ROW_DRAW UINT64_C(0x4031f060)
#define LIST_LAYOUT FN(0x403ae874, void (*)(void *))
#define VIEW_LAYOUT FN(0x403aeb38, void (*)(void *))
#define SCROLLBAR FN(0x403aec24, void (*)(void *))
#define RECT FN(0x4031e2a8, void (*)(void *, const int16_t *))
#define VIEW_ID FN(0x403f0db8, uint32_t (*)(uint32_t))
#define VIEW_SET FN(0x4039ecc4, void (*)(uint32_t, const int16_t *))
#define VIEW_RECT FN(0x4039ed6c, void (*)(uint32_t, int16_t *))
#define MARK FN(0x4039e848, void (*)(void *, uintptr_t))
#define REDRAW FN(0x403efed8, void (*)(uintptr_t, uint32_t, uint32_t))
#define TIMER_START FN(0x403d7084, int (*)(uint32_t, uint32_t, void (*)(void)))
#define TIMER_CANCEL FN(0x403d7144, void (*)(uint32_t))
#define STOCK_BACK_ACTION FN(0x403b1ba4, void (*)(void *))

enum { TIMER_SLOT = 0x1a, TITLE_STATUS = 0x1405, VALUE_CHILD = 0x4ba7 };
/* Both stock text setters rewrite their font preset. Use separate font
 * records for ASCII filenames and wide localized rows, including the header. */
enum { TEXT_BASE = 0xfc, FONT_FILE = 0x207, FONT_VALUE = 0x208, FONT_WIDE = 0x21f };
enum { ROW_PREVIOUS = 21, ROW_NEXT = 22, LUT_ROWS = 23, STATUS_SLOT = 23 };
static const uintptr_t descriptors[] = {0, 0x40b6e200, 0x40b6e260};
enum { BURST = 1, LUT = 2 };
enum { UI_LEFT = 0x285, UI_TOUCH_BACK = 0x2a0, UI_STAY = 0x68c };

/* 0x403DB910 initializes this request for the action call at 0x40508DA0.
 * The dispatcher rereads target at 0x40508E04. Its condition has a different ABI. */
typedef struct {
    uint8_t *page;
    uint32_t event, target;
} MenuAction;
_Static_assert(offsetof(MenuAction, target) == 0xc, "native action target");

typedef struct {
    uint8_t record[24];
    uint8_t buffer[51];
} TextLease;

static struct {
    uint8_t *page;
    uint8_t *resume_page;
    uint64_t lease_epoch;
    uint32_t type, resume_type, epoch, timer_epoch, page_bound, language;
    uint32_t in_folder, return_pending, root_focus;
    uint16_t root_row, root_scroll;
    int16_t root_view[4];
    int last_rates[2];
    uint8_t saved_disabled[LUT_ROWS];
    uint8_t saved_fonts[2][16];
    TextLease saved_text[STATUS_SLOT + 1];
    PhotoPage page_view;
    PhotoStatus last_status;
} ui;
static _Atomic uint32_t ui_leased;

typedef struct { const uint16_t *english, *chinese; } Labels;
#define WIDE(text) ((const uint16_t *)u##text)
#define LABEL(english, chinese) {WIDE(english), WIDE(chinese)}
static const Labels labels[] = {
    LABEL("Continuous shooting speed", "连拍速度"),
    LABEL("Set C30 and C120 shooting speeds.", "设置 C30 和 C120 的连拍速度。"),
    LABEL("Select LUT", "选择 LUT"),
    LABEL("Choose a LUT for the rear display and JPEG photos.", "选择用于显示屏和 JPEG 照片的 LUT。"),
    LABEL("Continuous shooting speed", "连拍速度"),
    LABEL("C30 shooting speed", "C30 连拍速度"),
    LABEL("C120 shooting speed", "C120 连拍速度"),
    LABEL("Off", "无LUT"),
    LABEL("Prev", "上一页"),
    LABEL("Next", "下一页"),
    LABEL("", "")
};
_Static_assert(sizeof(labels) / sizeof(labels[0]) == PHOTO_TEXT_END - PHOTO_FIRST_TEXT, "published text ID count");

static const Labels back_label = LABEL("Back", "返回上一级");
static const Labels flat_label = LABEL("Root LUTs", "根目录 LUT");
static const struct { const char *category; Labels label; } categories[] = {
    {"01-Fuji-Film", LABEL("01 Fuji Film", "FUJI 色调")},
    {"02-Kodak-Negative", LABEL("02 Kodak Negative", "Kodak 负片")},
    {"03-Classic-Negative", LABEL("03 Classic Negative", "传统负片")},
    {"04-Color-Slide", LABEL("04 Color Slide", "彩色反转片")},
    {"05-Black-and-White", LABEL("05 Black and White", "黑白片")},
    {"06-Instant-Polaroid", LABEL("06 Instant Polaroid", "拍立得即显")},
    {"07-Cinema-Print", LABEL("07 Cinema Print", "电影色调")},
    {"08-Camera-Style", LABEL("08 Camera Style", "相机直出与机内风格")},
    {"09-Portrait-Daily", LABEL("09 Portrait Daily", "人像与日常拍摄")},
    {"10-Creative-Vintage", LABEL("10 Creative Vintage", "创意色调")},
};

uint32_t photo_text_lookup(int32_t language, int32_t id, const uint16_t **text, uint32_t *bytes) {
    uint32_t index = (uint32_t)id - PHOTO_FIRST_TEXT;
    if (index >= sizeof(labels) / sizeof(labels[0])) return FX_ARGUMENT;
    *text = language == 8 ? labels[index].chinese : labels[index].english;
    uint32_t n = 0;
    while ((*text)[n]) ++n;
    *bytes = n * sizeof(uint16_t);
    return 0;
}

static void *font_record(uint32_t font) {
    return (void *)(uintptr_t)(UINT64_C(0xa032bf0c8) + 16u * font);
}

static void *text_record(uint32_t index) {
    return (void *)(uintptr_t)(UINT64_C(0xa032bd4a8) + 24u * (TEXT_BASE + index));
}

static void widgets_lease(int save) {
    uint32_t slots = ui.type == BURST ? 2u : STATUS_SLOT + 1u;
    for (uint32_t i = 0; i < slots; ++i) {
        TextLease *lease = &ui.saved_text[i];
        uint8_t *record = text_record(i);
        uintptr_t buffer;
        if (save) memcpy(lease->record, record, sizeof(lease->record));
        memcpy(&buffer, lease->record + 8, sizeof(buffer));
        if (save) {
            if (buffer) memcpy(lease->buffer, (const void *)buffer, sizeof(lease->buffer));
        } else {
            if (buffer) memcpy((void *)buffer, lease->buffer, sizeof(lease->buffer));
            memcpy(record, lease->record, sizeof(lease->record));
        }
    }
    uint32_t fonts[2] = {ui.type == BURST ? FONT_VALUE : FONT_FILE, FONT_WIDE};
    for (unsigned i = 0; i < (ui.type == BURST ? 1u : 2u); ++i) {
        void *record = font_record(fonts[i]);
        if (save) memcpy(ui.saved_fonts[i], record, sizeof(ui.saved_fonts[i]));
        else memcpy(record, ui.saved_fonts[i], sizeof(ui.saved_fonts[i]));
    }
    if (ui.type == LUT) {
        void *flags = (void *)(uintptr_t)(UINT64_C(0xa032aa850) + PHOTO_LUT_ROW - 0x2921u);
        if (save) memcpy(ui.saved_disabled, flags, LUT_ROWS);
        else memcpy(flags, ui.saved_disabled, LUT_ROWS);
    }
}

static uintptr_t load_pointer(const uint8_t *memory) {
    uintptr_t value;
    memcpy(&value, memory, sizeof(value));
    return value;
}

static uint32_t load_halfword(const uint8_t *memory) {
    return (uint32_t)memory[0] | (uint32_t)memory[1] << 8;
}

static void store_halfword(uint8_t *memory, uint16_t value) {
    memcpy(memory, &value, sizeof(value));
}

static int same_page(const void *page, uint32_t type) {
    return page && load_pointer((const uint8_t *)page + 0x50) == descriptors[type];
}

static uint32_t absolute_row(const uint8_t *page) {
    return load_halfword(page + 0x32) + load_halfword(page + 0x58);
}

static void repaint(uint8_t *page) {
    MARK(page + 0x18, 0x403efe48);
    REDRAW(0x403efe74, 3, 7);
}

static void bind_ascii(void *node, uint32_t index, uint32_t font, const char *value) {
    if (!node) return;
    SET_TEXT(value, TEXT_BASE + index, font);
    BIND_TEXT(node, 0, 0);
    BIND_TEXT(node, TEXT_BASE + index, 3);
    DIRTY(node, 1);
}

static const uint16_t *local_label(const Labels *label, uint32_t language) {
    return language == 8 ? label->chinese : label->english;
}

static void bind_wide(void *node, uint32_t index, uint32_t language, const uint16_t *value) {
    SET_WIDE(value, TEXT_BASE + index, FONT_WIDE);
    if (language == 8) *(uint8_t *)font_record(FONT_WIDE) = (uint8_t)TEXT_FONT(language, 0x8ae);
    BIND_TEXT(node, 0, 0);
    BIND_TEXT(node, TEXT_BASE + index, 4);
    DIRTY(node, 1);
}

static void bind_status(void *node, uint32_t language, const PhotoStatus *s) {
    if (!node) return;
    static const Labels statuses[] = {
        LABEL("No LUT", "无LUT"), LABEL("Loading", "等待载入"), LABEL("Reading", "正在读取"),
        LABEL("Preparing", "正在处理"), LABEL("Ready", "已载入"), LABEL("LUT file missing", "找不到 LUT 文件"),
        LABEL("LUT load failed", "LUT 载入失败"), LABEL("Ready / save failed", "已载入，保存失败"),
        LABEL("No LUT / save failed", "无LUT，保存失败"), LABEL("LUT worker failed", "LUT 启动失败"),
        LABEL("LUT settings invalid", "LUT 设置读取失败"), LABEL("No LUT / closing preview", "无LUT，正在关闭预览")
    };
    static const Labels invalid = LABEL("Invalid LUT file", "LUT 文件无效");
    static const Labels resources = LABEL("Insufficient memory", "内存不足");
    static const Labels close_error = LABEL("Card close failed", "存储卡关闭失败");
    const Labels *label = &statuses[s->phase < sizeof(statuses) / sizeof(statuses[0]) ? s->phase : PHOTO_LOAD_ERROR];
    if (s->phase == PHOTO_LOAD_ERROR) {
        if (s->reason == FX_ARGUMENT || s->reason == FX_INTEGRITY) label = &invalid;
        else if (s->reason == FX_BUDGET) label = &resources;
        else if (s->reason == FX_CLOSE) label = &close_error;
    }
    const uint16_t *text = local_label(label, language);
    uint16_t display[25];
    unsigned n = 0;
    while (text[n]) { display[n] = text[n]; ++n; }
    if (s->phase == PHOTO_PREPARING && s->total && s->done <= s->total) {
        uint32_t percent = (uint32_t)((uint64_t)s->done * 100u / s->total);
        display[n++] = ' ';
        if (percent == 100) display[n++] = '1';
        if (percent >= 10) display[n++] = (uint16_t)('0' + percent / 10u % 10u);
        display[n++] = (uint16_t)('0' + percent % 10u);
        display[n++] = '%';
    }
    display[n] = 0;
    DYNAMIC(node, 1);
    bind_wide(node, STATUS_SLOT, language, display);
}

static int timer_active(void) {
    return *(volatile const uint8_t *)(uintptr_t)0xa032b4ba0ull != 0;
}

void photo_lut_timer(void);
int photo_lut_entry(void *page);
int photo_burst_entry(void *page);

static void release_ui(void) {
    if (!ui.page) return;
    ui.page = NULL;
    ++ui.epoch;
    ui.timer_epoch = 0;
    if (timer_active() &&
        load_pointer((const uint8_t *)(uintptr_t)0xa032b4bb0ull) == (uintptr_t)photo_lut_timer)
        TIMER_CANCEL(TIMER_SLOT);
    DRAW_FENCE();
    widgets_lease(0);
    if (ui.type == LUT) photo_lut_close_browser();
    ui.type = 0;
    ui.lease_epoch = 0;
    atomic_store_explicit(&ui_leased, 0, memory_order_release);
}

uint32_t photo_ui_pending(void) {
    return atomic_load_explicit(&ui_leased, memory_order_acquire);
}

static int lease_current(void) {
    if (!ui.page) return 0;
    if (ui.lease_epoch == photo_gui_epoch()) return 1;
    ui.resume_page = ui.page;
    ui.resume_type = ui.type;
    release_ui();
    return 0;
}

static void maybe_timer(void) {
    if (lease_current() && ui.type == LUT && !timer_active()) {
        ui.timer_epoch = ui.epoch;
        if (!TIMER_START(TIMER_SLOT, 100, photo_lut_timer)) ui.timer_epoch = 0;
        (void)lease_current();
    }
}

void photo_ui_deactivate(void) {
    if (!lease_current()) {
        if (ui.resume_page && photo_gui_epoch()) {
            uint8_t *page = ui.resume_page;
            uint32_t type = ui.resume_type;
            ui.resume_page = NULL;
            if (CURRENT_PAGE() == page && same_page(page, type)) {
                if (type == LUT) (void)photo_lut_entry(page);
                else (void)photo_burst_entry(page);
            }
        }
        return;
    }
    if (CURRENT_PAGE() != ui.page || !same_page(ui.page, ui.type)) release_ui();
    else maybe_timer();
}

static int start_ui(void *page, uint32_t type) {
    uint64_t epoch = photo_gui_epoch();
    int leased = lease_current();
    if (!epoch || !same_page(page, type)) return 0;
    if (leased && ui.page == page && ui.type == type) return 1;
    release_ui();
    uint32_t unleased = 0;
    if (!atomic_compare_exchange_strong(&ui_leased, &unleased, 1)) return 0;
    if (photo_gui_epoch() != epoch) {
        atomic_store_explicit(&ui_leased, 0, memory_order_release);
        return 0;
    }
    ui.type = type;
    ui.page = page;
    ui.resume_page = NULL;
    ui.lease_epoch = epoch;
    ++ui.epoch;
    ui.page_bound = 0;
    ui.in_folder = ui.return_pending = ui.root_focus = 0;
    ui.last_status.phase = UINT32_MAX;
    ui.language = UINT32_MAX;
    ui.last_rates[0] = ui.last_rates[1] = -2;
    widgets_lease(1);
    return lease_current();
}

int photo_burst_refresh(void *page) {
    if (!lease_current() || ui.page != page || ui.type != BURST || !same_page(page, BURST)) return 0;
    static const char *const values[2][2] = {{"30 fps", "40 fps (FX RAW)"}, {"120 fps", "140 fps (RAW)"}};
    int changed = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int rate = photo_burst_rate(i, 0);
        if (rate == ui.last_rates[i]) continue;
        void *node = FIND_CHILD(page, PHOTO_C30_ITEM + i, VALUE_CHILD);
        if (!node) continue;
        bind_ascii(node, i, FONT_VALUE, rate >= 0 ? values[i][rate] : "-");
        ui.last_rates[i] = rate;
        changed = 1;
    }
    if (changed) repaint(page);
    (void)lease_current();
    return 0;
}

int photo_burst_entry(void *page) {
    if (start_ui(page, BURST)) photo_burst_refresh(page);
    return 0;
}

int photo_burst_exit(void *page) {
    if (lease_current() && ui.page == page && ui.type == BURST) release_ui();
    return 0;
}

static void browser_layout(uint8_t *page, const PhotoPage *view) {
    uint32_t old = absolute_row(page);
    int restore = view->focus && view->focus == ui.root_focus;
    if (old < LUT_ROWS) ROW_FOCUS(page, old, 0);
    for (uint32_t i = 0; i < LUT_ROWS; ++i) {
        uint32_t item = PHOTO_LUT_ROW + i;
        int show = i <= view->count || (i == ROW_PREVIOUS && view->previous) || (i == ROW_NEXT && view->next);
        void *widget = FIND_WIDGET(item, page + 0x18);
        ROW_DISABLE(page, i, (uint32_t)!show);
        if (!widget) continue;
        uintptr_t row_draw = ROW_DRAW;
        memcpy((uint8_t *)widget + 0x48, &row_draw, sizeof(row_draw));
        DIRTY(widget, (uint32_t)show);
        for (uint32_t child_id = VALUE_CHILD; child_id <= 0x4baa; ++child_id) {
            void *child = FIND_CHILD(page, item, child_id);
            if (child) DYNAMIC(child, 0);
        }
    }
    store_halfword(page + 0x30, (uint16_t)ROW_COUNT(page));
    store_halfword(page + 0x32, restore ? ui.root_row : 0);
    store_halfword(page + 0x58, restore ? ui.root_scroll : 0);
    LIST_LAYOUT(page);
    for (uint32_t i = 0; i < LUT_ROWS; ++i) {
        uint8_t *widget = FIND_WIDGET(PHOTO_LUT_ROW + i, page + 0x18);
        if (!widget) continue;
        int16_t bounds[4];
        memcpy(bounds, widget + 0x10, sizeof(bounds));
        bounds[2] = (int16_t)(bounds[0] + 708);
        RECT(widget, bounds);
        const int16_t text[4] = {(int16_t)(bounds[0] + 6), (int16_t)(bounds[1] + 8),
                                (int16_t)(bounds[2] - 6), (int16_t)(bounds[1] + 48)};
        memcpy(widget + 0xc0, text, sizeof(text));
    }
    VIEW_LAYOUT(page);
    if (restore) {
        store_halfword(page + 0x32, ui.root_row);
        store_halfword(page + 0x58, ui.root_scroll);
    }
    uint32_t view_id = VIEW_ID(2);
    int16_t rect[4] = {0};
    if (restore) memcpy(rect, ui.root_view, sizeof(rect));
    else {
        VIEW_RECT(view_id, rect);
        rect[3] = (int16_t)(rect[3] - rect[1]);
        rect[1] = 0;
    }
    VIEW_SET(view_id, rect);
    ROW_FOCUS(page, restore ? view->focus : 0, 1);
    SCROLLBAR(page);
}

static const Labels *category_label(const char *name) {
    if (name[0] == PHOTO_FLAT_FOLDER[0]) return &flat_label;
    for (unsigned i = 0; i < sizeof(categories) / sizeof(categories[0]); ++i)
        if (!photo_name_compare(name, categories[i].category)) return &categories[i].label;
    return NULL;
}

static void browser_text(uint8_t *page, const PhotoPage *view, uint32_t language) {
    for (uint32_t i = 0; i < LUT_ROWS; ++i) {
        const Labels *label = NULL;
        const char *name = NULL;
        if (!i) label = view->level == PHOTO_FOLDER ? &back_label : &labels[7];
        else if (i <= view->count) {
            name = view->names[i - 1];
            if (view->level == PHOTO_ROOT) label = category_label(name);
        } else if (i == ROW_PREVIOUS && view->previous) label = &labels[8];
        else if (i == ROW_NEXT && view->next) label = &labels[9];
        if (!label && !name) continue;
        void *node = FIND_WIDGET(PHOTO_LUT_ROW + i, page + 0x18);
        if (!node) continue;
        if (label) bind_wide(node, i, language, local_label(label, language));
        else bind_ascii(node, i, FONT_FILE, name);
    }
}

int photo_lut_refresh(void *page_arg) {
    uint8_t *page = page_arg;
    if (!lease_current() || ui.page != page || ui.type != LUT || !same_page(page, LUT)) return 0;
    PhotoPage view;
    int published = photo_lut_page(&view);
    int placeholder = !published && (!ui.page_bound || (ui.in_folder && ui.page_view.level == PHOTO_ROOT));
    uint32_t language = LANGUAGE();
    int language_changed = ui.language != language;
    int changed = placeholder || (published && (!ui.page_bound || memcmp(&view, &ui.page_view, sizeof(view))));
    if (changed) {
        if (placeholder) {
            memset(&view, 0, sizeof(view));
            view.level = ui.in_folder ? PHOTO_FOLDER : PHOTO_ROOT;
        }
        browser_layout(page, &view);
        ui.page_view = view;
        ui.page_bound = 1;
        ui.in_folder = view.level == PHOTO_FOLDER;
        if (!ui.in_folder) ui.return_pending = 0;
    }
    if (language_changed) {
        memcpy(font_record(FONT_FILE), ui.saved_fonts[0], sizeof(ui.saved_fonts[0]));
        memcpy(font_record(FONT_WIDE), ui.saved_fonts[1], sizeof(ui.saved_fonts[1]));
    }
    if (changed || language_changed) browser_text(page, &ui.page_view, language);
    PhotoStatus now = photo_lut_status();
    if (language_changed || memcmp(&now, &ui.last_status, sizeof(now))) {
        bind_status(FIND_WIDGET(TITLE_STATUS, page + 0x18), language, &now);
        ui.last_status = now;
        ui.language = language;
        changed = 1;
    }
    if (changed) repaint(page);
    maybe_timer();
    return 0;
}

void photo_lut_timer(void) {
    if (!lease_current() || ui.type != LUT || !ui.timer_epoch || ui.timer_epoch != ui.epoch) return;
    if (CURRENT_PAGE() != ui.page || !same_page(ui.page, LUT)) { release_ui(); return; }
    photo_lut_refresh(ui.page);
}

int photo_lut_entry(void *page) {
    int existing = lease_current() && ui.page == page && ui.type == LUT;
    if (!existing && start_ui(page, LUT)) photo_lut_request_page(0);
    return photo_lut_refresh(page);
}

int photo_lut_exit(void *page) {
    if (lease_current() && ui.page == page && ui.type == LUT) release_ui();
    return 0;
}

static int owns_action_page(const MenuAction *action, uint32_t type) {
    return action && ui.type == type && ui.page == action->page &&
           ui.lease_epoch == photo_gui_epoch() && same_page(action->page, type);
}

static uint32_t event_row(const MenuAction *action, uint32_t type, uint32_t base_item) {
    if (!owns_action_page(action, type)) return UINT32_MAX;
    uint32_t event = action->event;
    if (event != 0x286 && event != 0x28b && event != 0x29f && event != 0x2ae) return UINT32_MAX;
    uint32_t row = absolute_row(action->page);
    const uint8_t *desc = (const uint8_t *)descriptors[type];
    if (row >= load_halfword(desc + 0x20)) return UINT32_MAX;
    const uint8_t *rows = (const uint8_t *)load_pointer(desc + 0x10);
    uint32_t item;
    memcpy(&item, rows + row * 0x2cu + 0x18u, sizeof(item));
    return item == base_item + row ? row : UINT32_MAX;
}

int photo_burst_activate(void *action) {
    uint32_t row = event_row(action, BURST, PHOTO_C30_ITEM);
    if (row < 2 && photo_burst_rate(row, 1) >= 0) photo_burst_refresh(ui.page);
    return 0;
}

static void leave_folder(void) {
    if (!ui.return_pending) {
        ui.return_pending = 1;
        photo_lut_leave_folder(ui.root_focus);
    }
}

int photo_lut_back_action(MenuAction *action) {
    if (!action) return 0;
    if (owns_action_page(action, LUT) && ui.in_folder) {
        action->target = UI_STAY;
        leave_folder();
    } else if (action->event == UI_LEFT || action->event == UI_TOUCH_BACK) {
        STOCK_BACK_ACTION(action);
    }
    return 0;
}

int photo_lut_activate(void *action) {
    uint32_t row = event_row(action, LUT, PHOTO_LUT_ROW);
    if (row >= LUT_ROWS) return 0;
    /* A folder can be left while its first snapshot is still scanning. Its
     * placeholder has only Back and never exposes stale file indices. */
    if (!row && ui.page_bound && ui.page_view.level == PHOTO_FOLDER && !ui.page_view.serial) {
        leave_folder();
        return 0;
    }
    PhotoPage current;
    if (!ui.page_bound || !photo_lut_page(&current) ||
        current.epoch != ui.page_view.epoch || current.serial != ui.page_view.serial) return 0;
    if (!row) {
        if (current.level == PHOTO_FOLDER) leave_folder();
        else (void)photo_lut_select("", ui.lease_epoch);
    } else if (row <= current.count) {
        const char *name = current.names[row - 1];
        if (current.level == PHOTO_ROOT) {
            ui.root_row = (uint16_t)load_halfword(ui.page + 0x32);
            ui.root_scroll = (uint16_t)load_halfword(ui.page + 0x58);
            ui.root_focus = row;
            VIEW_RECT(VIEW_ID(2), ui.root_view);
            photo_lut_enter_folder(name, row);
            ui.in_folder = 1;
        } else {
            char path[FX_Z8_LUT_PATH_BYTES];
            photo_folder_path(current.category, name, path);
            (void)photo_lut_select(path, ui.lease_epoch);
        }
    } else if (row == ROW_PREVIOUS && current.previous) photo_lut_request_page(-1);
    else if (row == ROW_NEXT && current.next) photo_lut_request_page(1);
    /* Stock finishes dispatch before the refresh/timer binds a new snapshot. */
    return 0;
}
