"""Build the headless Z8 C3.11 Layer 0 v2 provider for Pure WLM."""

import argparse
import json
from pathlib import Path
import struct
import subprocess

from build_sdk_module import main as build_frontend
from build_z8_jpeg_lut import FOUNDATION, LLVM, audit_simd, compile_audited_unit, write_foundation_bindings
from wlm_bundle import decode_bundle, encode_bundle
from wlm_native import compose_components, encode_component
from z8_native_arena import BASE, STOCK_SHA256, arena_profile, verify_arena
from z8_native_capsule import EXEC, HOOK, IMAGE, INSTRUCTION, WRITE, Patch, _word_branch, decode, encode, sha


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'modules/lut_preview'
JPEG = ROOT / 'modules/jpeg_lut'
NAME = 'lut_preview'
SELECTOR = 0x400
SEGMENTS = ((0x40BF0000, 0x7000, 5, EXEC), (0x40BF7000, 0x2000, 4, 0), (0x40BF9000, 0x6000, 6, WRITE))
HOOKS = (
    (0x40E9A540, 0x40CD7AD8, '66f5f897', 'fx_layer0_enqueue_hook'),
    (0x40CD862C, 0x402B6D88, 'd779d797', 'fx_layer0_display_hook'),
    (0x40CD72A4, 0x40CD9640, 'e7080094', 'fx_layer0_vsync_hook'),
    (0x40CD72D4, 0x40CD9640, 'db080094', 'fx_layer0_vsync_hook'),
    (0x40D67328, 0x40D60E94, 'dbe6ff97', 'fx_layer0_power_sync_hook'),
    (0x40D67340, 0x40D60E94, 'd5e6ff97', 'fx_layer0_power_sync_hook'),
)
POWER_SEQUENCE = bytes.fromhex(
    '610240b960018052dbe6ff97610240b9e0031d3206000014610240b940018052d5e6ff97'
    '610240b9e00b0032f37b41a9f40742f8c4e6ff17'
)
DIAGNOSTICS = ('photo_rear_set_display_mode', 'photo_rear_set_copy_mode', 'photo_rear_set_source_lag',
               'photo_rear_metrics', 'photo_rear_evidence', 'photo_rear_power')


def branch_link(site, target):
    return (int.from_bytes(_word_branch(site, target), 'little') | 0x80000000).to_bytes(4, 'little')


# Generate the runtime checks from the two hash-pinned, user-supplied MAIN images.
REAR_BINDINGS = (
    0x40CD7AD8, 0x402B6D88, 0x40CD9640, 0x40D60E94, 0x40E9A570,
    0x40D2C698, 0x40D2C660, 0x41186780, 0x41186810, 0x4104D2DC,
    0x4104D600, 0x40393F6C, 0x40393F28, 0x4101553C,
)


def write_rear_bindings(stock, foundation, output):
    rows = ['static const struct { uintptr_t address; uint8_t prefix[32]; } layer0_bindings[] = {']
    for address in REAR_BINDINGS:
        start = address - BASE
        prefix = stock[start:start + 32]
        if len(prefix) != 32 or prefix != foundation[start:start + 32]:
            raise ValueError(f'rear binding differs at {address:#x}')
        values = ', '.join(f'0x{byte:02x}' for byte in prefix)
        rows.append(f'    {{0x{address:08x}u, {{{values}}}}},')
    (output / 'layer0_stock_prefixes.h').write_text('\n'.join([*rows, '};', '']), encoding='ascii')


