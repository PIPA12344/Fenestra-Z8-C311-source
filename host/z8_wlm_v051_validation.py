"""Offline admission, preimage and page checks for the cold-retry3 WLM port."""

from bisect import bisect_left
import json
from pathlib import Path

from z8_native_arena import ARENA_BEGIN, ARENA_END, BASE, COPY_END_NEW, COPY_END_SITE
from z8_native_arena import INIT_FILE_BEGIN, INIT_FILE_END, INIT_RAM_BEGIN, arena_profile, verify_arena
from z8_native_capsule import DATA, HOOK, IMAGE, INSTRUCTION, NONE, POINTER, Patch, decode, sha
from wlm_bundle import decode_bundle
from wlm_native import decode_component

ROOT = Path(__file__).resolve().parents[1]
FOUNDATION = ROOT / 'build/cold-retry-20261107/foundation-3/z8-core.main'
FOUNDATION_SHA = '0e0114267ef82caf78b7795a470192b434528ec0f4a8a6b031d1a003c4c63dfd'
CORE_ID = 'f06231d84970cda74ff6afcdb2aad7d03f6fe0bc4a3ec76c15a9cfd40654579a'
CURRENT_MANIFEST_SHA = '263682a97113d3064c4fa34e733934e6377eb1fcdf6632b71bc205c6974a9d14'
PAGE = 4096
REVOKED = 0x40BFF000
COUNT_ADDRESS = 0xA0281CD5C
TEXT_COUNT = 3251
TEXT_ACCEPTED = (3136, 8192)
# cold-retry3 fx_z8_capsule_locale_stage: unsigned range check on d.text_count.
TEXT_RANGE_SITE = 0x401E77AC
TEXT_RANGE_CODE = bytes.fromhex('0a00841229010a0b0a7882123f010a6b83fcff54')
PEERS = (
    'dist/z8-card-wav-20261107/card/FENESTRA/card_wav.wlm',
    'build/z8-portrait-video-20261124/r4/portrait_video.wlm',
    'build/z8-portrait-skin-flex-r1-20261112/card/FENESTRA/portrait_skin.wlm',
    'build/z8-photo-menu-hsfc-gate-20261123/jpeg/jpeg_lut.wlm',
    'build/lut-preview-r2-publication-20261123/assets/lut_preview.wlm',
)
PEER_COMPONENTS = {'card_wav', 'portrait_video', 'portrait_skin', 'jpeg_lut',
                   'lut_preview', 'z8.photo.menu', 'shared.fxlog'}
# Treat whole peer slots as occupied even if a particular variant uses less.
PEER_ARENA = (
    (0x40940000, 0x40950000, 'card_wav'),
    (0x40B60000, 0x40B80000, 'z8.photo.menu'),
    (0x40BCF000, 0x40BD0000, 'fxlog'),
    (0x40BE0000, 0x40BE6000, 'portrait_video'),
    (0x40BE6000, 0x40BF0000, 'jpeg_lut'),
    (0x40BF0000, REVOKED, 'lut_preview'),
    (REVOKED, ARENA_END, 'loader revocation'),
)
CORE_CELLS = ((COUNT_ADDRESS, 4), (0xA02F2AC40, 8), (0xA02F2AC50, 8), (COPY_END_SITE, 8))


def image_bytes(main, address, count):
    """Reverse the checked MAIN to initialized-RAM translation."""
    if INIT_RAM_BEGIN <= address and address + count <= INIT_RAM_BEGIN + INIT_FILE_END - INIT_FILE_BEGIN:
        offset = INIT_FILE_BEGIN + address - INIT_RAM_BEGIN
    elif (BASE <= address and address + count <= BASE + len(main)
          and (address + count <= BASE + INIT_FILE_BEGIN or address >= BASE + INIT_FILE_END)):
        offset = address - BASE
    else:
        raise ValueError(f'no MAIN preimage at {address:#x}+{count:#x}')
    return main[offset:offset + count]


def foundation_gate(stock, path=FOUNDATION):
    verify_arena(stock)
    path = Path(path)
    main = path.read_bytes()
    if sha(main) != FOUNDATION_SHA or bytes.fromhex(CORE_ID) not in main:
        raise ValueError('exact cold-retry3 MAIN and its embedded build identity are required')
    if image_bytes(main, COUNT_ADDRESS, 4) != b'\xff\xff\x00\x00':
        raise ValueError('unexpected static native text count sentinel')
    if (image_bytes(main, TEXT_RANGE_SITE, len(TEXT_RANGE_CODE)) != TEXT_RANGE_CODE
            or not TEXT_ACCEPTED[0] <= TEXT_COUNT <= TEXT_ACCEPTED[1]):
        raise ValueError('cold-retry3 native locale descriptor limit differs')
    if image_bytes(main, COPY_END_SITE, 8) != COPY_END_NEW:
        raise ValueError('cold-retry3 loader arena reservation differs')
    if any(image_bytes(main, ARENA_BEGIN, ARENA_END - ARENA_BEGIN)):
        raise ValueError('cold-retry3 arena is not empty')
    return main


def read_peers(target):
    return {
        path: decode_bundle((ROOT / path).read_bytes(), module_id=Path(path).stem, target=target)
        for path in PEERS
    }


