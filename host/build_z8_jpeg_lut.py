"""Package the card-backed JPEG 3D LUT WLM for cold-retry3."""

import json
from pathlib import Path
import re
import struct
import subprocess

from build_sdk_module import main as build_frontend
from wlm_bundle import decode_bundle, encode_bundle
from wlm_native import compose_components, decode_component, encode_component
from z8_native_arena import BASE, STOCK_SHA256, arena_profile, verify_arena
from z8_native_capsule import EXEC, HOOK, IMAGE, INSTRUCTION, WRITE, Patch, _word_branch, decode, encode, sha


ROOT = Path(__file__).resolve().parents[1]
MODULE = 'jpeg_lut'
SOURCE = ROOT / 'modules' / MODULE
LLVM = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin')
FOUNDATION = '0e0114267ef82caf78b7795a470192b434528ec0f4a8a6b031d1a003c4c63dfd'
BUILD_ID = 'f06231d84970cda74ff6afcdb2aad7d03f6fe0bc4a3ec76c15a9cfd40654579a'
SELECTOR = 1 << 8
SDK_MENU = 1 << 20
SDK_BINDING = 1 << 21
SDK_REQUIRED = SDK_MENU | SDK_BINDING
CARD_HELPERS = {
    'CARD_DRIVE': 0x401D450C, 'DSP_SUBMIT': 0x40C90AEC,
    'CPU_IMAGE_VIEW': 0x41186718, 'IMAGE_INVALIDATE': 0x40D2B95C, 'IMAGE_CLEAN': 0x40D2B9E4,
    'IMAGE_CV_CLEAN': 0x40D2B8D4, 'IMAGE_COPY': 0x4127A900, 'TASK_DELAY': 0x40187B00,
    'TASK_CREATE': 0x40185064, 'TASK_DELETE': 0x4018543C, 'TASK_RESUME': 0x401855E0,
    'SEM_CREATE': 0x40180E50, 'SEM_DELETE': 0x4018102C, 'SEM_SIGNAL': 0x401811E8, 'SEM_WAIT': 0x401815CC,
}
JPEG_ENCODER = 0x40E82090
HOOKS = ((0x40EAEB8C, 'fx_z8_jpeg_lut_main'), (0x40EAFE58, 'fx_z8_jpeg_lut_hd'),
         (0x40EB00EC, 'fx_z8_jpeg_lut_vga'), (0x40EB02DC, 'fx_z8_jpeg_lut_thumb'))
JPEG_RX_EXTENSION = (0x40BE6000, 0x40BE7000)
JPEG_ARENA = (JPEG_RX_EXTENSION[0], 0x40BF0000)
SEGMENTS = ((JPEG_ARENA[0], 0x8000, 5, EXEC), (0x40BEE000, 0x1000, 4, 0), (0x40BEF000, 0x1000, 6, WRITE))


def verify_jpeg_extension_preimage(stock, foundation):
    start, end = JPEG_RX_EXTENSION
    expected = bytes(end - start)
    for name, image in (('stock MAIN', stock), ('foundation MAIN', foundation)):
        if image[start - BASE:end - BASE] != expected:
            raise ValueError(f'JPEG LUT RX extension requires 4096 zero bytes in {name}')


def verify_jpeg_peers(components):
    target = bytes.fromhex(STOCK_SHA256)
    required = arena_profile()['requires_id']
    peers = {}
    for data in components:
        metadata = decode_component(data, target=target, requires_id=required)
        checksum, module_id = metadata['sha256'], metadata['id']
        if checksum in peers:
            continue
        # An older JPEG component has the same ID and cannot be composed with
        # this provider. Its variants still have to clear the newly used page.
        start, end = JPEG_RX_EXTENSION if module_id == MODULE else JPEG_ARENA
        for variant in metadata['variants']:
            value = variant['value']
            capsule = decode_component(data, target=target, requires_id=required, selection=value)['capsule']
            for patch in decode(capsule)['patches']:
                if patch.address < end and patch.address + len(patch.new) > start:
                    raise ValueError(f'{module_id} variant {value:#x} intersects JPEG LUT arena at {patch.address:#x}')
        peers[checksum] = dict(id=module_id, variants=len(metadata['variants']))
    return dict(start=hex(JPEG_ARENA[0]), end=hex(JPEG_ARENA[1]), collisions=0, peers=peers)


def assemble_unit(run, assembly, obj):
    run('clang', '--target=aarch64-none-elf', '-x', 'assembler', '-c', assembly, '-o', obj)


