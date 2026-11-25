"""Build the installable Z8 C3.11 Cube LUT WLM with its JPEG provider and shooting menu."""

import argparse
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import re
import struct
import subprocess
from tempfile import TemporaryDirectory

from build_z8_jpeg_lut import (FOUNDATION, LLVM, audit_simd,
                               build as build_jpeg, compile_audited_unit,
                               verify_jpeg_extension_preimage)
from build_z8_lut_preview import build as build_rear
from wlm_bundle import decode_bundle, encode_bundle, summary
from wlm_native import compose_components, decode_component, encode_component
from z8_native_arena import BASE, STOCK_SHA256, arena_profile, verify_arena
from z8_native_capsule import DATA, EXEC, HOOK, IMAGE, INSTRUCTION, POINTER, WRITE, Patch, decode, encode, sha
from z8_ui_profile import get_profile, read_original


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
STOCK_PATH = ROOT / "inputs/z8-c311-main.bin"
FOUNDATION_PATH = BUILD / "cold-retry3/core/z8-core.main"
OUTPUT = BUILD / "cube-lut"
MENU_SOURCE = ROOT / "nativepacks/z8_photo_menu"
MENU_UNITS = ("photo_entry.S", "photo_journal.c", "photo_catalog.c", "photo_flight.c", "photo_worker.c", "photo_ui.c")
RELEASE = ROOT / "inputs/r10-c1/photo_tools.wlm"
RELEASE_SHA = "aaefcb935612b0aff2a175d19aaca4c08265095add3c8c854917fca9d6eb9b0a"
R10_COMPONENT_SHA = {
    "photo_tools": "070e4d9ed792d4ad80ecfa6b679dddd55b6643f7d0f4a5c617e78c282971235f",
    "shared.locale": "d1d5045db53d6196517e2ac0023a88a85202c963bb54751d03e697df2ea00c78",
    "shared.movie": "bc16488c6710edb20362e26aaca092cc81c4ba77c08206e66a16277d8c29574f",
}
TARGET = bytes.fromhex(STOCK_SHA256)
REQUIRED = 0x81947C592AF69A37
NAME = "z8.photo.menu"
BURST = 0x010
LUT = 0x400
MASK = BURST | LUT
VALUES = (BURST, LUT, MASK)
ARENA = (0x40B60000, 0x40B80000)
RX, RO, RW, STACK = 0x40B60000, 0x40B6C000, 0x40B70000, 0x40B78000
SEGMENTS = ((RX, RO, 5, EXEC), (RO, RW, 4, 0), (RW, STACK, 6, WRITE), (STACK, ARENA[1], 6, WRITE))
FLIGHT_SYMBOLS = {
    "photo_flight_entry": RX + 0x340, "photo_heartbeat_abi": RX + 0x348,
    "photo_flight_ring": 0x40B73000, "photo_flight_head": 0x40B73800,
    "photo_heartbeat_slots": 0x40B73808, "photo_watchdog_status": 0x40B73908,
}
FLIGHT_END = 0x40B73928

PAGE_ROOT = 0xA028F4CC0
PARENT_PAGE = 0x19
PARENT_POINTER = 0xA028F5670
PARENT_ORIG = 0xA028CC480
PARENT_ORIG_ROWS = 0x43DCF790
PARENT_ORIG_EVENTS = 0x43DCFFA8
PARENT_ROWS = 0x40B6C080
PARENT_EVENTS = 0x40B6C8F0
PARENT_TEMPLATE_EVENT = 0x43E286C0
PARENT_COUNTS = {BURST: 48, LUT: 48, MASK: 49}
PARENT_ACTIVATIONS = {5: 0x286, 6: 0x28B, 9: 0x29F, 11: 0x2AE}
PARENT_NEW = {
    BURST: (0x2DE6, 0x1F40, 0x1F41, 0x5D5, 0x40B6CA80),
    LUT: (0x2DE7, 0x1F42, 0x1F43, 0x5D6, 0x40B6D620),
}
ROW_BYTES = 0x2C
EVENT_BYTES = 0x20
PIXEL_ROW = 45
PIXEL_EVENT = 0x43E27B40
PIXEL_EFFECTIVE = (0xC46, 0xC47, 0xC46, 0, 0x101040, 0xFFFF, 0x2C13, 0x40004,
                   0xFFFFFFFF, 0xFFFFFFFF, 2)
CHILD_TEMPLATE = 0xA028F5800
CHILD_EVENTS_TEMPLATE = 0x43BFF8E8
CHILD_EVENTS = (0x27E, 0x27F, 0x283, 0x284, 0x285, 0x286, 0x28B, 0x29D,
                0x29E, 0x29F, 0x2A0, 0x2AE, 0x281, 0x306, 0x3BB)
