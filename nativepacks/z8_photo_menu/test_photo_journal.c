/* SPDX-License-Identifier: GPL-3.0-only */
#include "photo_menu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { SLOT0, SLOT1, TEMP, LOOK, OTHER, NESTED, FILES };
enum { SWAP_NONE, SWAP_DEST_CHECK, SWAP_TEMP_RECHECK };
typedef struct {
    uint8_t data[PHOTO_RECORD_BYTES + 1];
    uint32_t length, exists;
} File;
typedef struct {
    File cards[2][FILES];
    File replacement[FILES];
    File *open_file;
    int32_t descriptor, last_fd;
    uint32_t offset, calls, fail_at, replace_at, short_write, closed_uncertain;
    uint32_t max_read, swap, swapped, dest, temp_created, temp_checks, removed_slots;
    char drive;
    int current;
} Card;

static const char no_lut[FX_Z8_LUT_NAME_BYTES] = "";
static const char look[FX_Z8_LUT_NAME_BYTES] = "look.cube";
static const char other[FX_Z8_LUT_NAME_BYTES] = "other.CUBE";
static const char nested[] = "LUTS\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz.cube";
_Static_assert(sizeof(nested) == FX_Z8_LUT_PATH_BYTES, "exercise the longest path");

static int current(void *ctx) { return ((Card *)ctx)->current; }

static unsigned file_index(const Card *card, const char *name) {
    static const char *const names[FILES] = {"FXPHOTO0.CFG", "FXPHOTO1.CFG", "FXPHOTO.TMP",
                                             "look.cube", "other.CUBE", nested};
    assert(!memcmp(name + 1, ":\\FENESTRA\\", 11));
    assert(name[0] == card->drive);
    for (unsigned i = 0; i < FILES; ++i) {
        if (!strcmp(name + 12, names[i])) return i;
    }
    assert(0 && "unexpected filename");
    return FILES;
}

static File *file(Card *card, const char *name) {
    File *mounted = card->swapped ? card->replacement : card->cards[card->drive - 'A'];
    return mounted + file_index(card, name);
}

static int at_fault(Card *card) {
    ++card->calls;
    if (card->calls == card->replace_at) card->current = 0;
    return card->calls == card->fail_at;
}

static FxResult open_file(void *ctx, const char *name, uint32_t flags, int32_t *fd) {
    Card *card = ctx;
    assert(card->current && !card->open_file);
    assert(!card->closed_uncertain);
    if (at_fault(card)) return FX_IO;
    unsigned slot = file_index(card, name);
    if (!flags && card->temp_created) {
        if (slot == TEMP) ++card->temp_checks;
        if ((slot == card->dest && card->swap == SWAP_DEST_CHECK) ||
            (slot == TEMP && card->temp_checks == 2 && card->swap == SWAP_TEMP_RECHECK))
            card->swapped = 1;  /* Foundation still reports the same drive. */
    }
    File *f = file(card, name);
    if (flags) {
        assert(flags == 0x101);
        if (f->exists) return FX_IO;
        f->exists = 1;
        f->length = 0;
        assert(slot == TEMP);
        card->temp_created = 1;
    } else if (!f->exists) return FX_EMPTY;
    card->open_file = f;
    card->offset = 0;
    *fd = card->descriptor = ++card->last_fd;
    return FX_OK;
}

static FxResult read_file(void *ctx, int32_t fd, void *out, uint32_t amount, uint32_t *done) {
    Card *card = ctx;
    assert(card->current && card->open_file && card->descriptor == fd);
    assert((uintptr_t)out % 64u == 0);
    if (at_fault(card)) return FX_IO;
    uint32_t left = card->open_file->length - card->offset;
    if (amount > left) amount = left;
    if (amount > card->max_read) amount = card->max_read;
    memcpy(out, card->open_file->data + card->offset, amount);
    card->offset += amount;
    *done = amount;
    return FX_OK;
}

