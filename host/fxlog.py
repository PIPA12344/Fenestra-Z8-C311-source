"""Build the Z8 fxlog directory component and decode native log snapshots."""
from collections.abc import Callable
from dataclasses import dataclass
import hashlib
import re
import struct

from wlm_native import encode_component
from z8_native_arena import ARENA_BEGIN, ARENA_END
from z8_native_capsule import IMAGE, WRITE, Patch, encode

FXLOG_DIR = 0x40bcf000
PAGE_BYTES = 4096
ABI = 1
TICKS_PER_MS = 24000
DIRECTORY_MAGIC = b'FXLOGDIR'
RING_MAGIC = b'FXRING01'
HEADER_BYTES = SLOT_BYTES = 64
SLOT_COUNT = 32
MAX_READ = 1008
LEVELS = 'EWIDT'

_DIRECTORY = struct.Struct('<8s6IQ24x')
_SLOT = struct.Struct('<II16sQQQI12x')
_RING = struct.Struct('<8sIIQII32x')
_ENTRY = struct.Struct('<IIQ3I4B')
_HEARTBEAT = struct.Struct('<4I')
_U32_MASK = (1 << 32) - 1
_MAX_FMT_BYTES = PAGE_BYTES
_FORMAT = re.compile(r'%(?:%|(0?)([0-9]*)([udxXc]))')


def directory_page() -> bytes:
    """Return the canonical initial page, with an empty slot table."""
    header = _DIRECTORY.pack(DIRECTORY_MAGIC, ABI, HEADER_BYTES, SLOT_BYTES,
                             SLOT_COUNT, TICKS_PER_MS, 0, 0)
    return header.ljust(PAGE_BYTES, b'\0')


def shared_component(target: bytes, requires_id: int) -> bytes:
    """Encode the same data-only component for every Z8 WLM that uses fxlog."""
    page = directory_page()
    package_id = int.from_bytes(hashlib.sha256(page).digest()[:8], 'little') or 1
    capsule = encode([Patch(IMAGE, WRITE, FXLOG_DIR, bytes(PAGE_BYTES), page)],
                     package_id, requires_id)
    return encode_component('shared.fxlog', version=1, selector_bit=0, rank=95,
                            variants=[dict(mask=0, value=0, capsule=capsule)],
                            target=target, requires_id=requires_id)


class FxlogDirectoryError(ValueError):
    """The camera has no directory or a different directory ABI."""


@dataclass(frozen=True)
class FxlogPlugin:
    slot: int
    id: str
    ring: int
    heartbeat: int
    build: int
    heartbeat_count: int


@dataclass(frozen=True)
class FxlogLine:
    t_ms: int
    core: int
    level: int
    plugin_id: str
    text: str

    def __str__(self):
        return f'{self.t_ms:>10} ms  核 {self.core:<2} {LEVELS[self.level]} {self.plugin_id}  {self.text}'


@dataclass(frozen=True)
class FxlogHeartbeat:
    plugin_id: str
    slot: int
    stage: int
    count: int
    last_ms: int
    core: int
    age_ms: int | None


@dataclass(frozen=True)
class FxlogSnapshot:
    latest_ms: int
    plugins: tuple[FxlogPlugin, ...]
    logs: tuple[FxlogLine, ...]
    heartbeats: tuple[FxlogHeartbeat, ...]
    lost: int  # per poll: overwritten, unpublished or malformed entries
    dropped: int  # current sum of writer-maintained ring counters


def _time_delta(latest_ms, timestamp):
    """Signed low-32-bit difference, including a timestamp just ahead of latest_ms."""
    delta = ((latest_ms & _U32_MASK) - timestamp) & _U32_MASK
    return delta - (1 << 32) if delta >= (1 << 31) else delta


def _heartbeat_age(latest_ms, timestamp):
    # Before the first wrap both timestamps are absolute. Afterward we cannot
    # distinguish a nearly full-wrap age from a heartbeat sampled just ahead
    # of latest_ms, so interpret differences within half a wrap.
    delta = latest_ms - timestamp if latest_ms <= _U32_MASK else _time_delta(latest_ms, timestamp)
    return max(0, delta)