BURST_PAGE, LUT_PAGE = 0x5D5, 0x5D6
BURST_STATE, LUT_STATE = 0xA02917CA0, 0xA02917D00
BURST_DESC, LUT_DESC = 0x40B6E200, 0x40B6E260
BURST_VECTOR, LUT_VECTOR = 0x40B6E2C0, 0x40B6E2E8
BURST_LAYOUT, BURST_ROWS, BURST_EVENTS, BURST_EVENT_TABLE = 0x40B6E320, 0x40B6E430, 0x40B6E488, 0x40B6E498
LUT_LAYOUT, LUT_ROWS, LUT_EVENTS, LUT_EVENT_TABLE = 0x40B6E680, 0x40B6E85C, 0x40B6EC50, 0x40B6ED08
TEXT_HOOKS = ((0x4032B5DC, "d50a0094", "c9d22094"), (0x4032B72C, "810a0094", "75d22094"))
OPEN_HOOK = (0x402DD4F8, "f60f1df8", "c20a2214")
BURST_POWER_HOOKS = ((0x40D67338, RX + 0x240), (0x40D6732C, RX + 0x2C0))
BURST_POWER_LOAD = bytes.fromhex('610240b9')
APPROVED_BSS = {(0xA02F2AB80, POINTER, "d0961d4300000000", "0000b24000000000")}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def audit_chinese_glyphs(stock, source):
    """Use only the exact language-8 strings addressed by Nikon's lookup table."""
    language = 8
    profile = get_profile()['text']
    count = profile['base']
    require(count == 3136, 'stock language-8 corpus count differs')

    table, = unpack('Q', stock, profile['table_directory_file'] + 8 * language)
    pool, following = unpack('2Q', stock, profile['blob_directory_file'] + 8 * language)
    offsets = unpack(f'{count + 1}I', read_original(stock, table, 4 * (count + 1)))
    require(offsets[0] == 0 and offsets[-1] == following - pool and
            all(a <= b and not (a | b) & 1 for a, b in zip(offsets, offsets[1:])),
            'stock language-8 text table differs')
    blob = read_original(stock, pool, offsets[-1])
    corpus = set().union(*(blob[a:b].decode('utf-16-le') for a, b in zip(offsets, offsets[1:])))

    code = re.sub(r'^\s*#.*$', '', source, flags=re.MULTILINE)
    literal = r'("(?:[^"\\\n]|\\.)*")'
    pairs = re.findall(r'\bLABEL\(\s*' + literal + r'\s*,\s*' + literal + r'\s*\)', code)
    require(len(pairs) == len(re.findall(r'\bLABEL\s*\(', code)) and len(pairs) >= 11,
            'Chinese glyph audit requires literal labels and status strings')
    used = {char for _, chinese in pairs for char in json.loads(chinese)} | set(' 0123456789%')
    # Audit standalone UTF-16 literals too, including literals containing only ASCII.
    used.update(char for quoted in re.findall(r'\bu' + literal, code) for char in json.loads(quoted))
    # Catch newly added non-ASCII literals outside the localized label table.
    used.update(char for quoted in re.findall(literal, code) for char in quoted if ord(char) > 127)
    missing = sorted(used - corpus)
    require(not missing, 'stock language-8 corpus missing ' + ', '.join(f'{ch} U+{ord(ch):04X}' for ch in missing))
    non_ascii = {char for char in used if ord(char) > 127}
    return dict(language=language, stock_strings=count, stock_corpus_sha256=sha(blob),
                label_count=len(pairs), distinct_non_ascii=len(non_ascii),
                characters=''.join(sorted(used)), coverage_percent=100, missing=[])


def verify_text_shim(rx, lookup=None):
    start = RX + 0x100
    mov, sub, cmp_, in_range, tail = unpack('5I', rx, 0x100)
    require((mov, sub, cmp_) == (0x5283E809, 0x4B090029, 0x71002D3F) and
            in_range & 0xFF00001F == 0x54000003 and tail & 0xFC000000 == 0x14000000,
            'text shim must intercept exactly 0x1f40..0x1f4a')

    def signed_branch(word, mask):
        sign = (mask + 1) >> 1
        immediate = word & mask
        return ((immediate ^ sign) - sign) * 4

    local = start + 12 + signed_branch(in_range >> 5, 0x7FFFF)
    require(RX <= local < RX + len(rx) and (lookup is None or local == lookup) and
            start + 16 + signed_branch(tail, 0x3FFFFFF) == 0x4032E130,
            'text shim must tail-branch unused IDs directly to stock')


def checked_file(path, expected):
    data = path.read_bytes()
    require(sha(data) == expected, f"pinned input SHA-256 differs for {path}")
    return data


def unpack(fmt, data, offset=0):
    return struct.unpack_from("<" + fmt, data, offset)


def projected(main, patches, address, size):
    data = bytearray(read_original(main, address, size))
    for patch in patches:
        first = max(address, patch.address)
        last = min(address + size, patch.address + len(patch.new))
        if first < last:
            begin = first - patch.address
            data[first-address:last-address] = patch.new[begin:begin+last-first]
    return bytes(data)


def bundle_at(path, expected=None):
    data = checked_file(path, expected) if expected else path.read_bytes()
    return decode_bundle(data, module_id=path.stem, target=TARGET)


def bundle_components(bundle):
    return [row["data"] for row in bundle["components"]]


def released_photo(path):
    result = bundle_at(path, RELEASE_SHA)
    require({row["id"]: row["sha256"] for row in result["components"]} == R10_COMPONENT_SHA,
            "r10-c1 release native components differ")
    return result


