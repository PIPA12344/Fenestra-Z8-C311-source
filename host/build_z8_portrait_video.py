"""Build a native portrait-video WLM against the exact Z8 cold-retry3 MAIN."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from build_sdk_module import main as build_frontend
from wlm_bundle import decode_bundle, encode_bundle
from wlm_native import compose_components, decode_component, encode_component
from z8_native_arena import BASE, STOCK_SHA256, arena_profile, verify_arena
from z8_native_capsule import EXEC, HOOK, IMAGE, INSTRUCTION, WRITE, Patch, _word_branch, decode, encode, sha

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'modules/portrait_video'
LLVM = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin')
FOUNDATION = '0e0114267ef82caf78b7795a470192b434528ec0f4a8a6b031d1a003c4c63dfd'
MODULE = 'portrait_video'
SELECTOR = 1 << 6
TARGET = bytes.fromhex(STOCK_SHA256)
SLOT = 0x40be0000
PAGE = 4096
HOOKS = {
    # First video sample of a file; the generic create at 0x40cebe38 is not
    # used by the movie recorder (live counters, 2026-11-10).
    0x40ce42dc: ('portrait_sample_hook', 0xb9455e88, False),
    0x40ceefdc: ('portrait_close_hook', 0x94000d93, True),
    0x40cefd60: ('portrait_tkhd_hook', 0x97ffd09f, True),
}


def build(stock_path, foundation_path, output, llvm, *, fxlog=True):
    stock, foundation = stock_path.read_bytes(), foundation_path.read_bytes()
    verify_arena(stock)
    if sha(foundation) != FOUNDATION:
        raise ValueError('exact cold-retry3 MAIN required')
    output = output.resolve()
    if not output.is_relative_to(ROOT / 'build'):
        raise ValueError('new output under build/ required')
    output.mkdir(parents=True)

    def run(name, *args):
        path = llvm / name
        if path.with_suffix('.exe').is_file():
            path = path.with_suffix('.exe')
        return subprocess.check_output([str(path), *map(str, args)], text=True, timeout=60)

    arch = '--target=aarch64-none-elf'
    defines = []
    if fxlog:
        from fxlog import FXLOG_DIR, directory_page, shared_component

        page = directory_page()
        offset = FXLOG_DIR - BASE
        if foundation[offset:offset+len(page)] != bytes(len(page)):
            raise ValueError('shared fxlog directory slot is occupied')
        build_sources = [SOURCE / name for name in ('native.c', 'native.ld', 'hooks.S')]
        build_sources += [ROOT / 'include' / 'fenestra_fxlog.h']
        source_id = sha(b''.join(bytes.fromhex(sha(path.read_bytes().replace(b'\r\n', b'\n')))
                                 for path in build_sources))
        build_id = int(source_id[:16], 16) or 1
        defines = ['-DPORTRAIT_FXLOG', f'-DPORTRAIT_BUILD_ID=0x{build_id:016x}ULL']

    run('clang', arch, '-O2', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-mstrict-align', '-mgeneral-regs-only', '-mno-outline-atomics', '-I', ROOT / 'include',
        *defines, '-c', SOURCE / 'native.c', '-o', output / 'native.o')
    run('clang', arch, '-c', SOURCE / 'hooks.S', '-o', output / 'hooks.o')
    linker = output / 'native.ld'
    linker.write_text(run('clang', arch, '-E', '-P', '-x', 'c', *defines, SOURCE / 'native.ld'),
                      encoding='ascii', newline='\n')
    elf = output / 'native.elf'
    run('ld.lld', '-m', 'aarch64elf', '--build-id=none', '-z', 'max-page-size=4096',
        '-T', linker, '--defsym=portrait_sample_resume=0x40ce42e0',
        '--defsym=portrait_stock_reset=0x40cf2628', '--defsym=portrait_stock_copy=0x40ce3fdc',
        output / 'native.o', output / 'hooks.o', '-o', elf)
    symbols = {parts[2]: int(parts[0], 16) for line in run('llvm-nm', '-n', elf).splitlines()
               if len(parts := line.split()) == 3}
    data = elf.read_bytes()
    entry = symbols['fx_native_pack_entry']
    kind, machine, _, elf_entry, table, _, _, _, stride, count, *_ = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    segments = [(5, EXEC), (4, 0), (6, WRITE)]
    if fxlog:
        segments.append((6, WRITE))
    if (data[:7], kind, machine, elf_entry, stride, count) != (b'\x7fELF\x02\x01\x01', 2, 183, entry, 56, len(segments)):
        raise ValueError('unexpected fixed AArch64 ELF layout')

    patches = []
    for i, (permission, flags) in enumerate(segments):
        kind, actual, at, va, pa, n, size, align = struct.unpack_from('<II6Q', data, table + i*stride)
        address = SLOT + i*PAGE
        if ((kind, actual, va, pa, align) != (1, permission, address, address, PAGE) or
                not 0 < size <= PAGE or n > size or at + n > len(data) or (i == 0 and not va <= entry < va+n)):
            raise ValueError('native image out of its reserved slot')
        if i == 3:
            size = PAGE
        old = foundation[va-BASE:va-BASE+size]
        if old != bytes(size):
            raise ValueError('native slot is occupied')
        patches.append(Patch(IMAGE, flags, va, old, data[at:at+n].ljust(size, b'\0')))

    for address, (name, word, link) in HOOKS.items():
        old = struct.pack('<I', word)
        offset = address - BASE
        if stock[offset:offset+4] != old or foundation[offset:offset+4] != old:
            raise ValueError(f'video hook preimage differs at {address:#x}')
        target = symbols[name]
        if not SLOT <= target < SLOT + len(patches[0].new):
            raise ValueError(f'hook target outside code: {name}')
        new = _word_branch(address, target)
        if link:
            new = struct.pack('<I', struct.unpack('<I', new)[0] | 0x80000000)
        patches.append(Patch(HOOK, INSTRUCTION, address, old, new))

    required = arena_profile()['requires_id']
    identity = int.from_bytes(bytes.fromhex(sha(data))[:8], 'little') or 1
    capsule = encode(patches, identity, required, entry)
    component = encode_component(MODULE, 1, SELECTOR, 96, [dict(mask=SELECTOR, value=SELECTOR, capsule=capsule)],
                                 target=TARGET, requires_id=required)
    components = [component]
    if fxlog:
        components.append(shared_component(TARGET, required))
    frontend = output / 'frontend'
    build_frontend([str(SOURCE / 'module.c'), '--id', MODULE, '--target', 'z8-c311',
                    '--output-dir', str(frontend), '--llvm-bin', str(llvm), '--optimize', 's'])
    name = MODULE + '.wlm'
    bundle = encode_bundle((frontend / name).read_bytes(), MODULE, components, TARGET)
    parsed = decode_bundle(bundle, module_id=MODULE, target=TARGET)
    plan, composition = compose_components([row['data'] for row in parsed['components']],
                                           target=TARGET, requires_id=required)
    (output / name).write_bytes(bundle)
    (output / (MODULE + '.fnc')).write_bytes(plan)
    report = dict(module=MODULE, wlm_bytes=len(bundle), wlm_sha256=sha(bundle), main_sha256=FOUNDATION,
                  stock_sha256=STOCK_SHA256, plan_sha256=sha(plan), native_records=len(patches),
                  symbols=symbols, physical_recording_verified=False)
    if fxlog:
        report.update(fxlog_build_id=f'{build_id:016x}', composed_records=len(decode(plan)['patches']),
                      components=[])
        for raw in components:
            info = decode_component(raw, target=TARGET, requires_id=required,
                                    selection=composition['selection'])
            records = [dict(address=f'{p.address:#x}', bytes=len(p.new), kind=p.kind, flags=p.flags)
                       for p in decode(info['capsule'])['patches']]
            report['components'].append(dict(id=info['id'], sha256=info['sha256'], records=records))
    (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, default=ROOT / 'inputs/z8-c311-main.bin')
    parser.add_argument('--foundation', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, default=LLVM)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/z8-portrait-video-20261124/r4')
    parser.add_argument('--no-fxlog', action='store_true', help='rebuild the r3 WLM without logging')
    args = parser.parse_args()
    if args.no_fxlog and args.output.resolve() == parser.get_default('output'):
        parser.error('--no-fxlog requires a separate --output directory under build/')
    print(json.dumps(build(args.stock, args.foundation, args.output, args.llvm_bin.resolve(),
                           fxlog=not args.no_fxlog), indent=2))