def compile_audited_unit(run, flags, source, obj):
    assembly = obj.with_suffix('.audit.s')
    emit = ('-E', '-P') if source.suffix == '.S' else ('-S',)
    run('clang', *flags, *emit, source, '-o', assembly)
    assemble_unit(run, assembly, obj)


def audit_simd(run, elf, isolated):
    """Check the assembly used to produce the linked objects, including LTO output."""
    linked = {f[2] for line in run('llvm-nm', '-n', elf).splitlines()
              if len(f := line.split()) == 3 and f[1].lower() == 't'}
    if not set(isolated) <= linked:
        raise ValueError(f'{elf}: missing linked isolated SIMD symbols')
    counts = dict.fromkeys(isolated, 0)
    instructions = 0
    sources = {}
    vector_operand = re.compile(r'(?<![.\w])(?:[bhsdqvzp](?:[12]?\d|3[01])|fpcr|fpsr|ffr)(?!\w)')
    # The trampoline contains an exact stock STR and branch, checked against
    # sealed machine-code bytes by inspect_menu_elf. The two markers are data.
    prefix_words = {'.inst0xf81d0ff6', '.inst0x17ddf51e', '.word0x46523431', '.word0x48425431'}
    for path in sorted(elf.parent.glob('*.audit.s')):
        source = path.read_bytes()
        sources[path.name] = sha(source)
        assembly = re.sub(r'/\*.*?\*/', '', source.decode(), flags=re.S)
        owner, executable = None, False
        for number, line in enumerate(assembly.splitlines(), 1):
            asm = line.split('//', 1)[0].strip()
            if not asm or asm.startswith('#'):
                continue
            if asm == '.text' or asm.startswith(('.section\t', '.section ')):
                executable = asm == '.text' or bool(re.search(r',\s*"[a-z]*x[a-z]*"', asm))
                owner = None
            elif asm.startswith('.size'):
                owner = None
            elif re.fullmatch(r'[A-Za-z_][\w.$]*:', asm):
                owner = asm[:-1]
            elif executable and re.match(r'\.(?:inst|word|long|quad|byte|short|hword|2byte|4byte|8byte)\b', asm):
                raw = re.sub(r'\s+', '', asm).lower()
                if path.name != 'photo_entry.audit.s' or raw not in prefix_words:
                    raise ValueError(f'{path}:{number}: unaudited raw encoding: {asm}')
            elif re.match(r'[a-z][\w.]*\b', asm) and not asm.endswith(':'):
                instructions += 1
                if vector_operand.search(asm):
                    if owner not in counts:
                        raise ValueError(f'{path}:{number}: FP/SIMD outside isolated functions: {asm}')
                    counts[owner] += 1
    if not instructions or not all(counts.values()):
        raise ValueError(f'{elf}: missing expected FP/SIMD instructions')
    return dict(method='assembler_inputs', isolated_instructions=counts, outside=0, assembly_sha256=sources)


def write_foundation_bindings(foundation, output, shared_output=False):
    if sha(foundation) != FOUNDATION:
        raise ValueError('exact cold-retry3 MAIN required for native bindings')
    helpers = {name: address for name, address in CARD_HELPERS.items()
               if shared_output or (not name.startswith('SEM_') and name not in ('TASK_CREATE', 'TASK_DELETE', 'TASK_RESUME'))}
    # The exact foundation identity pins these file and allocator entry points.
    helpers.update(CARD_OPEN=0x402DD4F8, CARD_READ=0x401D3BA0, CARD_CLOSE=0x402DD558,
                   HEAP_ALLOC=0x40D2C408, HEAP_FREE=0x40D2C448)
    binding = []
    rows = []
    for name, address in helpers.items():
        start = address - BASE
        prefix = foundation[start:start + 32]
        binding.append(f'#define JPEG_LUT_{name} 0x{address:08x}u')
        values = ', '.join(f'0x{byte:02x}' for byte in prefix)
        rows.append(f'    {{JPEG_LUT_{name}, {{{values}}}}},')
    binding += ['static const struct { uintptr_t address; uint8_t prefix[32]; } jpeg_lut_bindings[] = {', *rows, '};']
    (output / 'jpeg_lut_foundation.h').write_text('\n'.join(binding) + '\n', encoding='ascii')


