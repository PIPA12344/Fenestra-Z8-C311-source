"""Replay the shipped shooting menu against synthetic card directories and settings records."""

from dataclasses import dataclass
from hashlib import sha256
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

import pytest
from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_MODE_ARM, Uc, UcError
from unicorn import arm64_const as arm

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))

from wlm_bundle import decode_bundle
from wlm_native import compose_components
from z8_native_capsule import decode
from z8_native_arena import STOCK_SHA256
from build_z8_photo_menu import FOUNDATION, FOUNDATION_PATH, LLVM, OUTPUT, RELEASE, STOCK_PATH

OUT = Path(os.environ.get('FX_Z8_PHOTO_MENU_BUILD', OUTPUT))
STOCK = Path(os.environ.get('FX_Z8_LUT_STOCK', STOCK_PATH))
BASE = Path(os.environ.get('FX_Z8_LUT_FOUNDATION', FOUNDATION_PATH))
PHOTO_TOOLS = Path(os.environ.get('FX_Z8_LUT_PHOTO_TOOLS', RELEASE))
TARGET = bytes.fromhex(STOCK_SHA256)
REQUIRED = 0x81947c592af69a37
ROM = 0x40160000
INIT, INIT_FILE, INIT_END = 0xa02808000, 0x4b13000, 0x4dc0000
OPEN, TEXT, RATE = 0x402dd4f8, 0x4032e130, 0x40b51000
ROW_BYTES, EVENT_BYTES = 0x2c, 0x20
ACTIVATE = (0x286, 0x28b, 0x29f, 0x2ae)
SCRATCH, END = 0x110000000, 0x110020000
HOST, EXPORTS, ARG, RESULT, PAGE = (SCRATCH + i * 0x1000 for i in range(5))
STACK = END - 16
HEARTBEATS, WATCHDOG = 0x40B73808, 0x40B73908
CRASH_LOG = b'B:\\FENESTRA\\FXCRASH.LOG'
CRASH_TMP = b'B:\\FENESTRA\\FXCRASH.TMP'


def read_main(image, address, size):
    if INIT <= address < INIT + INIT_END - INIT_FILE:
        start = INIT_FILE + address - INIT
    else:
        start = address - ROM
    assert 0 <= start <= len(image) - size, (hex(address), size)
    return image[start:start + size]


def components(path):
    return {row['id']: row['data'] for row in
            decode_bundle(path.read_bytes(), module_id=path.stem, target=TARGET)['components']}


@dataclass
class Composition:
    patches: dict
    entries: list
    selection: int

    def proposed(self, address, size):
        for patch in self.patches.values():
            offset = address - patch.address
            if 0 <= offset <= len(patch.new) - size:
                return patch.new[offset:offset + size]
        raise AssertionError(f'{address:#x} + {size:#x} is outside package records')


def compose(*bundles):
    wire, meta = compose_components(
        [data for bundle in bundles for data in bundle.values()], target=TARGET, requires_id=REQUIRED)
    patches = {p.address: p for p in decode(wire)['patches']} if wire else {}
    return Composition(patches, meta['entries'], meta['selection'])


@pytest.fixture(scope='module')
def inputs():
    if not all(path.is_file() for path in (STOCK, BASE, PHOTO_TOOLS, OUT / 'lut_preview.wlm')):
        pytest.skip('requires the locally built LUT release and its three pinned inputs')
    stock, base = STOCK.read_bytes(), BASE.read_bytes()
    assert sha256(stock).hexdigest() == STOCK_SHA256
    assert sha256(base).hexdigest() == FOUNDATION
    return stock, base


@pytest.fixture(scope='module')
def bundles():
    return {name: components(OUT / f'{name}.wlm') for name in ('photo_tools', 'lut_preview')}


@pytest.fixture(scope='module')
def variants(inputs):
    lut = components(OUT / 'lut_preview.wlm')
    return {'lut': compose(lut), 'both': compose(components(PHOTO_TOOLS), lut)}


