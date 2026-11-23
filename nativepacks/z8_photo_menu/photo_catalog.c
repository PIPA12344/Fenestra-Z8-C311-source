/* SPDX-License-Identifier: GPL-3.0-only */
#include "photo_menu.h"
#include "fenestra_memory.h"

static unsigned folded(unsigned byte) {
    return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}

int photo_name_compare(const char *a, const char *b) {
    while (*a && folded((unsigned char)*a) == folded((unsigned char)*b)) { ++a; ++b; }
    return (int)folded((unsigned char)*a) - (int)folded((unsigned char)*b);
}

static uint32_t dir_word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void photo_catalog_start(PhotoCatalog *catalog, int32_t direction, const char *cursor) {
    memset(catalog, 0, sizeof(*catalog));
    catalog->direction = direction;
    if (direction) memcpy(catalog->cursor, cursor, sizeof(catalog->cursor));
}

void photo_folder_path(const char *category, const char *name, char *out) {
    if (category[0] != PHOTO_FLAT_FOLDER[0]) {
        memcpy(out, "LUTS\\", 5);
        out += 5;
        while (*category) *out++ = *category++;
        if (name[0]) *out++ = '\\';
    }
    while (*name) *out++ = *name++;
    *out = '\0';
}

int photo_dir_name(const uint8_t entry[PHOTO_DIR_BYTES], const char *category, int folders,
                   char name[FX_Z8_LUT_NAME_BYTES]) {
    unsigned attrs = entry[0x20e];
    if ((attrs & 8u) || !!(attrs & 16u) != folders) return 0;
    if (!folders) {
        uint32_t bytes = dir_word(entry + 0x218);
        if (dir_word(entry + 0x21c) || !bytes || bytes > 2u * 1024u * 1024u) return 0;
    }
    memset(name, 0, FX_Z8_LUT_NAME_BYTES);
    int long_name = entry[0x0e] || entry[0x0f];
    size_t limit = long_name ? FX_Z8_LUT_NAME_BYTES : 13u;
    size_t n;
    for (n = 0; n < limit; ++n) {
        unsigned c = long_name ? (unsigned)entry[14 + 2 * n] + 256u * entry[15 + 2 * n] : entry[n];
        if (!c) break;
        if (c > 127) return 0;
        name[n] = (char)c;
    }
    if (n == limit) return 0;
    if (folders) return fx_z8_lut_category(name);
    if (!fx_z8_lut_name(name)) return 0;
    if (!category || category[0] == PHOTO_FLAT_FOLDER[0]) return 1;
    /* The relative path includes LUTS, two separators and a terminator. */
    for (size_t i = 0; category[i]; ++i) ++n;
    return n + 6u < FX_Z8_LUT_PATH_BYTES;
}

void photo_catalog_add(PhotoCatalog *catalog, const char name[FX_Z8_LUT_NAME_BYTES]) {
    int sign = catalog->direction < 0 ? -1 : 1;
    int from_cursor = photo_name_compare(name, catalog->cursor);
    if (catalog->direction && from_cursor * sign <= 0) {
        catalog->opposite = 1;
        return;
    }
    unsigned at = 0;
    for (; at < catalog->count; ++at) {
        int order = photo_name_compare(name, catalog->names[at]);
        if (!order) {
            if (memcmp(name, catalog->names[at], FX_Z8_LUT_NAME_BYTES) < 0)
                memcpy(catalog->names[at], name, FX_Z8_LUT_NAME_BYTES);
            return;
        }
        if (order * sign < 0) break;
    }
    if (at == PHOTO_FILES_PER_PAGE + 1u) return;
    if (catalog->count < PHOTO_FILES_PER_PAGE + 1u) ++catalog->count;
    for (unsigned i = catalog->count - 1; i > at; --i)
        memcpy(catalog->names[i], catalog->names[i - 1], FX_Z8_LUT_NAME_BYTES);
    memcpy(catalog->names[at], name, FX_Z8_LUT_NAME_BYTES);
}

void photo_catalog_finish(const PhotoCatalog *catalog, PhotoPage *page) {
    memset(page, 0, sizeof(*page));
    page->count = catalog->count > PHOTO_FILES_PER_PAGE ? PHOTO_FILES_PER_PAGE : catalog->count;
    int backwards = catalog->direction < 0;
    page->previous = backwards ? catalog->count > PHOTO_FILES_PER_PAGE : catalog->opposite;
    page->next = backwards ? catalog->opposite : catalog->count > PHOTO_FILES_PER_PAGE;
    for (unsigned i = 0; i < page->count; ++i) {
        unsigned from = backwards ? page->count - 1 - i : i;
        memcpy(page->names[i], catalog->names[from], sizeof(page->names[i]));
    }
}