def build(stock_path, foundation_path, output, llvm, linked=False, shared_output=False, *, peer_components):
    if shared_output and not linked:
        raise ValueError('shared output requires the headless linked frontend')
    stock, foundation = stock_path.read_bytes(), foundation_path.read_bytes()
    verify_arena(stock)
    if sha(foundation) != FOUNDATION:
        raise ValueError('exact cold-retry3 MAIN required')
    verify_jpeg_extension_preimage(stock, foundation)
    peer_scan = verify_jpeg_peers(peer_components)
    output = output.resolve()
    if not output.is_relative_to(ROOT / 'build'):
        raise ValueError('new output under build/ required')
    output.mkdir(parents=True)
    write_foundation_bindings(foundation, output, shared_output)

    def run(name, *args):
        path = llvm / name
        if path.with_suffix('.exe').is_file():
            path = path.with_suffix('.exe')
        return subprocess.check_output([str(path), *map(str, args)], text=True, timeout=60)

    flags = ('--target=aarch64-none-elf', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion',
             '-Wshadow', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
             '-fno-asynchronous-unwind-tables', '-mstrict-align', '-mgeneral-regs-only', '-mno-outline-atomics',
             '-flto', '-ffunction-sections', '-fdata-sections', '-DFX_FREESTANDING=1', '-I', ROOT / 'include', '-I', output)
    if shared_output:
        flags += ('-DFX_Z8_LUT_SHARED_OUTPUT=1',)
    lto_objects, native_objects = [], []
    sources = [SOURCE / name for name in ('native.c', 'cube.c', 'cube_frame.c', 'xm6.c')]
    if shared_output:
        sources.extend(SOURCE / name for name in ('cube_frame_neon.c', 'smp.c'))
    for src in (*sources, ROOT / 'src/memory.c'):
        obj = output / (src.stem + '.o')
        # Keep SIMD isolated even at link time. RTOS control and hook code
        # continue to build with the general-register ABI restriction.
        unit_flags = flags
        if src.stem == 'cube_frame_neon':
            unit_flags = tuple(flag for flag in flags if flag not in ('-mgeneral-regs-only', '-flto'))
        optimize = '-O2' if src.stem in ('cube_frame', 'cube_frame_neon') else '-Oz'
        if '-flto' in unit_flags:
            run('clang', *unit_flags, optimize, '-c', src, '-o', obj)
            lto_objects.append(obj)
        else:
            compile_audited_unit(run, (*unit_flags, optimize), src, obj)
            native_objects.append(obj)
    elf = output / 'native.elf'
    link = ('-m', 'aarch64elf', '--build-id=none', '-z', 'max-page-size=4096', '--gc-sections',
            '--undefined=fx_z8_jpeg_lut_control', '--undefined=fx_z8_jpeg_snapshot_meta',
            '--undefined=fx_z8_jpeg_phase_telemetry',
            *(f'--undefined={name}' for _, name in HOOKS), '-T', SOURCE / 'native.ld')
    lto = output / 'jpeg'
    run('ld.lld', *link, '--lto-emit-asm', *lto_objects, *native_objects, '-o', lto)
    assembly, combined = lto.with_suffix('.audit.s'), lto.with_suffix('.o')
    lto.with_suffix('.lto.s').rename(assembly)
    assemble_unit(run, assembly, combined)
    run('ld.lld', *link, *native_objects, combined, '-o', elf)
    simd_audit = audit_simd(run, elf, ('jpeg_cube_cache_row_neon', 'jpeg_neon_sentinel') if shared_output else ())
    symbols = {parts[2]: int(parts[0], 16) for line in run('llvm-nm', '-n', elf).splitlines()
               if len(parts := line.split()) == 3}
    data = elf.read_bytes()
    entry = symbols['fx_native_pack_entry']
    kind, machine, _, elf_entry, table, _, _, _, stride, count, *_ = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    if (data[:7], kind, machine, elf_entry, stride, count) != (b'\x7fELF\x02\x01\x01', 2, 183, entry, 56, 3):
        raise ValueError('unexpected fixed AArch64 ELF layout')

    patches = []
    for index, (address, limit, permissions, record_flags) in enumerate(SEGMENTS):
        kind, flags, offset, va, pa, file_size, size, alignment = struct.unpack_from('<II6Q', data, table + index * stride)
        if ((kind, flags, va, pa, alignment) != (1, permissions, address, address, 4096) or
                not 0 < size <= limit or file_size > size or offset + file_size > len(data)):
            raise ValueError('native load segment exceeds its assigned range')
        old = foundation[va - BASE:va - BASE + size]
        if old != bytes(size):
            raise ValueError('native arena slot is occupied')
        patches.append(Patch(IMAGE, record_flags, va, old, data[offset:offset + file_size].ljust(size, b'\0')))

    control = symbols['fx_z8_jpeg_lut_control']
    if not all(patches[0].address <= address < patches[0].address + len(patches[0].new)
               for address in (entry, control, *(symbols[name] for _, name in HOOKS))):
        raise ValueError('native entry outside executable record')
    hook_rows = []
    for site, name in HOOKS:
        call_offset = site - BASE
        original, = struct.unpack('<I', _word_branch(site, JPEG_ENCODER))
        old = struct.pack('<I', original | 0x80000000)
        if stock[call_offset:call_offset + 4] != old or foundation[call_offset:call_offset + 4] != old:
            raise ValueError(f'{name} call preimage differs')
        instruction, = struct.unpack('<I', _word_branch(site, symbols[name]))
        replacement = struct.pack('<I', instruction | 0x80000000)
        patches.append(Patch(HOOK, INSTRUCTION, site, old, replacement, 0))
        hook_rows.append(dict(address=site, before=old.hex(), after=replacement.hex()))

    target = bytes.fromhex(STOCK_SHA256)
    required = arena_profile()['requires_id']
    identity = int.from_bytes(bytes.fromhex(sha(data))[:8], 'little') or 1
    capsule = encode(patches, identity, required, entry)
    component = encode_component(MODULE, 1, SELECTOR, 121,
                                 [dict(mask=SELECTOR, value=SELECTOR, capsule=capsule)], target=target, requires_id=required)
    frontend = output / 'frontend'
    required_sdk = 0 if linked else SDK_REQUIRED
    frontend_source = 'linked_module.c' if linked else 'module.c'
    build_frontend([str(SOURCE / frontend_source), '--id', MODULE, '--target', 'z8-c311', '--required', hex(required_sdk),
                    '--output-dir', str(frontend), '--llvm-bin', str(llvm), '--optimize', 's'])
    name = MODULE + '.wlm'
    bundle = encode_bundle((frontend / name).read_bytes(), MODULE, [component], target)
    parsed = decode_bundle(bundle, module_id=MODULE, target=target)
    if parsed['frontend_metadata']['required'] != required_sdk:
        raise ValueError('JPEG LUT frontend capability mismatch')
    plan, metadata = compose_components([parsed['components'][0]['data']], target=target, requires_id=required)
    if decode(plan)['patches'] != patches or metadata['entries'] != [entry]:
        raise ValueError('composed WLM changed its records or entry point')
    (output / name).write_bytes(bundle)
    (output / (MODULE + '.fnc')).write_bytes(plan)
    report = dict(module=MODULE, wlm_sha256=sha(bundle), wlm_bytes=len(bundle), elf_sha256=sha(data), build_identity=BUILD_ID,
                  simd_audit=simd_audit, peer_variant_scan=peer_scan,
                  rx_extension=dict(start=hex(JPEG_RX_EXTENSION[0]), end=hex(JPEG_RX_EXTENSION[1]),
                                    stock_zero=True, foundation_zero=True),
                  frontend_required=parsed['frontend_metadata']['required'],
                  main_sha256=FOUNDATION, stock_sha256=STOCK_SHA256, plan_sha256=sha(plan), symbols=symbols,
                  hook=hook_rows[0], hooks=hook_rows,
                  segments=[dict(address=p.address, bytes=len(p.new), sha256=sha(p.new)) for p in patches if p.kind == IMAGE],
                  xm6_input='owned_full_frame_snapshot', xm6_capture_deadline_ms=2000,
                  arm_render='borrowed_const_source_to_private_output',
                  arm_core1_offload=dict(views=['MAIN', 'HD'], helper_cores=[2, 3], wait_ticks=1,
                                         fallback_slice_ms=4, fallback_delay_ticks=3) if shared_output else None,
                  arm_max_stripes=3 if shared_output else 1, phase_telemetry=symbols['fx_z8_jpeg_phase_telemetry'],
                  burst_telemetry=symbols.get('fx_z8_jpeg_burst_telemetry'),
                  arm_maintenance_rows=16, capture_chunk_rows=32, capture_cooperative_slice_ms=10,
                  card_cube_operation=5, linked_cube_operation=6, linked=linked,
                  shared_output=shared_output, physical_qualified=False)
    (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report