class Machine:
    """Execute actual composed AArch64 at its linked addresses with explicit firmware boundaries."""

    def __init__(self, foundation, plan):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.reg_write(arm.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.pages = set()
        self.foundation = foundation
        self.unmapped = None
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.map_firmware_fault)
        self.map(SCRATCH, END + 0x1000 - SCRATCH, zero=True)
        for patch in plan.patches.values():
            self.map(patch.address, len(patch.old))
            if patch.address == 0xa02f2ab80:
                assert self.read(patch.address, len(patch.old)) == bytes(len(patch.old))
            else:
                assert self.read(patch.address, len(patch.old)) == patch.old, hex(patch.address)
            self.uc.mem_write(patch.address, patch.new)

    def map_firmware_fault(self, _uc, access, address, size, _value, _data):
        if (ROM <= address < ROM + len(self.foundation) - 0x1000 or
                INIT <= address < INIT + INIT_END - INIT_FILE):
            self.map(address, size)
            return True
        self.unmapped = access, hex(address), size, hex(self.uc.reg_read(arm.UC_ARM64_REG_PC))
        return False

    def map(self, address, size, *, zero=False):
        for page in range(address & ~0xfff, (address + size + 0xfff) & ~0xfff, 0x1000):
            if page in self.pages:
                continue
            self.uc.mem_map(page, 0x1000)
            self.pages.add(page)
            if not zero and (ROM <= page <= ROM + len(self.foundation) - 0x1000 or
                             INIT <= page <= INIT + INIT_END - INIT_FILE - 0x1000):
                self.uc.mem_write(page, read_main(self.foundation, page, 0x1000))

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def get(self, address, fmt):
        values = struct.unpack('<' + fmt, self.read(address, struct.calcsize('<' + fmt)))
        return values[0] if len(values) == 1 else values

    def put(self, address, fmt, *values):
        self.uc.mem_write(address, struct.pack('<' + fmt, *values))

    def reg(self, number):
        return self.uc.reg_read(getattr(arm, f'UC_ARM64_REG_X{number}'))

    def intercept(self, address, handler):
        self.map(address, 4)
        self.uc.hook_add(UC_HOOK_CODE, lambda *_: handler(), begin=address, end=address)

    def stub(self, address, handler, argc):
        def intercept():
            result = handler(*(self.reg(i) for i in range(argc)))
            self.uc.reg_write(arm.UC_ARM64_REG_X0, result & 0xffffffffffffffff)
            self.uc.reg_write(arm.UC_ARM64_REG_PC, self.reg(30))
        self.intercept(address, intercept)

    def cstring(self, address, limit=96):
        raw = self.read(address, limit)
        assert b'\0' in raw
        return raw.split(b'\0', 1)[0]

    def pack(self, entry, lookups):
        lookup = END + 4

        def resolve(_host, name, version, result):
            assert version == 1
            export = lookups.get(self.cstring(name))
            self.put(result, 'Q', export or 0)
            return 0 if export else 3

        self.stub(lookup, resolve, 4)
        self.put(HOST, '2I4Q', 1, 40, 0, 0, 0, lookup)
        assert self.run(entry, HOST, EXPORTS) == 0
        abi, size, provider_count, _, _, _, context, event, pending = self.get(EXPORTS, '4I5Q')
        assert (abi, size, provider_count) == (1, 56, 0)
        assert 0x40b70000 <= context < 0x40b78000
        return context, event, pending

    def run(self, address, *args, stop=END, stack=STACK):
        self.map(address, 4)
        self.map(stop, 4)
        for i, value in enumerate(args):
            self.uc.reg_write(getattr(arm, f'UC_ARM64_REG_X{i}'), value)
        self.uc.reg_write(arm.UC_ARM64_REG_LR, stop)
        self.uc.reg_write(arm.UC_ARM64_REG_SP, stack)
        self.unmapped = None
        try:
            self.uc.emu_start(address, stop, count=2_000_000)
        except UcError as exc:
            raise AssertionError((hex(address), self.unmapped)) from exc
        assert self.uc.reg_read(arm.UC_ARM64_REG_PC) == stop, hex(address)
        return self.reg(0)


@pytest.fixture(scope='module')
def symbols(inputs):
    nm = shutil.which('llvm-nm') or LLVM / 'llvm-nm.exe'
    elf = OUT / 'native/410/photo_menu.elf'
    rows = subprocess.check_output([str(nm), '-n', str(elf)], text=True, timeout=10)
    return {row[2]: int(row[0], 16) for line in rows.splitlines() if len(row := line.split()) == 3}


def record(generation, name=b'', *, version=1):
    name_size = {1: 32, 2: 64}[version]
    raw = struct.pack(f'<8sHHQBB2x{name_size}s4x', b'FXPHOTO', version, name_size + 32,
                      generation, bool(name), len(name), name)
    return raw + struct.pack('<I', zlib.crc32(raw))


