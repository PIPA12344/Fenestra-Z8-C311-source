"""Verify and compact the v0.5.1 text banks without changing the camera text ABI."""

import hashlib
import struct

from z8_native_arena import BASE, INIT_FILE_BEGIN, INIT_RAM_BEGIN, RESOURCE_ROOTS
from z8_native_capsule import IMAGE, POINTER, WRITE


LANGUAGES = 36
FIRST_EXTENDED = 3137
TEXT_COUNT = 3251
OFFSET_DIRECTORY = 0x4327C5A0
POOL_DIRECTORY = 0x439DAF58
STOCK_READONLY_BEGIN = 0x41600000
PRORES_SELECTION = 0x02
FILE_TYPE_HELP = 0xA17
OPEN_GATE_FILE_TYPE_HELP = 3233

# The extension IDs are reachable through the corresponding feature's menu hooks.
# The boundaries come from the pinned upstream z8patch.shared.STRING_IDS allocation.
GROUP_SELECTORS = {
    'opengate-nraw': (3137, 5, 0x01),
    'pixelshift': (3142, 5, 0x10),
    'prores': (3147, 6, 0x02),
    'anamorphic': (3153, 8, 0x01),
    'h265': (3161, 56, 0x04),
    'log': (3217, 8, 0x08),
    'opengate-e1': (3225, 9, 0x01),
    'sdr-heif': (3234, 4, 0x10),
    'photo-areas': (3238, 12, 0x10),
}
EXTENDED_SELECTORS = tuple(
    bit for _, count, bit in GROUP_SELECTORS.values() for _ in range(count)
)
EXPECTED_DEPENDENCIES = {FILE_TYPE_HELP: PRORES_SELECTION, OPEN_GATE_FILE_TYPE_HELP: PRORES_SELECTION}


def resolve_texts(image, *, text_count=TEXT_COUNT):
    """Read length-delimited UTF-16 bytes using the actual projected directories."""
    result = []
    for language in range(LANGUAGES):
        table = struct.unpack_from('<Q', image, OFFSET_DIRECTORY - BASE + 8 * language)[0] - BASE
        pool = struct.unpack_from('<Q', image, POOL_DIRECTORY - BASE + 8 * language)[0] - BASE
        if table < 0 or table + 4 * (text_count + 1) > len(image) or not 0 <= pool < len(image):
            raise ValueError(f'invalid locale directory for language {language}')
        offsets = struct.unpack_from(f'<{text_count + 1}i', image, table)
        strings = []
        for ident, (start, end) in enumerate(zip(offsets, offsets[1:])):
            start += pool
            end += pool
            if ident == FIRST_EXTENDED - 1 and end < start:
                strings.append(b'')  # The stock endpoint is a discontinuous spacer.
            elif 0 <= start <= end <= len(image) and not ((start | end) & 1) and end - start <= 65534:
                strings.append(bytes(image[start:end]))
            else:
                raise ValueError(f'invalid locale span for language {language}, ID {ident}')
        result.append(tuple(strings))
    return tuple(result)


def capsule_locale_texts(stock, patches):
    """Project a plan to MAIN and resolve the text payload exactly as the getter does."""
    image = bytearray(stock)
    icon_root = RESOURCE_ROOTS[0]
    readonly = [(STOCK_READONLY_BEGIN, BASE + len(stock))]
    for patch in patches:
        if (patch.address, len(patch.new), patch.flags) == (int(icon_root['address'], 0), icon_root['bytes'], POINTER):
            continue  # BSS resource root for the icon directory, outside file-backed locale text.
        offset = (INIT_FILE_BEGIN + patch.address - INIT_RAM_BEGIN
                  if patch.address >= INIT_RAM_BEGIN else patch.address - BASE)
        if offset < 0 or offset + len(patch.new) > len(image) or stock[offset:offset + len(patch.old)] != patch.old:
            raise ValueError(f'bad locale-verification preimage at {patch.address:#x}')
        image[offset:offset + len(patch.new)] = patch.new
        if patch.kind == IMAGE and not patch.flags & WRITE:
            readonly.append((patch.address, patch.address + len(patch.new)))
    for language in range(LANGUAGES):
        table = struct.unpack_from('<Q', image, OFFSET_DIRECTORY - BASE + 8 * language)[0]
        pool = struct.unpack_from('<Q', image, POOL_DIRECTORY - BASE + 8 * language)[0]
        for address, size, alignment in ((table, 4 * (TEXT_COUNT + 1), 4), (pool, 2, 2)):
            if address % alignment or not any(start <= address < address + size <= end for start, end in readonly):
                raise ValueError(f'language {language} locale span escapes the stock or sealed read-only image')
    return resolve_texts(image)


def reachable_ids(selection):
    return (*range(FIRST_EXTENDED), *(ident for ident, bit in enumerate(EXTENDED_SELECTORS, FIRST_EXTENDED)
                                      if selection & bit))


def text_digest(rows, selection):
    """Bind each reachable ID, language, exact length and UTF-16 payload."""
    digest = hashlib.sha256()
    ids = reachable_ids(selection)
    for row in rows:
        for ident in ids:
            text = row[ident]
            digest.update(struct.pack('<HH', ident, len(text)))
            digest.update(text)
    return digest.hexdigest()


class LocaleAudit:
    """Find selector dependencies along each edge of the 31-selection cube.

    Stock IDs and each owner's reachable extended IDs form connected sets of
    selections. Testing every single-bit edge therefore detects any dependency.
    """

    def __init__(self):
        self.fingerprints = {}
        self.witnesses = {}

    def observe(self, selection, rows):
        ids = set(reachable_ids(selection))
        fingerprints = {
            ident: hashlib.sha256(b''.join(struct.pack('<H', len(text)) + text for text in translations)).digest()
            for ident, translations in enumerate(zip(*rows)) if ident in ids
        }
        dependent = self.witnesses
        for bit in (1, 2, 4, 8, 16):
            neighbor = self.fingerprints.get(selection ^ bit)
            if neighbor is None:
                continue
            for ident in fingerprints.keys() & neighbor.keys():
                if fingerprints[ident] != neighbor[ident]:
                    dependent[ident] = dependent.get(ident, 0) | bit
        self.fingerprints[selection] = fingerprints

    def verify(self):
        if set(self.fingerprints) != set(range(1, 32)) or self.witnesses != EXPECTED_DEPENDENCIES:
            raise ValueError(f'v0.5.1 locale dependencies differ: {self.witnesses}')
        return dict(selections=len(self.fingerprints), languages=LANGUAGES, text_count=TEXT_COUNT,
                    selector_mask=PRORES_SELECTION, dependencies={hex(k): v for k, v in sorted(self.witnesses.items())})