def preimages(stock, foundation):
    """Check exact loader and UI ingress before compiling a patch."""
    require(arena_profile()["requires_id"] == REQUIRED, "foundation native dependency differs")
    verify_arena(stock)
    require(sha(foundation) == FOUNDATION, "exact cold-retry3 foundation MAIN required")
    require(not any(foundation[RX-BASE:0x40BD0000-BASE]), "foundation menu reservation is occupied")
    verify_jpeg_extension_preimage(stock, foundation)
    checks = [
        (PARENT_POINTER, bytes.fromhex("80c48c020a000000")),
        (BURST_STATE, bytes(96)), (LUT_STATE, bytes(96)),
        *((site, bytes.fromhex(old)) for site, old, _ in (*TEXT_HOOKS, OPEN_HOOK)),
        *((site, BURST_POWER_LOAD) for site, _ in BURST_POWER_HOOKS),
    ]
    for address, old in checks:
        require(read_original(stock, address, len(old)) == read_original(foundation, address, len(old)) == old,
                f"menu hook preimage differs at {address:#x}")
    descriptor = read_original(stock, PARENT_ORIG, 80)
    require(sha(descriptor) == "95b4499df9384a1ccaa34b3ef91a570450cd4400251d002d8acbc80ecda1cf65"
            and descriptor == read_original(foundation, PARENT_ORIG, 80), "stock photo descriptor changed")
    require(unpack("I", descriptor)[0] == 0x80001003
            and unpack("3QH", descriptor, 8) == (0x43DCF680, PARENT_ORIG_ROWS, PARENT_ORIG_EVENTS, 47),
            "stock photo descriptor format changed")
    require(read_original(stock, 0x401CDF9C, 4) == bytes(4)
            and read_original(foundation, 0x401CDF9C, 4).hex() == "20013fd6",
            "scoped startup-file open caller differs")
    require(read_original(stock, 0x4032E130, 4).hex() == "098995d2"
            and read_original(foundation, 0x4032E130, 4).hex() == "949dfa17",
            "static text tail call target differs")
    for page, address in ((BURST_PAGE, BURST_STATE), (LUT_PAGE, LUT_STATE)):
        require(PAGE_ROOT + page * 96 == address, "group-2 page slot mapping differs")
    return dict(parent_descriptor_sha256=sha(descriptor), startup_open_lr="0x401cdfa0",
                reference_original_arena_zero=True, reference_foundation_arena_zero=True,
                hooks=[dict(address=hex(address), bytes=len(old), preimage=old.hex()) for address, old in checks])


def parent_tables(foundation, released_patches, selection, *, v051=False):
    photo = bool(selection & BURST)
    patch_view = released_patches if photo else ()
    first_rows = projected(foundation, patch_view, PARENT_ORIG_ROWS, 47 * ROW_BYTES)
    first_pointers = projected(foundation, patch_view, PARENT_ORIG_EVENTS, 47 * 8)
    stock_rows = read_original(foundation, PARENT_ORIG_ROWS, 47 * ROW_BYTES)
    stock_pointers = read_original(foundation, PARENT_ORIG_EVENTS, 47 * 8)
    pi = PIXEL_ROW * ROW_BYTES
    expected_rows = (stock_rows[:pi] + struct.pack("<11I", *PIXEL_EFFECTIVE) + stock_rows[pi + ROW_BYTES:]
                     if photo else stock_rows)
    require(first_rows == expected_rows and first_pointers == stock_pointers, "effective parent rows or pointers differ")
    require(unpack("Q", first_pointers, PIXEL_ROW * 8)[0] == PIXEL_EVENT, "Pixel Shift row event pointer differs")
    if photo:
        from z8_photo_tools_profile import V051_SYMBOLS

        action = V051_SYMBOLS["psm_menu_action"] if v051 else 0x40990000
        available = V051_SYMBOLS["psm_menu_available"] if v051 else 0x40992240
        for index, event in PARENT_ACTIVATIONS.items():
            on_card = projected(foundation, patch_view, PIXEL_EVENT + index * EVENT_BYTES, EVENT_BYTES)
            require(unpack("4Q", on_card) == (event, action, available, 0x1D),
                    "Pixel Shift activation differs")
        for site, target in ((0xA028E6670, 0x4099017C), (0xA028E6690, 0x409902EC)):
            require(projected(foundation, patch_view, site, 8) == struct.pack("<Q", target),
                    "r10-c1 Photo Shooting Menu callback differs")
    return first_rows, first_pointers


class Layout:
    """Emit fixed data plus link-time virtual addresses of native callbacks."""

    def __init__(self):
        self.offset = RO
        self.lines = ['.section .photo_layout,"a",%progbits']

    def place(self, address, label=None):
        require(self.offset <= address < RW, "generated Photo Menu RO arrays overlap or escape reservation")
        self.lines.append(f".org {address - RO:#x}")
        self.offset = address
        if label:
            self.lines.extend((f".global {label}", f"{label}:"))

    def raw(self, data):
        for start in range(0, len(data), 24):
            self.lines.append(".byte " + ", ".join(f"{byte:#04x}" for byte in data[start:start+24]))
        self.offset += len(data)

    def quads(self, values):
        for value in values:
            self.lines.append(f".quad {value if isinstance(value, str) else hex(value)}")
        self.offset += len(values) * 8

    def source(self):
        return "\n".join(self.lines) + "\n"