class WorkerFixture:
    """Run the sealed worker against modeled card, task and LUT-provider boundaries."""

    OPEN_RETURN = 0x401cdfa0
    MISSING = 0x80000010
    DIR_END = 0x80000030
    GATE = SCRATCH + 0x2f00

    def __init__(self, foundation, plan, symbols, files=None, *, path_capable=True):
        self.m = m = Machine(foundation, plan)
        self.symbols = symbols
        self.tasks = []
        self.resumed = []
        self.opened = []
        self.closed = []
        self.files = {} if files is None else files.copy()
        self.fds = {}
        self.next_fd = 100
        self.drive = ord('A')
        self.dir_entries = []
        self.subdirectories = {}
        self.directory_reads = []
        self.directory_opens = []
        self.directory = None
        self.heap_available = True
        self.heap_allocations = []
        self.heap_frees = []
        self.heap = SCRATCH + 0xa000
        self.path_capable = path_capable
        self.jpeg_missing_path = None
        self.sequence = []
        self.phase = 0
        self.serial = 0
        self.permit = None
        self.ticket = 0
        self.current = b''
        self.rear_ready = b''
        self.clear_busy = 0
        self.io_calls = []
        self.card_trace = []
        self.short_crash_write = False
        self.before_clean = None
        self.stock_inputs = []
        self.stock_return = None
        self.close_error = 0
        self.log_rename_error = 0
        self.directory_error = 0
        self.active_exposures = 4
        self.task_turn_sleeps = None
        self.rear_steps = []
        self.rear_step_calls = 0

        def create(task):
            attrs, reserved, context, entry, priority, reserved2, size, stack, cpu, affinity = m.get(task, 'IIQQIIQQII')
            assert (attrs, reserved, priority, reserved2, size, stack, cpu, affinity) == (
                0, 0, 28, 0, 32768, 0x40b78000, 1, 0xffffffff)
            assert 0x40b60100 <= entry < 0x40b6c000
            self.tasks.append((entry, context))
            return 17

        def resume(task):
            self.resumed.append(task)
            return 0

        def delay(ticks):
            if self.task_turn_sleeps is not None:
                self.task_turn_sleeps.append(ticks)
                m.uc.reg_write(arm.UC_ARM64_REG_LR, END)
            return 0

        def open_continuation(path, flags, mode):
            # The packaged trampoline executes the real stock prologue before the stub.
            sp = m.uc.reg_read(arm.UC_ARM64_REG_SP)
            assert m.get(sp, 'Q') == m.reg(22)
            self.stock_inputs.append((tuple(m.reg(i) for i in range(19)),
                                      m.uc.reg_read(arm.UC_ARM64_REG_NZCV), sp, m.reg(30)))
            m.uc.reg_write(arm.UC_ARM64_REG_SP, sp + 0x30)
            result = self.open(path, flags, mode)
            return result if self.stock_return is None else self.stock_return

        m.stub(0x40185064, create, 1)
        m.stub(0x401855e0, resume, 1)
        m.stub(0x40187b00, delay, 1)
        m.stub(0x401d450c, lambda: self.drive, 0)
        m.stub(OPEN + 4, open_continuation, 3)
        m.stub(0x402dd558, self.close, 1)
        m.stub(0x401d3ba0, self.read, 4)
        m.stub(0x401b7668, self.clean, 4)
        m.stub(0x402dd78c, self.write, 5)
        m.stub(0x402dd9fc, self.rename, 2)
        m.stub(0x402dda4c, self.remove, 1)
        m.stub(0x402ddc6c, self.dir_open, 2)
        m.stub(0x402ddd1c, self.dir_next, 2)
        m.stub(0x402ddd6c, self.dir_close, 1)
        m.stub(0x40d2c408, self.allocate, 2)
        m.stub(0x40d2c448, self.free, 1)
        jpeg, rear = END + 8, END + 12
        m.stub(jpeg, self.jpeg, 2)
        m.stub(rear, self.rear, 2)
        self.context, self.event_at, self.pending = m.pack(symbols['fx_native_pack_entry'], {
            b'camera.z8.jpeg.lut.worker': jpeg, b'camera.z8.rear.lut.worker': rear,
            b'camera.z8.rear.lut.io': self.GATE})

    def event(self, event):
        if event in (4, 6):
            self.reopen_io()
        return self.m.run(self.event_at, self.context, event)

    def close_io(self):
        self.m.put(self.GATE, 'Q', self.m.get(self.GATE, 'Q') & ~1 | 0x04000000)

    def reopen_io(self):
        self.m.put(self.GATE, 'Q', ((self.m.get(self.GATE, 'Q') >> 32) + 1) << 32 | 1)

    def admission_edge(self, op):
        self.io_calls.append(op)
        assert self.m.get(self.GATE, 'Q') & 0x7ffffffe, op

    def open(self, pointer, flags, mode):
        path = self.m.cstring(pointer)
        if not path.lower().endswith((b'boot-z8.flag', b'boot-z8.flag.extra')):
            self.admission_edge('open')
        self.opened.append((path, flags, mode))
        if flags == 0 and mode == 0 and path in self.files:
            pass
        elif flags == 0x101 and mode == 0 and path not in self.files:
            self.files[path] = b''
        else:
            return self.MISSING
        fd = self.next_fd
        self.next_fd += 1
        self.fds[fd] = path, 0
        return fd

    def close(self, fd):
        if self.close_error:
            return self.close_error
        path = self.fds.pop(fd)[0]
        self.closed.append(path)
        self.card_trace.append(('close', path))
        return 0

    def read(self, fd, buffer, size, output):
        self.admission_edge('read')
        assert not buffer % 64
        path, offset = self.fds[fd]
        data = self.files[path][offset:offset + size]
        self.fds[fd] = path, offset + len(data)
        if data:
            self.m.uc.mem_write(buffer, data)
        self.m.put(output, 'I', len(data))
        return 0

    def clean(self, ctx, buffer, size, op):
        assert not ctx and not ((buffer | size) & 63) and op == 4
        self.card_trace.append(('clean', buffer, size))
        if self.before_clean:
            self.before_clean()
        return 0

    def write(self, fd, buffer, size, output, mode):
        self.admission_edge('write')
        path, offset = self.fds[fd]
        assert not mode and not offset and not buffer % 64
        if path.startswith(CRASH_TMP[:-1]):
            assert 0 < size <= 4096
            assert next(call for call in reversed(self.card_trace) if call[0] in ('clean', 'write')) == (
                'clean', buffer, (size + 63) & ~63)
            assert not self.m.get(self.GATE, 'Q') & 0x04000000
            assert self.m.get(HEARTBEATS + 7 * 32, '4I')[::3] == (12, 4)
            self.card_trace.append(('write', path, size))
            if self.short_crash_write:
                size -= 1
        else:
            assert size == 96
        self.files[path] = self.m.read(buffer, size)
        self.m.put(output, 'I', size)
        return 0

    def rename(self, source, dest):
        self.admission_edge('rename')
        src, dst = (self.m.cstring(at) for at in (source, dest))
        assert src[:12] == dst[:12] and dst not in self.files
        self.card_trace.append(('rename', src, dst))
        if src.startswith(CRASH_TMP[:-1]) and dst == CRASH_LOG and self.log_rename_error:
            return self.log_rename_error
        self.files[dst] = self.files.pop(src)
        return 0

    def remove(self, pointer):
        self.admission_edge('remove')
        self.files.pop(self.m.cstring(pointer))
        return 0

    def startup_open(self, name=None, flags=0, mode=0, scoped=True, call_site=False):
        name = name or bytes([self.drive]) + b':\\FENESTRA\\boot-z8.flag'
        self.m.uc.mem_write(ARG, name + b'\0')
        entry = OPEN
        if call_site:
            assert scoped
            self.m.uc.reg_write(arm.UC_ARM64_REG_X9, OPEN)
            entry = self.OPEN_RETURN - 4
        return self.m.run(entry, ARG, flags, mode, stop=self.OPEN_RETURN if scoped else END) & 0xffffffff

    def dir_open(self, name, result):
        self.admission_edge('dir_open')
        assert self.directory is None
        path = self.m.cstring(name)
        root = bytes([self.drive]) + b':\\FENESTRA'
        assert path == root or path.startswith(root + b'\\LUTS')
        entries = self.dir_entries if path == root else self.subdirectories.get(path[len(root) + 1:])
        self.directory_opens.append(path)
        if entries is None:
            return self.MISSING
        self.directory = iter(entries)
        self.m.put(result, 'i', 70)
        return 0

    def dir_next(self, fd, raw):
        self.admission_edge('dir_next')
        assert not raw % 64
        assert fd == 70 and self.directory is not None
        self.directory_reads.append(self.directory_opens[-1])
        if self.directory_error:
            return self.directory_error
        item = next(self.directory, None)
        if item is None:
            return self.DIR_END
        name, size, attrs = item
        data = bytearray(0x220)
        if isinstance(name, bytes):
            data[:len(name)] = name  # 13-byte short-name fallback from an empty LFN.
        else:
            encoded = name.encode('utf-16-le')
            data[14:14 + len(encoded)] = encoded
        struct.pack_into('<B', data, 0x20e, attrs)
        struct.pack_into('<Q', data, 0x218, size)
        self.m.uc.mem_write(raw, bytes(data))
        return 0

    def dir_close(self, fd):
        assert fd == 70 and self.directory is not None
        self.directory = None
        return 0

    def allocate(self, size, flags):
        assert (size, flags) == (16 * 1024, 0)
        if not self.heap_available:
            return 0
        assert len(self.heap_allocations) == len(self.heap_frees)
        self.heap_allocations.append(size)
        return self.heap

    def free(self, address):
        assert address == self.heap and len(self.heap_allocations) == len(self.heap_frees) + 1
        self.heap_frees.append(address)
        return 0

    def jpeg(self, op, data):
        m = self.m
        if op == 9:
            return 0x100 if self.path_capable else 1
        if op in (6, 10):
            if op == 6:
                enabled, name = m.get(data, 'I32s')
                assert enabled == 1
                output = data + 40
            else:
                assert self.path_capable
                name = m.get(data, '64s')
                output = data + 64
            self.permit, self.ticket = m.get(output, 'QI')
            assert m.get(output + 16, 'B') == self.drive
            assert m.get(output + 24, 'QI') == (self.GATE, m.get(self.GATE, 'Q') >> 32)
            assert m.get(self.permit, 'I') == self.ticket - 1
            self.current = name.rstrip(b'\0')
            self.sequence.append(('prepare', self.current))
            self.serial += 1
            self.phase = 1
            return 0
        if op == 3:
            if not self.phase:
                return 0
            if self.phase < 4:
                self.phase += 1
            return 2 if self.phase == 4 else 1
        if op == 7:
            if self.current == self.jpeg_missing_path:
                m.put(data, '8I', 1, self.serial, 5, 12, 0, 0, 0, 0)  # FX_EMPTY
                return 0
            m.put(data, '8I', 1, self.serial, self.phase, 0, 512, self.phase, 4, 0)
            return 0
        if op == 8:
            m.put(data, '6I', 3, 2, 45, 1, 3000, self.active_exposures)
            return 0
        if op == 2:
            self.phase = 0
            self.sequence.append(('stop', self.current))
            return 0
        raise AssertionError((op, data))

    def rear(self, op, data):
        if op in (4, 6, 8):
            assert not data
            self.sequence.append(('retire', self.current))
            if self.clear_busy:
                self.clear_busy -= 1
                return 5
            self.rear_ready = b''
            return self.jpeg(2, 0) if op == 4 else 0
        if op == 5:
            assert self.phase == 4
            assert self.m.get(data, 'QI') == (self.permit, self.ticket)
            assert self.m.get(self.permit, 'I') == self.ticket - 1
            self.rear_ready = self.current
            self.sequence.append(('ready', self.rear_ready))
            return 0
        if op == 7:
            assert not data
            return 1
        if op == 9:
            self.m.put(data, '6I', 3, 1234, 3, 1, 4, 5)
            return 0
        assert op == 2 and data == 0
        self.rear_step_calls += 1
        return self.rear_steps.pop(0) if self.rear_steps else 0

    def tick(self):
        self.m.run(self.symbols['photo_worker_slice'])

    def task_turn(self):
        entry, context = self.tasks[0]
        self.task_turn_sleeps = []
        self.m.run(entry, context, stack=0x40b80000)
        sleeps = self.task_turn_sleeps
        self.task_turn_sleeps = None
        return sleeps

    def select(self, name, epoch=None):
        if epoch is None:
            epoch = self.m.run(self.symbols['photo_gui_epoch'])
        self.m.put(ARG, '64s', name)
        return self.m.run(self.symbols['photo_lut_select'], ARG, epoch)

    def status(self):
        self.m.uc.reg_write(arm.UC_ARM64_REG_X8, RESULT)
        self.m.run(self.symbols['photo_lut_status'])
        return self.m.get(RESULT, '5I')

    def page(self):
        if not self.m.run(self.symbols['photo_lut_page'], RESULT):
            return None
        count, previous, next_, serial, epoch = self.m.get(RESULT, '5I')
        assert count <= 20
        names = [self.m.get(RESULT + 20 + i * 32, '32s').rstrip(b'\0') for i in range(count)]
        level, focus, category = self.m.get(RESULT + 20 + 20 * 32, 'II32s')
        return names, previous, next_, serial, epoch, level, focus, category.rstrip(b'\0')

    def enter_folder(self, name, row):
        self.m.put(ARG, '32s', name)
        self.m.run(self.symbols['photo_lut_enter_folder'], ARG, row)

    def until(self, condition, limit=80):
        for _ in range(limit):
            if condition():
                return
            self.tick()
        raise AssertionError((self.status(), self.sequence, self.opened))