def _in_arena(address, size):
    return ARENA_BEGIN <= address and address + size <= ARENA_END


def _format_message(fmt, arguments):
    parts = []
    end = used = 0
    while (start := fmt.find('%', end)) != -1:
        parts.append(fmt[end:start])
        match = _FORMAT.match(fmt, start)
        if not match:
            raise ValueError('格式符无效')
        end = match.end()
        if match.group(0) == '%%':
            parts.append('%')
            continue
        zero, width, conversion = match.groups()
        width = int(width or 0)
        if used == len(arguments) or width > _MAX_FMT_BYTES:
            raise ValueError('参数数量或字段宽度无效')
        value = arguments[used]
        used += 1
        if conversion == 'c':
            parts.append(chr(value & 0xff).rjust(width, '0' if zero else ' '))
            continue
        if conversion == 'd' and value & (1 << 31):
            value -= 1 << 32
        spec = ('0' if zero else '') + str(width) + ('d' if conversion == 'u' else conversion)
        parts.append(format(value, spec))
    if used != len(arguments):
        raise ValueError('参数数量无效')
    parts.append(fmt[end:])
    return ''.join(parts)


class FxlogReader:
    """Keep one incremental cursor and format-string cache per registered ring."""

    def __init__(self, read_fn: Callable[[int, int], bytes]):
        self.read_fn = read_fn
        self._heads = {}
        self._formats = {}

    def _read(self, address, count):
        data = self.read_fn(address, count)
        if len(data) != count:
            raise OSError(f'内存读取不完整（0x{address:x}，{len(data)}/{count} 字节）')
        return data

    def _directory(self):
        # Fit whole slots after the header and avoid an extra header-only read.
        first_slots = (MAX_READ - HEADER_BYTES) // SLOT_BYTES
        initial = self._read(FXLOG_DIR, HEADER_BYTES + first_slots * SLOT_BYTES)
        expectations = (DIRECTORY_MAGIC, ABI, HEADER_BYTES, SLOT_BYTES, SLOT_COUNT, TICKS_PER_MS, 0)
        *metadata, latest_ms = _DIRECTORY.unpack_from(initial)
        if tuple(metadata) != expectations:
            raise FxlogDirectoryError('未发现有效的 fxlog 目录。装入带 shared.fxlog 的 WLM 后需要冷启动相机。')
        table = bytearray(initial[HEADER_BYTES:])
        while len(table) < SLOT_COUNT * SLOT_BYTES:
            count = min(MAX_READ // SLOT_BYTES * SLOT_BYTES, SLOT_COUNT * SLOT_BYTES - len(table))
            table.extend(self._read(FXLOG_DIR + HEADER_BYTES + len(table), count))

        plugins = []
        for index, row in enumerate(_SLOT.iter_unpack(table)):
            state, abi, name, ring, heartbeat, build, count = row
            if state != 2 or abi != ABI:
                continue
            try:
                name = name.rstrip(b'\0').decode('ascii')
            except UnicodeError:
                continue
            if name and name.isprintable():
                plugins.append(FxlogPlugin(index, name, ring, heartbeat, build, count))
        return latest_ms, tuple(plugins)

    def _format_string(self, address, formats):
        if address in formats:
            return formats[address]
        if not _in_arena(address, 1):
            raise ValueError('格式地址超出原生 arena')
        result = bytearray()
        while len(result) < _MAX_FMT_BYTES and address + len(result) < ARENA_END:
            start = address + len(result)
            count = min(128, PAGE_BYTES - start % PAGE_BYTES, _MAX_FMT_BYTES - len(result), ARENA_END - start)
            block = self._read(start, count)
            before, separator, _ = block.partition(b'\0')
            result.extend(before)
            if separator:
                fmt = result.decode('utf-8', errors='replace')
                formats[address] = fmt
                return fmt
        raise ValueError('格式串没有结束符')

    def _ring_lines(self, plugin, capacity, first, head, formats):
        cursor = first
        lines = []
        lost = 0
        while cursor < head:
            offset = cursor & (capacity - 1)
            count = min(head - cursor, capacity - offset, MAX_READ // _ENTRY.size)
            address = plugin.ring + HEADER_BYTES + offset * _ENTRY.size
            entries = self._read(address, count * _ENTRY.size)
            for seq, t_ms, fmt, a0, a1, a2, level, core, nargs, flags in _ENTRY.iter_unpack(entries):
                expected = (cursor + 1) & _U32_MASK
                cursor += 1
                # Zero is the in-progress marker even when the seq field wraps.
                if not seq or seq != expected or level >= len(LEVELS) or nargs > 3 or flags:
                    lost += 1
                    continue
                try:
                    text = _format_message(self._format_string(fmt, formats), (a0, a1, a2)[:nargs])
                except ValueError as exc:
                    text = f'[{exc} @ 0x{fmt:x}]'
                lines.append(FxlogLine(t_ms, core, level, plugin.id, text))
        return lines, lost

    def _heartbeats(self, plugin, latest_ms):
        count = plugin.heartbeat_count
        if not count:
            return []
        if plugin.heartbeat % 4 or not _in_arena(plugin.heartbeat, count * _HEARTBEAT.size):
            raise ValueError(f'{plugin.id} 的 fxlog 心跳表无效')
        rows = []
        for first in range(0, count, MAX_READ // _HEARTBEAT.size):
            slots = min(count - first, MAX_READ // _HEARTBEAT.size)
            data = self._read(plugin.heartbeat + first * _HEARTBEAT.size, slots * _HEARTBEAT.size)
            for slot, (stage, beats, last_ms, core) in enumerate(_HEARTBEAT.iter_unpack(data), first):
                age = _heartbeat_age(latest_ms, last_ms) if beats else None
                rows.append(FxlogHeartbeat(plugin.id, slot, stage, beats, last_ms, core, age))
        return rows

    def poll(self, plugin_id: str | None = None) -> FxlogSnapshot:
        """Read newly reserved entries and current heartbeats, optionally filtering by ID."""
        try:
            latest_ms, plugins = self._directory()
        except FxlogDirectoryError:
            self._heads.clear()
            self._formats.clear()
            raise

        # Commit cursors only after a full poll. A transport failure can then be
        # retried without silently consuming logs already read from another ring.
        heads = {plugin: self._heads[plugin] for plugin in plugins if plugin in self._heads}
        formats = {plugin: self._formats[plugin] for plugin in plugins if plugin in self._formats}
        lines = []
        heartbeats = []
        lost = dropped = 0
        for plugin in plugins:
            if plugin_id is not None and plugin.id != plugin_id:
                continue
            if plugin.ring % HEADER_BYTES or not _in_arena(plugin.ring, HEADER_BYTES):
                raise ValueError(f'{plugin.id} 的 fxlog 环地址无效')
            magic, stride, capacity, head, writer_dropped, _ = _RING.unpack(self._read(plugin.ring, HEADER_BYTES))
            if (magic != RING_MAGIC or stride != _ENTRY.size or not capacity or capacity & (capacity - 1)
                    or not _in_arena(plugin.ring, HEADER_BYTES + capacity * stride)):
                raise ValueError(f'{plugin.id} 的 fxlog 环头无效')
            previous = heads.get(plugin, 0)
            if head < previous:
                previous = 0
                formats.pop(plugin, None)
            first = max(previous, head - capacity)
            new_lines, torn = self._ring_lines(plugin, capacity, first, head, formats.setdefault(plugin, {}))
            lost += first - previous + torn
            dropped += writer_dropped
            lines.extend(new_lines)
            heartbeats.extend(self._heartbeats(plugin, latest_ms))
            heads[plugin] = head

        self._heads, self._formats = heads, formats
        lines.sort(key=lambda line: -_time_delta(latest_ms, line.t_ms))
        return FxlogSnapshot(latest_ms, plugins, tuple(lines), tuple(heartbeats), lost, dropped)