def parent_event_program(stock, target):
    program = bytearray(read_original(stock, PARENT_TEMPLATE_EVENT, 0xBA0))
    require(unpack("4Q", program, 92 * EVENT_BYTES) == (0x3BB, 0, 0, 0x68C),
            "parent event template has no expected terminal")
    for index, event in PARENT_ACTIVATIONS.items():
        address = index * EVENT_BYTES
        require(unpack("4Q", program, address) == (event, 0x403B1BA8, 0x403B1C64 if index == 9 else 0, 0xC6),
                "parent activation event template differs")
        struct.pack_into("<Q", program, address + 0x18, target)
    return bytes(program)


def child_descriptor(original, flags, layout, rows, events, count, viewport=None):
    descriptor = bytearray(original)
    struct.pack_into("<I", descriptor, 0, flags)
    struct.pack_into("<3QH", descriptor, 8, layout, rows, events, count)
    if viewport:
        struct.pack_into("<4h", descriptor, 0x22, *viewport)
    return descriptor


def child_events(layout, address, stock, action, back=None):
    table = read_original(stock, CHILD_EVENTS_TEMPLATE, 15 * EVENT_BYTES)
    transitions = list(struct.iter_unpack("<4Q", table))
    require(tuple(row[0] for row in transitions) == CHILD_EVENTS, "child event template differs")
    layout.place(address)
    for event, callback, guard, target in transitions:
        if event in PARENT_ACTIVATIONS.values():
            require(callback == 0x403B1BA8 and guard == (0x403B1C64 if event == 0x29F else 0),
                    "child activate ABI changed")
            callback, target = action, 0x68C
        elif event in (0x285, 0x2A0, 0x306):
            require(target == 0x68D and callback == (0 if event == 0x306 else 0x403B1BA4)
                    and guard == (0x403B1C64 if event == 0x2A0 else 0),
                    "child back template differs")
            target = PARENT_PAGE
            if back:
                # Stock passes the mutable transition request only to the
                # action. The condition slot gets the notification result.
                callback = back
        layout.quads((event, callback, guard, target))


def lut_layout(stock):
    def resource(index):
        return list(unpack("17I", read_original(stock, 0x43DF2608 + index * 68, 68)))

    title, footer, select, status = [resource(index) for index in (1, 7, 8, 5)]
    require(title[14] == 0x2E5 and status[14] == 0x3E7 and select[0] == 0xABE,
            "Pixel Shift browser frame templates differ")
    # Keep title and progress in distinct rectangles in the fixed header.
    title[0], title[7], title[14] = 0x1F42, 340 | (48 << 16), 0x1400
    footer[14] = 0x1402
    select[0], select[14] = 0x1F4A, 0x1403
    back = select.copy()
    back[14] = 0x1404
    status[0], status[14] = 0x1F4A, 0x1405
    status[6:10] = [350 | (8 << 16), 714 | (48 << 16)] * 2
    status[10:14] = [56, 56, 39, 39]
    return b"".join(struct.pack("<17I", *row) for row in (resource(0), title, footer, select, back, status)) + bytes(68)