static FxResult write_file(void *ctx, int32_t fd, const void *in, uint32_t amount, uint32_t *done) {
    Card *card = ctx;
    assert(card->current && card->open_file && card->descriptor == fd && !card->offset);
    if (at_fault(card)) return FX_IO;
    assert(amount == PHOTO_RECORD_BYTES && !card->open_file->length);
    if (card->short_write) --amount;
    memcpy(card->open_file->data, in, amount);
    card->open_file->length = *done = amount;
    return FX_OK;
}

static FxResult close_file(void *ctx, int32_t fd) {
    Card *card = ctx;
    assert(card->open_file && card->descriptor == fd && !card->closed_uncertain);
    if (at_fault(card)) { card->closed_uncertain = 1; return FX_CLOSE; }
    card->open_file = NULL;
    return FX_OK;
}

static FxResult remove_file(void *ctx, const char *name) {
    Card *card = ctx;
    assert(card->current && !card->open_file);
    File *dest = file(card, name);
    if (file_index(card, name) <= SLOT1) ++card->removed_slots;
    if (at_fault(card)) return FX_IO;
    assert(dest->exists);
    dest->exists = 0;
    return FX_OK;
}

static FxResult rename_file(void *ctx, const char *from, const char *to) {
    Card *card = ctx;
    assert(card->current && !card->open_file);
    File *src = file(card, from), *dst = file(card, to);
    if (at_fault(card)) return FX_IO;
    assert(src->exists && !dst->exists && src->length == PHOTO_RECORD_BYTES);
    *dst = *src;
    src->exists = 0;
    return FX_OK;
}

static PhotoFileIO io(Card *card) {
    return (PhotoFileIO){card, current, open_file, read_file, write_file, close_file, remove_file, rename_file};
}

static void seed(File *file, uint64_t generation, const char *name) {
    PhotoChoice record = {.generation = generation};
    size_t length = strlen(name);
    assert(length < sizeof(record.path));
    memcpy(record.path, name, length);
    assert(photo_choice_write(&record, file->data) == FX_OK);
    file->length = PHOTO_RECORD_BYTES;
    file->exists = 1;
}

static void checksum(uint8_t *data, size_t bytes) {
    uint32_t crc = photo_crc32(data, bytes - 4u);
    for (unsigned i = 0; i < 4; ++i) data[bytes - 4u + i] = (uint8_t)(crc >> (8u * i));
}

static void seed_v1(File *file, uint64_t generation, const char *name) {
    memset(file->data, 0, sizeof(file->data));
    memcpy(file->data, "FXPHOTO\0", 8);
    file->data[8] = 1;
    file->data[10] = PHOTO_RECORD_V1_BYTES;
    for (unsigned i = 0; i < 8; ++i) file->data[12 + i] = (uint8_t)(generation >> (8u * i));
    file->data[20] = *name != '\0';
    file->data[21] = (uint8_t)strlen(name);
    memcpy(file->data + 24, name, strlen(name));
    checksum(file->data, PHOTO_RECORD_V1_BYTES);
    file->length = PHOTO_RECORD_V1_BYTES;
    file->exists = 1;
}

static Card card_fixture(char drive, unsigned active, uint32_t max_read) {
    Card card = {.drive = drive, .current = 1, .max_read = max_read, .dest = active ^ 1u};
    seed_v1(&card.cards[drive - 'A'][active], 2, look);
    seed(&card.cards[drive - 'A'][active ^ 1u], 1, no_lut);
    /* Stale temp is ignored at boot and checked before exclusive create. */
    seed(&card.cards[drive - 'A'][TEMP], 99, other);
    seed(&card.cards[('B' - drive)][active], 4, other);
    return card;
}

static void reopen(Card *card) {
    card->current = 1;
    card->open_file = NULL;
    card->fail_at = card->replace_at = card->closed_uncertain = 0;
}