def test_category_paging_caches_393_names_and_restores_root_focus(inputs, variants, symbols):
    f = WorkerFixture(inputs[1], variants['lut'], symbols)
    categories = [f'cat{i:02}' for i in range(22)]
    f.subdirectories[b'LUTS'] = [(name, 0, 16) for name in reversed(categories)]
    f.subdirectories[b'LUTS'] += [('.', 0, 16), ('..', 0, 16), ('a' * 27, 0, 16), ('not-dir', 0, 0),
                                  ('label', 0, 0x18), ('颜色', 0, 16), ('CAt01', 0, 16)]
    f.dir_entries = [('empty.cube', 0, 0), ('label.cube', 100, 8)]
    names = [f'File{i:03}.cube' for i in range(393)]
    f.subdirectories[b'LUTS\\cat21'] = [(name, 100, 0) for name in reversed(names)]
    assert f.event(6) == 0 and f.startup_open() == f.MISSING
    f.m.run(symbols['photo_lut_request_page'], 0)
    f.until(f.page)
    assert f.page()[:3] == ([b'cat00', b'CAt01', *(name.encode() for name in categories[2:20])], 0, 1)
    f.m.run(symbols['photo_lut_request_page'], 1)
    f.until(lambda: f.page() and f.page()[1])
    assert f.page()[:3] == ([b'cat20', b'cat21'], 1, 0)
    f.enter_folder(b'cat21', 2)
    for _ in range(100):
        if f.page() and f.page()[5] == 1:
            break
        reads = len(f.directory_reads)
        f.tick()
        assert len(f.directory_reads) - reads <= 8
    else:
        pytest.fail('cached folder did not publish')
    reads = len(f.directory_reads)
    for page_no in range(20):
        assert f.page()[:3] == ([name.encode() for name in names[page_no * 20:(page_no + 1) * 20]],
                                page_no != 0, page_no != 19)
        if page_no < 19:
            prior = f.page()[3]
            f.m.run(symbols['photo_lut_request_page'], 1)
            f.tick()
            assert f.page() and f.page()[3] != prior
    assert len(f.page()[0]) == 13 and len(f.directory_reads) == reads
    f.m.run(symbols['photo_lut_leave_folder'], 2)
    f.until(lambda: f.page() and f.page()[5] == 0)
    assert f.page()[:3] == ([b'cat20', b'cat21'], 1, 0)
    assert f.page()[6] == 2 and f.heap_allocations == [16384] and len(f.heap_frees) == 1
    f.enter_folder(b'cat21', 2)
    f.until(lambda: f.page() and f.page()[5] == 1, limit=100)
    f.close_io()
    f.until(lambda: len(f.heap_frees) == 2)
    assert not f.page() and not f.directory


