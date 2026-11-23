"""Run the isolated production NEON kernel and sentinel in AArch64 Unicorn."""

from pathlib import Path
import random
import shutil
import struct
import subprocess

from unicorn import Uc, UC_ARCH_ARM64, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_MODE_ARM
from unicorn.arm64_const import (
    UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_FPCR, UC_ARM64_REG_FPSR, UC_ARM64_REG_LR,
    UC_ARM64_REG_PC, UC_ARM64_REG_PSTATE, UC_ARM64_REG_Q0, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
)

from toolchain import host_compiler


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "modules/jpeg_lut"
PAGE = 4096
CUBE, CACHE, IO, STACK = 0x300000, 0x400000, 0x900000, 0xA00000
STOP, DELAY = STACK + 0x10000, STACK + 0x10100
SCALAR, NEON = "jpeg_cube_cache_row_scalar", "jpeg_cube_cache_row_neon"
WARM, SENTINEL = "jpeg_cube_cache_warm", "jpeg_neon_sentinel"


def build_kernel(directory):
    clang = Path(host_compiler("clang"))

    def sibling(name):
        local = clang.with_name(name + clang.suffix)
        return shutil.which(name) or local

    flags = ("--target=aarch64-none-elf", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wconversion", "-Wshadow", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
             "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-mstrict-align",
             "-mno-outline-atomics", "-ffunction-sections", "-fdata-sections")
    objects = []
    for name in ("cube", "cube_frame", "cube_frame_neon"):
        obj = directory / (name + ".o")
        extra = () if name.endswith("neon") else ("-mgeneral-regs-only", "-flto")
        optimize = "-Oz" if name == "cube" else "-O2"
        subprocess.run([clang, *flags, *extra, optimize, "-c", SOURCE / (name + ".c"), "-o", obj],
                       check=True, timeout=30)
        objects.append(obj)
    elf = directory / "kernel.elf"
    subprocess.run([sibling("ld.lld"), "-m", "aarch64elf", "-z", "max-page-size=4096", "--gc-sections",
                    "-Ttext=0x100000", "-e", NEON, "--undefined=" + WARM, "--undefined=" + SENTINEL,
                    *objects, "-o", elf], check=True, timeout=30)
    symbols = {}
    for line in subprocess.check_output([sibling("llvm-nm"), elf], text=True, timeout=30).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    return elf, symbols


class KernelReplay:
    def __init__(self, elf, symbols):
        self.vm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.symbols = symbols
        data = elf.read_bytes()
        table, = struct.unpack_from("<Q", data, 32)
        stride, count = struct.unpack_from("<HH", data, 54)
        pages = set()
        for i in range(count):
            kind, _, offset, va, _, raw, memory, _ = struct.unpack_from("<II6Q", data, table + i * stride)
            if kind == 1:
                for page in range(va & -PAGE, va + memory, PAGE):
                    if page not in pages:
                        self.vm.mem_map(page, PAGE)
                        pages.add(page)
                self.vm.mem_write(va, data[offset:offset + raw])
        for address, size in ((CUBE, 0x40000), (CACHE, 4 << 20), (IO, 0x10000), (STACK, 0x11000)):
            self.vm.mem_map(address, size)
        self.vm.reg_write(UC_ARM64_REG_PSTATE, 5)  # EL1h, IRQ unmasked
        self.vm.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.instructions = 0
        self.vm.hook_add(UC_HOOK_BLOCK, self._block)

    def _block(self, _, address, size, __):
        if address != STOP:
            self.instructions += size // 4

    def call(self, name, *args):
        for i, value in enumerate(args[:8]):
            self.vm.reg_write(UC_ARM64_REG_X0 + i, value)
        self.vm.reg_write(UC_ARM64_REG_LR, STOP)
        stack = STOP - 32
        if len(args) > 8:
            self.vm.mem_write(stack, struct.pack(f"<{len(args) - 8}Q", *args[8:]))
        self.vm.reg_write(UC_ARM64_REG_SP, stack)
        self.vm.emu_start(self.symbols[name], STOP, timeout=30_000_000, count=100_000_000)
        assert self.vm.reg_read(UC_ARM64_REG_PC) == STOP, name
        return self.vm.reg_read(UC_ARM64_REG_X0)

    def row(self, name, y, uv, alignment=0, in_place=False):
        pointers = tuple(IO + i * 0x4000 + (alignment + 1) * (2 * i + 1) + 1 for i in range(4))
        sy, sc, dy, dc = pointers
        for p, contents in zip(pointers, (y, uv, b"\xa5" * len(y), b"\xa5" * len(y))):
            self.vm.mem_write(p - 1, b"\xa5" + contents + b"\xa5")
        if in_place:
            dy, dc = sy, sc
        misses = self.call(name, CUBE, CACHE, sy, sc, dy, dc, len(y))
        for p in pointers:
            assert self.vm.mem_read(p - 1, 1) == self.vm.mem_read(p + len(y), 1) == b"\xa5"
        if not in_place:
            assert self.vm.mem_read(sy, len(y)) == y and self.vm.mem_read(sc, len(uv)) == uv
        return bytes(self.vm.mem_read(dy, len(y))), bytes(self.vm.mem_read(dc, len(y))), misses


