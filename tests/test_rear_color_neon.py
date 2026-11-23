"""Run the production NV12 kernels in AArch64 Unicorn."""

from pathlib import Path
import random
import shutil
import struct
import subprocess

import pytest

from test_jpeg_cube_neon import CACHE, CUBE, KernelReplay, cube_fixture, verify_sentinel
from toolchain import host_compiler


ROOT = Path(__file__).resolve().parents[1]
REAR, JPEG = ROOT / 'modules/lut_preview', ROOT / 'modules/jpeg_lut'
SCALAR, NEON = 'layer0_grade_nv12_rows', 'layer0_grade_nv12_rows_neon'
PAIR, SENTINEL = 'layer0_grade_nv12_pair_scalar', 'layer0_neon_sentinel'
W, H = 1024, 682
SRC, DST = 0xB00003, 0xD00005
CACHE_BYTES = 4 << 20


def llvm_tool(name):
    clang = Path(host_compiler('clang'))
    return shutil.which(name) or clang.with_name(name + clang.suffix)


def symbols(elf):
    return {f[2]: int(f[0], 16)
            for line in subprocess.check_output([llvm_tool('llvm-nm'), elf], text=True).splitlines()
            if len(f := line.split()) == 3}


@pytest.fixture(scope='module')
def kernel(tmp_path_factory):
    work = tmp_path_factory.mktemp('rear-neon')
    flags = ('--target=aarch64-none-elf', '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-stack-protector',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections',
             '-fdata-sections', '-mstrict-align', '-mno-outline-atomics', '-DFX_FREESTANDING=1',
             '-I', JPEG, '-O2')
    objects = []
    for source in (JPEG / 'cube.c', REAR / 'layer0_color.c', REAR / 'layer0_color_neon.c'):
        obj = work / (source.stem + '.o')
        extra = () if source.stem.endswith('_neon') else ('-mgeneral-regs-only',)
        subprocess.run([llvm_tool('clang'), *flags, *extra, '-c', source, '-o', obj],
                       check=True, timeout=30)
        objects.append(obj)
    elf = work / 'native.elf'
    subprocess.run([llvm_tool('ld.lld'), '-m', 'aarch64elf', '-z', 'max-page-size=4096',
                    '--gc-sections', '-Ttext=0x100000', '-e', NEON, '--undefined=' + SCALAR,
                    '--undefined=' + SENTINEL, *objects, '-o', elf], check=True, timeout=30)
    return elf


class RearReplay(KernelReplay):
    def __init__(self, elf, cube):
        super().__init__(elf, symbols(elf))
        for pointer in (SRC, DST):
            self.vm.mem_map(pointer & -4096, 1 << 20)
        self.vm.mem_write(CUBE, cube)

    def frame(self, name, source, width=W, height=H, *, in_place=False, first=0, end=None):
        end = height if end is None else end
        self.vm.mem_write(SRC - 1, b'\xa5' + source + b'\xa5')
        self.vm.mem_write(DST - 1, b'\xa5' * (len(source) + 2))
        dst = SRC if in_place else DST
        self.instructions = 0
        if name == PAIR:
            plane = width * height
            for row in range(first, end, 2):
                y, uv = row * width, plane + row // 2 * width
                self.call(PAIR, CUBE, CACHE, SRC + y, SRC + y + width, SRC + uv,
                          dst + y, dst + y + width, dst + uv, width)
        else:
            dimensions = (width, height) if name == NEON else ()
            assert self.call(name, CUBE, CACHE, SRC, len(source), dst, len(source),
                             first, end, *dimensions) == 1
        for address in (SRC, DST):
            assert self.vm.mem_read(address - 1, 1) == self.vm.mem_read(address + len(source), 1) == b'\xa5'
        if not in_place:
            assert self.vm.mem_read(SRC, len(source)) == source
        return bytes(self.vm.mem_read(dst, len(source))), self.instructions