@pytest.mark.parametrize('missing', [False, True])
def test_boot_restore_prefers_newer_v2_relative_path(inputs, variants, symbols, missing):
    root = b'A:\\FENESTRA\\'
    path = b'LUTS\\10-Creative-Vintage\\warm.cube'
    files = {root + b'FXPHOTO0.CFG': record(9, b'warm.cube'), root + b'warm.cube': b'flat',
             root + b'FXPHOTO1.CFG': record(10, path, version=2)}
    if not missing:
        files[root + path] = b'nested'
    f = WorkerFixture(inputs[1], variants['both'], symbols, files)
    if missing:
        f.jpeg_missing_path = path
    assert f.event(6) == 0 and f.startup_open() == f.MISSING
    f.until(lambda: f.status()[1] == (5 if missing else 4))
    assert not any(entry == ('prepare', b'warm.cube') for entry in f.sequence)
    assert not any(entry[0] in ('write', 'remove', 'rename') for entry in f.card_trace)
    assert f.files == files
    if not missing:
        assert f.rear_ready == path


def test_child_events_preserve_native_menu_shutter_and_dial(inputs, variants, symbols):
    stock, _ = inputs
    plan = variants['both']
    stock_events = dict((e, (action, guard, target)) for e, action, guard, target in
                        struct.iter_unpack('<4Q', read_main(stock, 0x43bff8e8, 15 * EVENT_BYTES)))
    for desc in (0x40b6e200, 0x40b6e260):
        event_array, = struct.unpack('<Q', plan.proposed(desc + 0x18, 8))
        program, = struct.unpack('<Q', plan.proposed(event_array, 8))
        actual = dict((e, (action, guard, target)) for e, action, guard, target in
                      struct.iter_unpack('<4Q', plan.proposed(program, 15 * EVENT_BYTES)))
        assert actual.keys() == stock_events.keys()
        for e, (action, guard, target) in stock_events.items():
            new_action, new_guard, new_target = actual[e]
            if e in ACTIVATE:
                assert new_guard == guard
                assert new_target == 0x68c and 0x40b60100 <= new_action < 0x40b6c000
            elif e in (0x285, 0x2a0, 0x306):
                assert new_guard == guard and new_target == 0x19
                assert new_action == (symbols['photo_lut_back_action'] if desc == 0x40b6e260 else action)
            else:
                assert (new_action, new_guard, new_target) == (action, guard, target)