def descriptor_source(stock, foundation, released_patches, selection):
    require(selection in VALUES, "unsupported Photo Menu selection")
    children = [kind for kind in (BURST, LUT) if selection & kind]
    rows, pointers = parent_tables(foundation, released_patches, selection)
    parent = read_original(stock, PARENT_ORIG, 80)
    layout = Layout()
    layout.place(RO)
    layout.raw(child_descriptor(parent, 0x80001003, 0x43DCF680, PARENT_ROWS, PARENT_EVENTS,
                                PARENT_COUNTS[selection]))
    layout.place(PARENT_ROWS)
    layout.raw(rows)
    row_template = unpack("11I", rows, 46 * ROW_BYTES)
    require(row_template == (0x702, 0xA3C, 0x702, 0, 0x101040, 0xFFFF, 0x2D0D, 0x40004,
                             0xFFFFFFFF, 0xFFFFFFFF, 2), "parent row clone template differs")
    for kind in children:
        item, title, help_id, _, _ = PARENT_NEW[kind]
        row = list(row_template)
        row[0], row[1], row[2], row[6] = title, help_id, title, item
        layout.raw(struct.pack("<11I", *row))

    layout.place(PARENT_EVENTS)
    layout.raw(pointers)
    layout.quads([PARENT_NEW[kind][4] for kind in children])
    require(unpack("Q", pointers, 46 * 8)[0] == PARENT_TEMPLATE_EVENT, "parent template event pointer differs")
    for kind in children:
        _, _, _, child, address = PARENT_NEW[kind]
        layout.place(address)
        layout.raw(parent_event_program(stock, child))

    # Keep all child objects and their symbol order identical in every
    # variant. Only the selected child state records are published.
    stock_burst = read_original(stock, 0xA028CC610, 80)
    require(sha(stock_burst) == "2cbd67efd25080966d91d6666925be821b35c3f03b17d2b72736ad7365b04850",
            "stock burst descriptor template differs")
    layout.place(BURST_DESC)
    layout.raw(child_descriptor(stock_burst, 0x80001008, BURST_LAYOUT, BURST_ROWS, BURST_EVENTS, 2))
    layout.place(LUT_DESC)
    layout.raw(child_descriptor(stock[0x4BDF810:0x4BDF860], 0x80001818, LUT_LAYOUT, LUT_ROWS, LUT_EVENTS, 23,
                                (6, 60, 714, 418)))
    for address, prefix in ((BURST_VECTOR, "photo_burst"), (LUT_VECTOR, "photo_lut")):
        layout.place(address)
        layout.quads((prefix + "_entry", prefix + "_exit", 0x403B2210, 0, prefix + "_refresh"))
    layout.place(BURST_LAYOUT)
    burst_layout = bytearray(read_original(stock, 0x43DD0930, 0x110))
    require(unpack("I", burst_layout, 0x44)[0] == 0x15A and unpack("I", burst_layout, 0x7C)[0] == 0x157,
            "burst title location differs")
    struct.pack_into("<I", burst_layout, 0x44, 0x1F44)
    layout.raw(burst_layout)
    layout.place(BURST_ROWS)
    template = list(unpack("11I", read_original(stock, 0x43DD0A40, ROW_BYTES)))
    require(template[4] == 0x10104D and template[6] == 0x2AC7, "burst value row template differs")
    for index in range(2):
        row = template.copy()
        row[0], row[6] = 0x1F45 + index, 0x3CCC + index
        layout.raw(struct.pack("<11I", *row))
    layout.place(BURST_EVENTS)
    layout.quads([BURST_EVENT_TABLE] * 2)
    child_events(layout, BURST_EVENT_TABLE, stock, "photo_burst_activate")
    layout.place(LUT_LAYOUT)
    layout.raw(lut_layout(stock))
    layout.place(LUT_ROWS)
    template = list(unpack("11I", read_original(stock, 0x43F1F704, ROW_BYTES)))
    require(template[4] == 0x101E00 and template[6] == 0x4675, "LUT browser row template differs")
    for index in range(23):
        row = template.copy()
        row[0] = {0: 0x1F47, 21: 0x1F48, 22: 0x1F49}.get(index, 0x1F4A)
        row[4] |= 65
        row[6] = 0x4800 + index
        layout.raw(struct.pack("<11I", *row))
    layout.place(LUT_EVENTS)
    layout.quads([LUT_EVENT_TABLE] * 23)
    child_events(layout, LUT_EVENT_TABLE, stock, "photo_lut_activate", "photo_lut_back_action")
    layout.place(0x40B6EEE8, "photo_feature_selection")
    layout.raw(struct.pack("<I", selection))
    return layout.source()


def child_state(stock, page, vector, descriptor):
    state = bytearray(read_original(stock, CHILD_TEMPLATE, 96))
    require(unpack("I", state)[0] == 0x001E0002 and unpack("Q", state, 0x50)[0] == 0xA028CC610,
            "stock child page state template differs")
    struct.pack_into("<I", state, 0, (page << 16) | 2)
    struct.pack_into("<Q", state, 0x28, vector)
    struct.pack_into("<Q", state, 0x50, descriptor)
    return bytes(state)