def build(stock_path, foundation_path, output, llvm):
    stock, foundation = stock_path.read_bytes(), foundation_path.read_bytes()
    verify_arena(stock)
    if sha(foundation) != FOUNDATION:
        raise ValueError('exact cold-retry3 MAIN required')
    output = output.resolve()
    if not output.is_relative_to(ROOT / 'build'):
        raise ValueError('new output under build/ required')
    output.mkdir(parents=True)
    write_foundation_bindings(foundation, output, shared_output=True)
    write_rear_bindings(stock, foundation, output)

    def run(tool, *args):
        path = llvm / tool
        if path.with_suffix('.exe').is_file():
            path = path.with_suffix('.exe')
        return subprocess.check_output([str(path), *map(str, args)], text=True, timeout=60)

    flags = ('--target=aarch64-none-elf', '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-stack-protector', '-fno-unwind-tables',
             '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections', '-mgeneral-regs-only',
             '-mstrict-align', '-mno-outline-atomics', '-DFX_FREESTANDING=1', '-DLAYER0_DEFAULT_DISPLAY_MODE=1',
             '-DLAYER0_DEFAULT_COPY_MODE=2', '-DFX_LUT_PREVIEW_SMP=1',
             '-I', ROOT / 'include', '-I', JPEG, '-I', output)
    sources = ((SOURCE / 'layer0_provider.c', '-Os'), (SOURCE / 'layer0_color.c', '-O2'),
               (SOURCE / 'layer0_color_neon.c', '-O2'),
               (SOURCE / 'photo_workspace.c', '-Oz'), (JPEG / 'cube.c', '-O2'), (JPEG / 'smp.c', '-Os'),
               (ROOT / 'src/memory.c', '-Oz'))
    objects = []
    for source, optimize in sources:
        obj = output / (source.stem + '.o')
        unit_flags = flags if source.stem != 'layer0_color_neon' else tuple(
            flag for flag in flags if flag != '-mgeneral-regs-only')
        compile_audited_unit(run, (*unit_flags, optimize), source, obj)
        objects.append(obj)

    elf = output / 'native.elf'
    entries = {*DIAGNOSTICS, *(name for _, _, _, name in HOOKS)}
    run('ld.lld', '-m', 'aarch64elf', '--build-id=none', '-z', 'max-page-size=4096', '--gc-sections',
        *(f'--undefined={name}' for name in sorted(entries)), '-T', SOURCE / 'native.ld', *objects, '-o', elf)
    simd_audit = audit_simd(run, elf, ('layer0_grade_nv12_planes_neon', 'layer0_neon_sentinel'))
    symbols = {fields[3]: {'address': int(fields[0], 16), 'bytes': int(fields[1], 16)}
               for line in run('llvm-nm', '-S', elf).splitlines()
               if len(fields := line.split()) == 4}
    data = elf.read_bytes()
    # Older Layer 0 analysis tools use this filename.
    (output / 'preview.elf').write_bytes(data)
    entry = symbols['fx_native_pack_entry']['address']
    kind, machine, _, elf_entry, table, _, _, _, stride, count, *_ = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    if ((data[:7], kind, machine, elf_entry, stride, count) != (b'\x7fELF\x02\x01\x01', 2, 183, entry, 56, 3) or
            run('llvm-nm', '--undefined-only', elf).strip()):
        raise ValueError('unexpected fixed AArch64 ELF layout or undefined symbol')

    patches, segments = [], []
    for index, (address, limit, permissions, flags) in enumerate(SEGMENTS):
        typ, perm, offset, va, pa, file_size, size, align = struct.unpack_from('<II6Q', data, table + index * stride)
        if (typ, perm, va, pa, align) != (1, permissions, address, address, 4096) or not (
                0 < size <= limit and file_size <= size and offset + file_size <= len(data)):
            raise ValueError('Layer 0 exceeds its native reservation or has incompatible ELF permissions')
        old = foundation[va - BASE:va - BASE + size]
        if old != bytes(size) or stock[va - BASE:va - BASE + size] != old:
            raise ValueError('Layer 0 native reservation is occupied')
        new = data[offset:offset + file_size].ljust(size, b'\0')
        patches.append(Patch(IMAGE, flags, va, old, new))
        segments.append(dict(name=('RX', 'RO', 'RW')[index], address=hex(address),
                             memory_bytes=size, file_bytes=file_size, limit=limit, flags=flags, sha256=sha(new)))

    rx = patches[0]
    if not rx.address <= entry <= rx.address + len(rx.new) - 4:
        raise ValueError('Layer 0 entry is outside the executable segment')
    power = 0x40D67320 - BASE
    if any(image[power:power + len(POWER_SEQUENCE)] != POWER_SEQUENCE for image in (stock, foundation)):
        raise ValueError('stock OFF/cancel command sequence differs')
    hook_report = []
    for site, original, preimage, name in sorted(HOOKS):
        offset = site - BASE
        old = bytes.fromhex(preimage)
        if old != branch_link(site, original) or any(image[offset:offset + 4] != old for image in (stock, foundation)):
            raise ValueError(f'{name} preimage differs at {site:#x}')
        target = symbols[name]['address']
        if target % 4 or not rx.address <= target <= rx.address + len(rx.new) - 4:
            raise ValueError(f'{name} is outside executable segment')
        new = branch_link(site, target)
        patches.append(Patch(HOOK, INSTRUCTION, site, old, new, 0))
        hook_report.append(dict(symbol=name, address=hex(site), stock_target=hex(original), target=hex(target),
                                before=old.hex(), after=new.hex(), depends=0))

    target = bytes.fromhex(STOCK_SHA256)
    required = arena_profile()['requires_id']
    identity = int.from_bytes(bytes.fromhex(sha(data))[:8], 'little') or 1
    capsule = encode(patches, identity, required, entry)
    component = encode_component(NAME, 2, SELECTOR, 130, [dict(mask=SELECTOR, value=SELECTOR, capsule=capsule)],
                                 target=target, requires_id=required)
    plan, composed = compose_components([component], target=target, requires_id=required)
    if decode(plan)['patches'] != patches or composed['entries'] != [entry]:
        raise ValueError('Layer 0 records changed during composition')
    frontend_dir = output / 'frontend'
    build_frontend([str(SOURCE / 'module.c'), '--id', NAME, '--target', 'z8-c311', '--required', '0',
                    '--output-dir', str(frontend_dir), '--llvm-bin', str(llvm), '--optimize', 's'])
    filename = NAME + '.wlm'
    frontend = (frontend_dir / filename).read_bytes()
    bundle = encode_bundle(frontend, NAME, [component], target)
    parsed = decode_bundle(bundle, module_id=NAME, target=target)
    if parsed['frontend'] != frontend or [row['data'] for row in parsed['components']] != [component]:
        raise ValueError('Layer 0 WLM changed the frontend or component')
    (output / filename).write_bytes(bundle)
    build_inputs = [Path(__file__), *SOURCE.glob('*.h'), SOURCE / 'module.c', SOURCE / 'native.ld',
                    JPEG / 'neon_sentinel.h', JPEG / 'smp.h',
                    ROOT / 'include/fenestra_z8_photo_lut.h', ROOT / 'include/fenestra_z8_lut_release.h']
    report = dict(module=NAME, selector=SELECTOR, native_version=2, capabilities=0,
                  default_display_mode=1, default_copy_mode=2, default_source_lag=1, physical_verified=False,
                  stock_sha256=STOCK_SHA256, foundation_sha256=FOUNDATION, frontend_sha256=sha(frontend),
                  elf_sha256=sha(data), wlm_sha256=sha(bundle), wlm_bytes=len(bundle), simd_audit=simd_audit,
                  native_component_sha256=sha(component), native_entry=hex(entry), native_hooks=len(hook_report),
                  hooks=hook_report, segments=segments, diagnostics={name: symbols[name] for name in DIAGNOSTICS},
                  sources={source.relative_to(ROOT).as_posix(): sha(source.read_bytes()) for source, _ in sources},
                  build_inputs={path.relative_to(ROOT).as_posix(): sha(path.read_bytes()) for path in build_inputs})
    (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, default=ROOT / 'inputs/z8-c311-main.bin')
    parser.add_argument('--foundation', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, default=LLVM)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.stock, args.foundation, args.output, args.llvm_bin.resolve()), indent=2))