def cube_fixture():
    grid = 3
    lo, hi = (60_000, -60_000, 100_000), (950_000, 870_000, 1_100_000)
    axes = []
    for low, high in zip(lo, hi):
        span = (high - low) * 255
        for v in range(256):
            offset = max(0, min(span, v * 1_000_000 - low * 255))
            axes.append((offset * (grid - 1) * 256 + span // 2) // span)
    rng = random.Random(0xC311)
    nodes = [rng.randrange(65536) for _ in range(3 * grid**3)]
    return struct.pack(f"<3I6i768H{len(nodes)}H", grid, grid**3, 0, *lo, *hi, *axes, *nodes)


def test_neon_matches_scalar_on_hits_misses_and_tails(tmp_path):
    replay = KernelReplay(*build_kernel(tmp_path))
    replay.vm.mem_write(CUBE, cube_fixture())
    replay.call(WARM, CUBE, CACHE, 0, 256)
    warm = bytes(replay.vm.mem_read(CACHE, 4 << 20))
    rng = random.Random(19883)
    # Replay the same seven rows in one stripe and in three imbalanced
    # stripes. Chroma inputs collide across stripes and across tag changes.
    rows = []
    for i, width in enumerate((2, 6, 8, 10, 160, 1620, 8256)):
        if i < 3:
            uv = bytes([128]) * width
        else:
            uv = bytes(rng.randrange(256) for _ in range(width))
        rows.append((bytes(rng.randrange(256) for _ in range(width)), uv))
    scalar = [replay.row(SCALAR, *pair, i % 2) for i, pair in enumerate(rows)]
    replay.vm.mem_write(CACHE, warm)
    neon = [replay.row(NEON, *pair, i % 2, in_place=i % 3 == 0) for i, pair in enumerate(rows)]
    assert neon == scalar
    assert sum(row[2] for row in neon) > 0
    replay.vm.mem_write(CACHE, warm)
    for i in (4, 5, 0, 1, 6, 2, 3):
        assert replay.row(NEON, *rows[i], i % 2)[:2] == scalar[i][:2]


def test_sentinel_separates_pcs_yield_from_all_vector_irq_check(tmp_path):
    verify_sentinel(KernelReplay(*build_kernel(tmp_path)), SENTINEL)


def verify_sentinel(replay, symbol):
    vm = replay.vm
    vm.mem_write(DELAY, bytes.fromhex("e0031f2ac0035fd6"))  # mov w0,wzr; ret
    vectors = [(i + 1) * 0x1098765432101234567890123456789 for i in range(32)]
    for i, pattern in enumerate(vectors):
        vm.reg_write(UC_ARM64_REG_Q0 + i, pattern)
    vm.reg_write(UC_ARM64_REG_FPCR, 0x00400000)
    vm.reg_write(UC_ARM64_REG_FPSR, 0x08000000)

    calls = polls = 0
    bad_d = revoke = False
    irq_corruption = None
    low_mask = (1 << 64) - 1

    def delay_hook(cpu, *_):
        nonlocal calls
        calls += 1
        assert cpu.reg_read(UC_ARM64_REG_X0) == 1
        # The stock timer heap uses Q0 before the context save. Model all
        # legal AAPCS64 vector clobbers, including the upper half of Q8–Q15.
        for i in range(32):
            reg = UC_ARM64_REG_Q0 + i
            value = cpu.reg_read(reg) & low_mask if 8 <= i < 16 else 0x01234567
            cpu.reg_write(reg, value | (0xDEADBEEF << 96))
        if bad_d:
            reg = UC_ARM64_REG_Q0 + 8
            cpu.reg_write(reg, cpu.reg_read(reg) ^ 1)
        if revoke:
            cpu.reg_write(UC_ARM64_REG_CPACR_EL1, 0)

    def counter_hook(cpu, address, _, which):
        nonlocal polls
        register, reading_start = which
        polls = 0 if reading_start else polls + 1
        if polls == 1 and irq_corruption is not None:
            i, bit = irq_corruption
            reg = UC_ARM64_REG_Q0 + i
            cpu.reg_write(reg, cpu.reg_read(reg) ^ (1 << bit))
        cpu.reg_write(register, 100 + polls * 24000)
        cpu.reg_write(UC_ARM64_REG_PC, address + 4)

    entry = replay.symbols[symbol]
    counters = [(entry + offset * 4, opcode & 31)
                for offset, (opcode,) in enumerate(struct.iter_unpack("<I", vm.mem_read(entry, 0x180)))
                if opcode & 0xFFFFFFE0 == 0xD53BE020]  # MRS Xt, CNTPCT_EL0
    assert len(counters) == 2
    for i, (address, register) in enumerate(counters):
        vm.hook_add(UC_HOOK_CODE, counter_hook, (UC_ARM64_REG_X0 + register, i == 0),
                    begin=address, end=address)
    vm.hook_add(UC_HOOK_CODE, delay_hook, begin=DELAY, end=DELAY)

    vm.reg_write(UC_ARM64_REG_CPACR_EL1, 0)
    assert replay.call(symbol, DELAY) == 0
    vm.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    vm.reg_write(UC_ARM64_REG_PSTATE, 5 | 0x80)
    assert replay.call(symbol, DELAY) == 0
    assert calls == 0
    vm.reg_write(UC_ARM64_REG_PSTATE, 5)
    assert replay.call(symbol, DELAY) == 1 and calls == 1 and polls == 3

    bad_d = True
    assert replay.call(symbol, DELAY) == 2 and polls == 3  # No second IRQ window.
    bad_d = False
    for i in range(32):
        for bit in (0, 127):
            irq_corruption = i, bit
            previous = calls
            assert replay.call(symbol, DELAY) == 2 and calls == previous + 1 and polls == 3

    irq_corruption, revoke = None, True
    assert replay.call(symbol, DELAY) == 2
    assert vm.reg_read(UC_ARM64_REG_CPACR_EL1) == 0
    assert [vm.reg_read(UC_ARM64_REG_Q0 + i) & low_mask for i in range(8, 16)] == [
        vectors[i] & low_mask for i in range(8, 16)
    ]
    assert vm.reg_read(UC_ARM64_REG_FPCR) == 0x00400000
    assert vm.reg_read(UC_ARM64_REG_FPSR) == 0x08000000