def hooks_for(stock, selection):
    yield Patch(HOOK, POINTER, PARENT_POINTER, read_original(stock, PARENT_POINTER, 8), struct.pack("<Q", RO), 1)
    for site, old, new in TEXT_HOOKS:
        yield Patch(HOOK, INSTRUCTION, site, bytes.fromhex(old), bytes.fromhex(new), 0)
    if selection & BURST:
        yield Patch(HOOK, DATA, BURST_STATE, bytes(96), child_state(stock, BURST_PAGE, BURST_VECTOR, BURST_DESC), 1)
    if selection == BURST:
        for site, target in BURST_POWER_HOOKS:
            displacement = target - site
            require(displacement % 4 == 0 and -(1 << 27) <= displacement < (1 << 27), 'burst power branch out of range')
            branch = struct.pack('<I', 0x14000000 | ((displacement // 4) & 0x3FFFFFF))
            yield Patch(HOOK, INSTRUCTION, site, BURST_POWER_LOAD, branch, 0)
    site, old, new = OPEN_HOOK
    yield Patch(HOOK, INSTRUCTION, site, bytes.fromhex(old), bytes.fromhex(new), 0)
    if selection & LUT:
        yield Patch(HOOK, DATA, LUT_STATE, bytes(96), child_state(stock, LUT_PAGE, LUT_VECTOR, LUT_DESC), 1)


def tool(llvm, name):
    path = llvm / (name + ".exe")
    return path if path.is_file() else llvm / name


def execute(command, timeout=90):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"{command[0]} exited {result.returncode}\n{result.stdout}{result.stderr}")
    return result.stdout


def inspect_menu_elf(path, llvm, stock, foundation):
    data = path.read_bytes()
    kind, machine, version, entry, phdr, shdr, _, _, stride, count, shstride, shcount, _ = unpack("HHIQQQIHHHHHH", data, 16)
    require(data[:16] == b"\x7fELF\x02\x01\x01" + bytes(9)
            and (kind, machine, version, stride, shstride) == (2, 183, 1, 56, 64) and count in (3, 4),
            "wrong Photo Menu ELF layout")
    require(not execute([tool(llvm, "llvm-nm"), "--undefined-only", path]).strip(), "unresolved Photo Menu symbol")
    for index in range(shcount):
        _, section, _, _, _, size, *_ = unpack("II4QII2Q", data, shdr + index * shstride)
        require(section not in (4, 9) or not size, "unresolved Photo Menu ELF relocation")

    symbols = {parts[2]: int(parts[0], 16)
               for line in execute([tool(llvm, "llvm-nm"), "-n", path]).splitlines()
               if len(parts := line.split()) == 3}
    require({symbol: symbols.get(symbol) for symbol in ("photo_open_hook", "photo_stock_open", "photo_text_shim")}
            == {"photo_open_hook": RX, "photo_stock_open": RX + 0x80, "photo_text_shim": RX + 0x100},
            "fixed hook symbol layout differs")
    require(entry == symbols.get("fx_native_pack_entry"), "Photo Menu native entry changed")
    require({name: symbols.get(name) for name in FLIGHT_SYMBOLS} == FLIGHT_SYMBOLS,
            "fixed flight recorder or heartbeat symbols moved")
    require((symbols.get('photo_burst_power_off'), symbols.get('photo_burst_power_cancel')) ==
            tuple(target for _, target in BURST_POWER_HOOKS), 'burst power hook layout differs')
    spans = SEGMENTS if count == 4 else (*SEGMENTS[:2], (RW, ARENA[1], 6, WRITE))
    records = []
    for index, (begin, end, perm, action) in enumerate(spans):
        typ, flags, offset, va, pa, file_size, memory_size, alignment = unpack("II6Q", data, phdr + index * stride)
        require((typ, flags, va, pa, alignment) == (1, perm, begin, begin, 4096)
                and 0 < memory_size <= end - begin and file_size <= memory_size and offset + file_size <= len(data),
                "Photo Menu ELF segment moved or exceeded its reservation")
        require(end != ARENA[1] or begin + memory_size == end, "resident worker stack coverage differs")
        old = read_original(foundation, begin, memory_size)
        require(old == bytes(memory_size) == read_original(stock, begin, memory_size), "populated Photo Menu reservation")
        records.append(Patch(IMAGE, action, begin, old, data[offset:offset+file_size].ljust(memory_size, b"\0")))
    require(RX <= entry < RX + len(records[0].new)
            and records[0].new[0x80:0x88].hex() == "f60f1df81ef5dd17", "open trampoline machine code differs")
    verify_flight_image(records)
    verify_text_shim(records[0].new, symbols['photo_text_lookup'])
    return records, entry


def verify_flight_image(records):
    code = next(p for p in records if p.kind == IMAGE and p.address == RX)
    state = next(p for p in records if p.kind == IMAGE and p.address == RW)
    require(unpack("II", code.new, 0x344) == (0x46523431, 0x48425431),
            "flight recorder or heartbeat ABI marker differs")
    begin = FLIGHT_SYMBOLS["photo_flight_ring"] - RW
    end = FLIGHT_END - RW
    require(state.flags == WRITE and state.new[begin:end] == bytes(end - begin),
            "flight recorder and heartbeat must start in zeroed writable RAM")


def compile_menu(stock, foundation, released_patches, llvm, output, selection):
    output.mkdir(parents=True)
    generated = output / "photo_layout.S"
    generated.write_text(descriptor_source(stock, foundation, released_patches, selection), encoding="ascii")
    sources = [MENU_SOURCE / name for name in MENU_UNITS]
    sources.extend((generated, ROOT / "src/memory.c"))
    common = ["--target=aarch64-none-elf", "-Oz", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
              "-fno-builtin", "-fno-pic", "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
              "-ffunction-sections", "-fdata-sections", "-mgeneral-regs-only", "-mstrict-align", "-mno-outline-atomics",
              "-DFX_FREESTANDING=1", "-I", ROOT / "include"]
    objects = []

    def run(name, *args):
        return execute([tool(llvm, name), *args])

    for source in sources:
        destination = output / (source.stem + ".o")
        flags = ["-std=c11"] if source.suffix == ".c" else []
        compile_audited_unit(run, (*common, *flags), source, destination)
        objects.append(destination)
    elf = output / "photo_menu.elf"
    execute([tool(llvm, "ld.lld"), "-m", "aarch64elf", "--build-id=none", "-z", "max-page-size=4096",
             "--gc-sections", "-T", MENU_SOURCE / "photo_menu.ld", *objects, "-o", elf])
    records, entry = inspect_menu_elf(elf, llvm, stock, foundation)
    identity = int.from_bytes(bytes.fromhex(sha(elf.read_bytes()))[:8], "little") or 1
    capsule = encode([*records, *hooks_for(stock, selection)], identity, REQUIRED, entry)
    return dict(mask=MASK, value=selection, capsule=capsule)


def component_variants(components, arena=ARENA, reservation="Photo Menu reserved arena"):
    """Read every variant, including inactive masks, to check arena coexistence."""
    unique = {}
    for data in components:
        metadata = decode_component(data, target=TARGET, requires_id=REQUIRED)
        unique[metadata["sha256"]] = (metadata, data)
    checked = {}
    for checksum, (metadata, data) in unique.items():
        for variant in metadata["variants"]:
            value = variant["value"]
            capsule = decode_component(data, target=TARGET, requires_id=REQUIRED, selection=value)["capsule"]
            for patch in decode(capsule)["patches"]:
                require(not (patch.address < arena[1] and patch.address + len(patch.new) > arena[0]),
                        f"{metadata['id']} variant {value:#x} intersects {reservation} at {patch.address:#x}")
        checked[checksum] = dict(id=metadata["id"], variants=len(metadata["variants"]))
    return checked


def section_report(capsule):
    names = {EXEC: "RX", 0: "RO", WRITE: "RW"}
    return [dict(address=hex(p.address), bytes=len(p.new), permissions=names[p.flags], sha256=sha(p.new))
            for p in capsule["patches"] if p.kind == IMAGE]


def audited_image(elf, records, llvm, isolated):
    """Match audited linked load bytes to the sealed component, including zero-fill."""
    data = elf.read_bytes()
    table, = unpack("Q", data, 32)
    stride, count = unpack("HH", data, 54)
    loads = []
    require(stride == 56 and count == len(records), f"{elf}: program header layout differs")
    for index, record in enumerate(records):
        kind, flags, offset, va, pa, file_size, memory_size, alignment = unpack("II6Q", data, table + index * stride)
        permissions = {"RX": 5, "RO": 4, "RW": 6}[record["permissions"]]
        require((kind, flags, va, pa, memory_size, alignment) ==
                (1, permissions, int(record["address"], 16), int(record["address"], 16), record["bytes"], 4096)
                and file_size <= memory_size and offset + file_size <= len(data)
                and sha(data[offset:offset+file_size].ljust(memory_size, b"\0")) == record["sha256"],
                f"{elf}: load segment differs from sealed component")
        loads.append(dict(**record, file_bytes=file_size, zero_fill_bytes=memory_size - file_size))
    simd = audit_simd(lambda name, *args: execute([tool(llvm, name), *args]), elf, isolated)
    return dict(segments=loads, simd_audit=simd)


def verify_plan_preimages(plan, foundation):
    runtime_verified = 0
    bootstrap_roots = []
    pages = {}
    if plan is None:
        return dict(file_preimages=0, approved_bootstrap_roots=[], image_pages=0)
    for patch in decode(plan)["patches"]:
        try:
            original = read_original(foundation, patch.address, len(patch.old))
        except ValueError:
            root = patch.address, patch.flags, patch.old.hex(), patch.new.hex()
            require(patch.kind == HOOK and root in APPROVED_BSS, f"unverified native target {patch.address:#x}")
            bootstrap_roots.append(hex(patch.address))
        else:
            require(original == patch.old, f"foundation native preimage differs at {patch.address:#x}")
            runtime_verified += 1
        if patch.kind == IMAGE:
            for page in range(patch.address // 4096, (patch.address + len(patch.new) - 1) // 4096 + 1):
                pages[page] = pages.get(page, 0) | patch.flags
                require(pages[page] != (EXEC | WRITE), f"native page {page * 4096:#x} would be writable and executable")
    return dict(file_preimages=runtime_verified, approved_bootstrap_roots=bootstrap_roots, image_pages=len(pages))


def verify_menu_projection(plan, foundation, released_patches, expected):
    patches = decode(plan)["patches"] if plan else []
    if not expected:
        require(unpack("Q", projected(foundation, patches, PARENT_POINTER, 8))[0] == PARENT_ORIG,
                "stock Photo Shooting page redirect was enabled")
        require(unpack("H", projected(foundation, patches, PARENT_ORIG, 80), 0x20)[0] == 47,
                "stock Photo Shooting Menu row count differs")
        return dict(rows=47)
    require(unpack("Q", projected(foundation, patches, PARENT_POINTER, 8))[0] == RO,
            "native Photo Shooting page redirect is missing")
    descriptor = projected(foundation, patches, RO, 80)
    count = PARENT_COUNTS[expected]
    source_desc = read_original(foundation, PARENT_ORIG, 80)
    require(descriptor == child_descriptor(source_desc, 0x80001003, 0x43DCF680, PARENT_ROWS, PARENT_EVENTS, count),
            "native Photo Shooting page descriptor differs")
    rows = projected(foundation, patches, PARENT_ROWS, count * ROW_BYTES)
    events = projected(foundation, patches, PARENT_EVENTS, count * 8)
    first_rows, first_events = parent_tables(foundation, released_patches, expected)
    require(rows[:47*ROW_BYTES] == first_rows == projected(foundation, patches, PARENT_ORIG_ROWS, 47 * ROW_BYTES)
            and events[:47*8] == first_events == projected(foundation, patches, PARENT_ORIG_EVENTS, 47 * 8),
            "first 47 effective rows or event pointers differ")
    index = 47
    for bit, state, pointer, vector, desc, child_count in (
            (BURST, BURST_STATE, BURST_PAGE, BURST_VECTOR, BURST_DESC, 2),
            (LUT, LUT_STATE, LUT_PAGE, LUT_VECTOR, LUT_DESC, 23)):
        actual = projected(foundation, patches, state, 96)
        if expected & bit:
            item, title, help_id, _, event_address = PARENT_NEW[bit]
            row = unpack("11I", rows, index * ROW_BYTES)
            require((row[0], row[1], row[2], row[6]) == (title, help_id, title, item)
                    and unpack("Q", events, index * 8)[0] == event_address, "appended parent row differs")
            require(projected(foundation, patches, event_address, 0xBA0) == parent_event_program(foundation, pointer),
                    "appended parent event program differs")
            require(actual == child_state(foundation, pointer, vector, desc)
                    and unpack("H", projected(foundation, patches, desc, 80), 0x20)[0] == child_count,
                    "published child page differs")
            index += 1
        else:
            require(actual == bytes(96), "non-selected child page became visible")
    for site, old, new in TEXT_HOOKS:
        require(projected(foundation, patches, site, 4).hex() == new, "shared text shim is missing")
    site, _, new = OPEN_HOOK
    require(projected(foundation, patches, site, 4).hex() == new,
            "Photo Menu boot-file open hook is missing")
    return dict(rows=count, effective_rows_sha256=sha(first_rows), effective_event_pointers_sha256=sha(first_events),
                pixel_shift_item=hex(unpack("I", first_rows, PIXEL_ROW * ROW_BYTES + 24)[0]),
                pixel_shift_event_pointer=hex(PIXEL_EVENT))


def build(stock_path, foundation_path, photo_path, output, llvm):
    stock, foundation = stock_path.read_bytes(), foundation_path.read_bytes()
    require(output.is_relative_to(BUILD) and output != BUILD and not output.exists(),
            "choose a new output directory under build/")
    preimages(stock, foundation)
    glyphs = audit_chinese_glyphs(stock, (MENU_SOURCE / "photo_ui.c").read_text(encoding="utf-8"))
    photo = bundle_components(released_photo(photo_path))
    plan, _ = compose_components(photo, target=TARGET, requires_id=REQUIRED)
    released_patches = decode(plan)["patches"]
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=".cube-lut-", dir=output.parent) as temp:
        stage = Path(temp) / "package"
        variants = [compile_menu(stock, foundation, released_patches, llvm, stage / "native" / f"{value:03x}", value)
                    for value in VALUES]
        for variant in variants:
            patches = decode(variant["capsule"])["patches"]
            verify_flight_image(patches)
            rx = next(patch.new for patch in patches if patch.kind == IMAGE and patch.address == RX)
            verify_text_shim(rx)
            audited_image(stage / "native" / f"{variant['value']:03x}" / "photo_menu.elf",
                          section_report(decode(variant["capsule"])), llvm, ())
        menu = encode_component(NAME, 1, 0, 140, variants, target=TARGET, requires_id=REQUIRED)
        with redirect_stdout(StringIO()):
            build_rear(stock_path, foundation_path, stage / "rear", llvm)
            rear = bundle_at(stage / "rear/lut_preview.wlm")
            build_jpeg(stock_path, foundation_path, stage / "jpeg", llvm, linked=True, shared_output=True,
                       peer_components=[*photo, *bundle_components(rear), menu])
        jpeg = bundle_at(stage / "jpeg/jpeg_lut.wlm")
        providers = [*bundle_components(jpeg), *bundle_components(rear)]
        component_variants([*photo, *providers])
        package = encode_bundle(rear["frontend"], "cube_lut", [*providers, menu], TARGET)
        delivered = decode_bundle(package, module_id="cube_lut", target=TARGET)
        components = bundle_components(delivered)
        require(components == [*providers, menu], "sealed LUT bundle differs from its inputs")
        compositions = {}
        for label, peers, selection in (("lut_only", [], LUT), ("with_r10_c1", photo, MASK)):
            plan, _ = compose_components([*peers, *components], target=TARGET, requires_id=REQUIRED)
            compositions[label] = {**verify_plan_preimages(plan, foundation),
                                   **verify_menu_projection(plan, foundation, released_patches, selection)}
        report = dict(package=summary(delivered), stock_sha256=STOCK_SHA256, foundation_sha256=FOUNDATION,
                      photo_tools_sha256=RELEASE_SHA, chinese_glyphs=glyphs, compositions=compositions)
        (stage / "cube_lut.wlm").write_bytes(package)
        (stage / "validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        stage.rename(output)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, default=STOCK_PATH)
    parser.add_argument("--foundation", type=Path, default=FOUNDATION_PATH)
    parser.add_argument("--photo-tools", type=Path, default=RELEASE, help="public Photo Tools r10-c1 WLM")
    parser.add_argument("--llvm-bin", type=Path, default=LLVM)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    report = build(args.stock.resolve(), args.foundation.resolve(), args.photo_tools.resolve(),
                   args.output.resolve(), args.llvm_bin.resolve())
    print(json.dumps(report["package"], indent=2))