def peer_destinations(peers):
    destinations = [(begin, end) for begin, end, _ in PEER_ARENA]
    seen = set()
    for bundle in peers.values():
        for component in bundle['components']:
            if component['sha256'] in seen:
                continue
            seen.add(component['sha256'])
            for variant in component['variants']:
                raw = decode_component(component['data'], selection=variant['value'])['capsule']
                destinations.extend((p.address, p.address + len(p.new)) for p in decode(raw)['patches'])
    return destinations


def validate_record(patch, stock, foundation, *, candidate):
    address = patch.address
    size = len(patch.new)
    end = address + size
    if any(address < cell + width and cell < end for cell, width in CORE_CELLS):
        raise ValueError(f'WLM record touches cold-retry3 core cell at {address:#x}')
    if address < ARENA_END and REVOKED < end:
        raise ValueError(f'WLM record touches loader revocation page at {address:#x}')
    if patch.kind == IMAGE:
        if not ARENA_BEGIN <= address < end <= ARENA_END:
            raise ValueError(f'IMAGE escapes native arena at {address:#x}')
    else:
        root = arena_profile()['resource_roots'][0]
        if (address, size, patch.flags) == (int(root['address'], 0), root['bytes'], POINTER):
            return  # Exact cold-bootstrap resource-root supplement checks its runtime preimage.
        admitted = (0x40200000 <= address < end <= ARENA_BEGIN
                    or ARENA_END <= address < end <= 0x44F6495D
                    or (INIT_RAM_BEGIN <= address < end <= INIT_RAM_BEGIN + INIT_FILE_END - INIT_FILE_BEGIN
                        and patch.flags != INSTRUCTION))
        if not admitted:
            raise ValueError(f'HOOK outside cold-retry3 policy at {address:#x}+{size:#x}')
    if patch.old != image_bytes(foundation, address, size):
        raise ValueError(f'cold-retry3 preimage differs at {address:#x}+{size:#x}')
    if candidate and patch.old != image_bytes(stock, address, size):
        raise ValueError(f'stock MAIN preimage differs at {address:#x}+{size:#x}')


def coalesce(patches, stock, foundation, blocked):
    """Join small same-owner runs within a page, preserving stock gap bytes."""
    spans = []
    for begin, end in sorted(blocked):
        if spans and begin <= spans[-1][1]:
            spans[-1] = (spans[-1][0], max(end, spans[-1][1]))
        else:
            spans.append((begin, end))
    starts = [start for start, _ in spans]
    result = []
    for patch in sorted(patches, key=lambda p: p.address):
        if result:
            previous = result[-1]
            end = previous.address + len(previous.new)
            gap = patch.address - end
            stop = patch.address + len(patch.new)
            neighbor = bisect_left(starts, patch.address) - 1
            available = not gap or neighbor < 0 or spans[neighbor][1] <= end
            if (previous.kind == patch.kind and previous.flags == patch.flags
                    and (patch.kind == IMAGE or (patch.kind == HOOK and patch.flags == DATA))
                    and previous.depends == patch.depends == NONE and 0 <= gap <= 64
                    and previous.address // PAGE == (stop - 1) // PAGE and available):
                preimage = image_bytes(stock, end, gap) if gap else b''
                if not gap or preimage == image_bytes(foundation, end, gap):
                    result[-1] = Patch(patch.kind, patch.flags, previous.address,
                                       previous.old + preimage + patch.old,
                                       previous.new + preimage + patch.new)
                    continue
        result.append(patch)
    return sorted(result, key=lambda p: (p.kind, p.address))


def patch_pages(patch):
    last = patch.address + len(patch.new) - 1
    return range(patch.address & -PAGE, (last & -PAGE) + PAGE, PAGE)


def budget(patches, fnc_bytes):
    """Count each virtual page as cold-retry3 does, including its revoked page."""
    pages = {REVOKED}
    flags = {}
    for patch in patches:
        for page in patch_pages(patch):
            if page == REVOKED:
                raise ValueError('WLM record occupies loader revocation page')
            pages.add(page)
            if patch.kind == IMAGE:
                old = flags.setdefault(page, patch.flags)
                if old != patch.flags:
                    raise ValueError(f'RX/RO/RW conflict on arena page {page:#x}')
    writes = sum(len(p.new) for p in patches)
    if len(patches) >= 1024 or len(pages) >= 1536 or writes > 8 << 20 or fnc_bytes > 16 << 20:
        raise ValueError(f'cold-retry3 budget: {len(patches)} records, {len(pages)} pages, '
                         f'{writes} writes, {fnc_bytes} FNC bytes')
    return dict(records=len(patches), pages=len(pages), write_bytes=writes, fnc_bytes=fnc_bytes)


def check_current_delivery():
    manifest = ROOT / 'dist/current/MANIFEST.json'
    data = manifest.read_bytes()
    if sha(data) != CURRENT_MANIFEST_SHA:
        raise ValueError('dist/current/MANIFEST.json SHA-256 differs')
    rows = json.loads(data)['files']
    for name, metadata in rows.items():
        blob = (manifest.parent / name).read_bytes()
        if len(blob) != metadata['bytes'] or sha(blob) != metadata['sha256']:
            raise ValueError(f'dist/current file differs: {name}')
    return dict(manifest_sha256=sha(data), files=len(rows))