def native_child_dispatch(m):
    # Run the original dispatcher including its notification, native request
    # constructor, indirect calls and target reload. Diagnostic logging, the
    # touch condition's runtime state and selection feedback are synthetic.
    m.put(PAGE, 'I', (0x5d6 << 16) | 2)
    m.put(PAGE + 0x50, 'Q', 0x40b6e260)
    for address in (0xa0322818c, 0xa032b50d8):
        m.map(address, 8, zero=True)
    m.put(0xa032b50d8, 'Q', 0xa028f4cc0)  # Group-2 page root for stock request helpers.
    m.stub(0x4056992c, lambda *_: 0, 3)
    m.stub(0x403b3c68, lambda *_: 0, 3)
    m.stub(0x403ec398, lambda *_: 0, 1)
    stock_actions, touch_inputs = [], []
    touch_allowed = True

    def touch_condition(notification):
        touch_inputs.append((notification, m.reg(20)))
        assert (notification, m.reg(20)) == (2, PAGE)
        return int(touch_allowed)

    m.stub(0x403b1c64, touch_condition, 1)
    m.intercept(0x403b1ba4, lambda: stock_actions.append(m.get(m.reg(0), 'QII')))

    def dispatch(event, *, allow_touch=True):
        nonlocal touch_allowed
        touch_allowed = allow_touch
        return m.run(0x40508bd4, PAGE, event, ARG)

    return dispatch, stock_actions, touch_inputs