static unsigned fault_boundaries(void) {
    unsigned injected = 0;
    for (char drive = 'A'; drive <= 'B'; ++drive) {
        for (unsigned active = 0; active < 2; ++active) {
            for (unsigned tombstone = 0; tombstone < 2; ++tombstone) {
                const char *name = tombstone ? no_lut : nested;
                Card clean = card_fixture(drive, active, 11);
                PhotoFileIO ops = io(&clean);
                assert(photo_journal_save(&ops, drive, name) == FX_OK);
                for (unsigned replace = 0; replace < 2; ++replace) {
                    for (uint32_t step = 1; step <= clean.calls; ++step) {
                        Card card = card_fixture(drive, active, 11);
                        card.fail_at = replace ? 0 : step;
                        card.replace_at = replace ? step : 0;
                        ops = io(&card);
                        FxResult rc = photo_journal_save(&ops, drive, name);
                        assert(rc != FX_OK);
                        /* The previous active slot always remains intact. A
                         * final-readback interruption may leave the new slot
                         * complete even though it was not acknowledged. */
                        assert(!memcmp(card.cards[drive - 'A'][active].data,
                                       clean.cards[drive - 'A'][active].data, PHOTO_RECORD_BYTES));
                        assert(card.cards[drive - 'A'][active].exists);
                        reopen(&card);
                        PhotoJournal restored;
                        assert(photo_journal_load(&ops, drive, &restored) == FX_OK);
                        if (restored.choice.generation == 2) assert(!strcmp(restored.choice.path, look));
                        else {
                            assert(restored.choice.generation == 3);
                            assert(!strcmp(restored.choice.path, name));
                        }
                        card.drive = drive == 'A' ? 'B' : 'A';
                        assert(photo_journal_load(&ops, card.drive, &restored) == FX_OK);
                        assert(restored.choice.generation == 4 && !strcmp(restored.choice.path, other));
                        ++injected;
                    }
                }
            }
        }
    }
    return injected;
}

static unsigned same_letter_swaps(void) {
    unsigned cases = 0;
    for (char drive = 'A'; drive <= 'B'; ++drive) {
        for (unsigned active = SLOT0; active <= SLOT1; ++active) {
            for (unsigned scenario = 0; scenario < 4; ++scenario) {
                Card card = card_fixture(drive, active, 11);
                card.swap = scenario < 2 ? SWAP_DEST_CHECK : SWAP_TEMP_RECHECK;
                /* The original active slot can be byte-identical on two
                 * cards. The replacement destination may even reuse the
                 * recorded generation with different payload and CRC. */
                seed_v1(&card.replacement[active], scenario == 1 ? 0x5 : 2, look);
                seed(&card.replacement[active ^ 1u], scenario == 1 ? 1 : 9, other);
                if (scenario == 0) card.cards[drive - 'A'][active ^ 1u].exists = 0;
                if (scenario < 2) seed(&card.replacement[TEMP], 3, no_lut);
                if (scenario == 3) seed(&card.replacement[TEMP], 3, other);
                File unchanged = card.replacement[active ^ 1u];
                PhotoFileIO ops = io(&card);
                assert(photo_journal_save(&ops, drive, no_lut) == FX_STALE);
                assert(card.swapped && card.current && !card.removed_slots);
                File *dest = &card.replacement[active ^ 1u];
                assert(dest->exists && dest->length == unchanged.length &&
                       !memcmp(dest->data, unchanged.data, unchanged.length));
                assert(card.cards[drive - 'A'][active].exists);
                ++cases;
            }
        }
    }
    return cases;
}