def frame_comparison(scalar_elf, neon_elf, cube, source):
    before, after = RearReplay(scalar_elf, cube), RearReplay(neon_elf, cube)
    counts = {}
    for phase in ('cold', 'warm'):
        expected, scalar = before.frame(SCALAR, source)
        actual, neon = after.frame(NEON, source)
        assert actual == expected
        assert after.vm.mem_read(CACHE, CACHE_BYTES) == before.vm.mem_read(CACHE, CACHE_BYTES)
        counts[phase] = {'scalar': scalar, 'neon': neon}
    return counts


def test_full_frame_parity_and_instruction_counts(kernel):
    rng = random.Random(0xC311)
    # Eight repeating chromas model a working set that fits the tagged cache.
    palette = bytes((127, 121, 119, 125, 109, 149, 85, 162, 111, 148, 79, 178, 88, 168, 90, 161))
    source = rng.randbytes(W * H) + palette * (W * H // (2 * len(palette)))
    counts = frame_comparison(kernel, kernel, cube_fixture(), source)
    print(counts)
    assert all(phase['neon'] < phase['scalar'] for phase in counts.values())


@pytest.mark.parametrize('width', (2, 4, 6, 8, 10, 12, 14, 130, 134, 198))
def test_unaligned_tails_bands_and_tag_collisions(kernel, width):
    scalar, neon = (RearReplay(kernel, cube_fixture()) for _ in range(2))
    height = 6
    rng = random.Random(width)
    chroma = bytes((0, 255, 64, 63, 128, 191, 192, 127, 255, 0, 63, 64, 191, 128, 127, 192))
    source = rng.randbytes(width * height) + (chroma * width)[:width * height // 2]
    for first, end, in_place in ((0, height, False), (0, height, True), (2, 4, False), (4, 4, False)):
        expected, _ = scalar.frame(PAIR, source, width, height, in_place=in_place, first=first, end=end)
        actual, _ = neon.frame(NEON, source, width, height, in_place=in_place, first=first, end=end)
        assert actual == expected
        assert neon.vm.mem_read(CACHE, CACHE_BYTES) == scalar.vm.mem_read(CACHE, CACHE_BYTES)


def test_every_possible_four_way_chroma_sum(kernel):
    replay = RearReplay(kernel, cube_fixture())
    cache = bytearray(CACHE_BYTES)
    y = bytes([0, 1]) * (W // 2) + bytes([2, 3]) * (W // 2)
    uv, expected = bytearray(), bytearray()
    for quad in range(2 * W // 2):
        cb, cr = quad % 64, quad // 64
        uv.extend((cb, cr))
        total = min(quad, 1020)
        expected.extend(((total + 2) >> 2, (1022 - total) >> 2))
        for lane in range(4):
            c = min(255, max(0, total - 255 * ((lane + quad) % 4)))
            r = min(255, max(0, 1020 - total - 255 * ((lane - quad) % 4)))
            struct.pack_into('<I', cache, ((cr << 14) | (cb << 8) | lane) * 4,
                             0x01000000 | (r << 16) | (c << 8) | lane)
    replay.vm.mem_write(CACHE, bytes(cache))
    source = y * 2 + uv
    actual, _ = replay.frame(NEON, bytes(source), W, 4)
    assert actual == y * 2 + expected
    assert replay.vm.mem_read(CACHE, CACHE_BYTES) == cache


def test_invalid_frames_are_rejected(kernel):
    replay = RearReplay(kernel, cube_fixture())
    width, height = 10, 4
    size = width * height * 3 // 2
    for sy, dy, first, end, w, h in ((size - 1, size, 0, 4, 10, 4), (size, size - 1, 0, 4, 10, 4),
                                    (size, size, 3, 4, 10, 4), (size, size, 4, 2, 10, 4),
                                    (size, size, 2, 6, 10, 4), (size, size, 0, 2, 9, 4),
                                    (size, size, 0, 2, 10, 3), (size, size, 0, 2, 0, 4)):
        assert replay.call(NEON, CUBE, CACHE, SRC, sy, DST, dy, first, end, w, h) == 0


def test_rear_sentinel_preserves_pcs_and_checks_all_irq_lanes(kernel):
    verify_sentinel(RearReplay(kernel, cube_fixture()), SENTINEL)