def test_original_child_dispatch_back_separates_condition_and_action_abi(inputs, variants):
    m = Machine(inputs[1], variants['both'])
    dispatch, stock_actions, touch_inputs = native_child_dispatch(m)
    assert dispatch(0x285) == 0x19  # The deployed guard tried to read at 2 + 8 here.
    assert dispatch(0x2a0, allow_touch=False) == 0x68c
    assert dispatch(0x2a0) == 0x19
    assert dispatch(0x306) == 0x19
    assert touch_inputs == [(2, PAGE)] * 2
    assert stock_actions == [(PAGE, 0x285, 0x19), (PAGE, 0x2a0, 0x19)]


def test_folder_rows_survive_native_text_setters_loading_and_full_pages(inputs, variants, symbols):
    f = WorkerFixture(inputs[1], variants['both'], symbols)
    m = f.m
    dispatch, _, _ = native_child_dispatch(m)
    assert f.event(6) == 0 and f.startup_open() == f.MISSING
    small, large = '03-Classic-Negative', '10-Creative-Vintage'
    small_names = [f'Small{i:03}.cube' for i in range(6)]
    large_names = [f'File{i:03}.cube' for i in range(393)]
    f.subdirectories[b'LUTS'] = [(name, 0, 16) for name in (small, large)]
    for folder, names in ((small, small_names), (large, large_names)):
        f.subdirectories[('LUTS\\' + folder).encode()] = [(name, 1024, 0) for name in names]

    text = 0xa032bd4a8 + 24 * 0xfc
    fonts = 0xa032bf0c8
    timer = 0xa032b4ba0
    flags = 0xa032aa850 + 0x4800 - 0x2921
    for address, size in ((text, 24 * 24), (fonts, 16 * 0x220), (timer, 24), (flags, 23),
                          (0xa032c55f8, 1)):  # Stock text-preset runtime flag.
        m.map(address, size, zero=True)
    buffers = SCRATCH + 0x9000
    for i in range(24):
        m.put(text + 24 * i + 8, 'Q', buffers + 128 * i)

    widget_base, stride = SCRATCH + 0x5000, 0x140
    file_font, wide_font = 0x207, 0x21f
    m.stub(0x4032db54, lambda: 8, 0)
    m.stub(0x4032e19c, lambda language, title: 2 if (language, title) == (8, 0x8ae) else 0, 2)

    def find(item, frame):
        assert frame == PAGE + 0x18
        return widget_base + (23 if item == 0x1405 else item - 0x4800) * stride

    def disable(page, row, disabled):
        assert page == PAGE and row < 23
        m.put(flags + row, 'B', disabled)
        return 0

    # Execute stock SET_WIDE, SET_TEXT and BIND_TEXT. Leave the display and
    # timer boundaries synthetic, including the locale font-family lookup.
    m.stub(0x4039e810, find, 2)
    m.stub(0x403b1ed0, disable, 3)
    m.stub(0x403afed4, lambda _: 23, 1)
    for address in (0x403d7084, 0x4050a6f0, 0x403f62b4, 0x403f621c, 0x403aff78,
                    0x403ae874, 0x403aeb38, 0x403aec24, 0x4031e2a8, 0x403f0db8,
                    0x4039ecc4, 0x4039ed6c, 0x4039e848, 0x403efed8, 0x402e76d4):
        m.stub(address, lambda: 0, 0)

    def bound_text(row, value, wide):
        node = widget_base + row * stride
        slot, mode = m.get(node + 8, 'II')
        assert (slot, mode) == (0xfc + row, 4 if wide else 3)
        length, pointer, font = m.get(text + row * 24, 'I4xQI')
        assert pointer == buffers + 128 * row
        assert font == (wide_font if wide else file_font)
        assert m.get(fonts + 16 * font, 'B') == (2 if wide else 0x13)
        encoding = 'utf-16-le' if wide else 'ascii'
        encoded = (value + '\0').encode(encoding)
        assert length == len(value) and m.read(pointer, len(encoded)) == encoded
        return pointer

    def check(back, names, previous=False, next_=False):
        expected = {0: (back, True)}
        expected.update({i: (name, False) for i, name in enumerate(names, 1)})
        if previous:
            expected[21] = ('上一页', True)
        if next_:
            expected[22] = ('下一页', True)
        assert list(m.read(flags, 23)) == [int(i not in expected) for i in range(23)]
        pointers = [bound_text(row, value, wide) for row, (value, wide) in expected.items()]
        assert len(set(pointers)) == len(pointers)

    def refresh():
        m.run(symbols['photo_lut_refresh'], PAGE)

    def enter(row):
        m.put(PAGE + 0x32, 'H', row)
        assert dispatch(0x286) == 0x68c

    def leave():
        assert dispatch(0x285) == 0x68c
        f.until(lambda: f.page() and f.page()[5] == 0)
        refresh()
        bound_text(0, '无LUT', True)

    m.run(symbols['photo_lut_entry'], PAGE)
    check('无LUT', [])
    f.until(f.page)
    refresh()
    bound_text(0, '无LUT', True)

    enter(1)
    f.tick()  # Six entries and EOF fit in one worker turn.
    assert f.page() and f.page()[5] == 1
    refresh()
    check('返回上一级', small_names)
    leave()

    enter(2)
    f.tick()
    assert not f.page()
    refresh()
    check('返回上一级', [])
    for _ in range(3):
        f.tick()
        assert not f.page()
        refresh()
        check('返回上一级', [])
    f.until(f.page)
    refresh()
    check('返回上一级', large_names[:20], next_=True)
    m.put(PAGE + 0x32, 'H', 22)
    assert dispatch(0x28b) == 0x68c and not f.page()
    refresh()  # Keep live bindings while Next is pending.
    check('返回上一级', large_names[:20], next_=True)
    f.tick()
    refresh()
    check('返回上一级', large_names[20:40], previous=True, next_=True)
    leave()
    assert m.get(PAGE + 0x32, 'H') == 2

    enter(2)
    refresh()
    check('返回上一级', [])
    assert dispatch(0x28b) == 0x68c  # The loading view's only row can cancel the scan.
    f.until(lambda: f.page() and f.page()[5] == 0)
    refresh()
    bound_text(0, '无LUT', True)

    enter(1)
    f.tick()
    refresh()
    check('返回上一级', small_names)
    m.run(symbols['photo_lut_exit'], PAGE)
    assert m.read(buffers, 24 * 128) == bytes(24 * 128)