static void record_cases(void) {
    Card card = {.drive = 'A', .current = 1, .max_read = 65};
    File *files = card.cards[0];
    PhotoFileIO ops = io(&card);
    PhotoJournal chosen;
    seed(&files[TEMP], 10, other);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_EMPTY);
    seed_v1(&files[SLOT0], 1, no_lut);
    seed_v1(&files[SLOT1], 2, look);
    assert(!memcmp(files[SLOT0].data + 60, "\x44\x79\xaa\xd4", 4));
    assert(!memcmp(files[SLOT1].data + 60, "\xfb\x62\xd6\x1c", 4));
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 1);
    assert(photo_file_exists(&ops, 'A', chosen.choice.path) == FX_EMPTY);
    files[LOOK].exists = 1;
    assert(photo_file_exists(&ops, 'A', chosen.choice.path) == FX_OK);
    files[SLOT1].data[60] ^= 1;
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0);
    files[SLOT0].data[60] ^= 1;
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_INTEGRITY);
    assert(photo_journal_save(&ops, 'A', look) == FX_OK);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0);
    seed_v1(&files[SLOT1], 1, look);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0);
    seed(&files[SLOT1], 1, other);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_INTEGRITY);
    assert(photo_journal_save(&ops, 'A', no_lut) == FX_INTEGRITY);
    ++files[SLOT1].length;
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0);
    card.short_write = 1;
    assert(photo_journal_save(&ops, 'A', no_lut) == FX_IO);
    assert(photo_journal_load(&ops, 'A', &chosen) == FX_OK && chosen.slot == 0);
    card.short_write = 0;
    seed(&files[SLOT0], UINT64_MAX, look);
    assert(photo_journal_save(&ops, 'A', no_lut) == FX_BOUNDS);
    const uint8_t reserved[] = {21, 22, 23, 24, 55, 56, 57, 58, 59};
    for (unsigned i = 0; i < sizeof(reserved); ++i) {
        seed_v1(&files[SLOT0], 1, no_lut);
        files[SLOT0].data[reserved[i]] = 1;
        checksum(files[SLOT0].data, PHOTO_RECORD_V1_BYTES);
        assert(photo_choice_read(files[SLOT0].data, PHOTO_RECORD_V1_BYTES, &chosen.choice) == FX_INTEGRITY);
    }
}

static void nested_records(void) {
    Card card = {.drive = 'B', .current = 1, .max_read = 13};
    File *files = card.cards[1];
    PhotoFileIO ops = io(&card);
    PhotoJournal chosen;
    seed(&files[SLOT0], 100, nested);
    seed_v1(&files[SLOT1], 101, look);
    assert(photo_journal_load(&ops, 'B', &chosen) == FX_OK && chosen.slot == 1);
    assert(photo_journal_save(&ops, 'B', nested) == FX_OK);
    assert(files[SLOT0].length == PHOTO_RECORD_BYTES && files[SLOT0].data[8] == 2);
    assert(files[SLOT0].data[21] == 63 && !memcmp(files[SLOT0].data + 24, nested, sizeof(nested)));
    assert(photo_journal_load(&ops, 'B', &chosen) == FX_OK && chosen.slot == 0);
    assert(chosen.choice.generation == 102 && !strcmp(chosen.choice.path, nested));
    assert(photo_file_exists(&ops, 'B', chosen.choice.path) == FX_EMPTY);
    files[NESTED].exists = 1;
    assert(photo_file_exists(&ops, 'B', chosen.choice.path) == FX_OK);
    assert(photo_journal_save(&ops, 'B', "LUTS\\..\\look.cube") == FX_ARGUMENT);
    const uint8_t invalid[] = {8, 10, 20, 21, 22, 23, 25, 87, 88, 91, 92, 95};
    for (unsigned i = 0; i < sizeof(invalid); ++i) {
        seed(&files[SLOT0], 102, nested);
        files[SLOT0].data[invalid[i]] ^= 1;
        if (invalid[i] < 92) checksum(files[SLOT0].data, PHOTO_RECORD_BYTES);
        /* Changing U in the required prefix makes this path invalid. */
        assert(photo_journal_load(&ops, 'B', &chosen) == FX_OK && chosen.slot == 1);
    }
}

int main(void) {
    record_cases();
    nested_records();
    unsigned injected = fault_boundaries();
    unsigned swaps = same_letter_swaps();
    printf("Journal V1/V2 validated on A and B across %u I/O failures and replacements and %u same-letter swaps\n",
           injected, swaps);
    return 0;
}
