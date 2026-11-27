"""Verify a release's exact WLMs using the production C composer and frozen scanner; no device access."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'host'), str(ROOT / 'tests')]
from toolchain import host_compiler
from wlm_bundle import decode_bundle
from wlm_native import compose_components, decode_component
from z8_native_capsule import decode, IMAGE


def verify(wlms, manifest, output, foundation=None):
    output.mkdir(parents=True, exist_ok=True)
    target = bytes.fromhex(manifest['stock_main_sha256'])
    required = int(manifest['requires_id'], 0)
    components, paths = [], []
    groups = {'v051-five': [], 'companion': []}
    for row in manifest['plugins']:
        path = wlms / Path(row['file']).name
        data = path.read_bytes()
        if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError('release identity differs: ' + path.name)
        paths.append(path.resolve())
        bundle = decode_bundle(data, module_id=path.stem, target=target)
        for component in bundle['components']:
            dest = output / f'{path.stem}-{len(components)}.wnc'
            dest.write_bytes(component['data'])
            components.append(dest)
            groups[row['upgrade_group']].append(component['data'])
    reference, metadata = compose_components([p.read_bytes() for p in components], target=target, requires_id=required)
    patches = decode(reference)['patches']
    page_counts = Counter(page for p in patches if p.kind == IMAGE
                          for page in range(p.address // 4096, (p.address + len(p.new) + 4095) // 4096))
    pages = {page for p in patches for page in range(p.address // 4096, (p.address + len(p.new) + 4095) // 4096)}
    group_pages = {}
    for name, inputs in groups.items():
        addresses = set()
        for component in inputs:
            selected = decode_component(component, target=target, requires_id=required, selection=metadata['selection'])
            for patch in decode(selected['capsule'])['patches']:
                if patch.kind == IMAGE:
                    addresses.update(range(patch.address // 4096, (patch.address + len(patch.new) + 4095) // 4096))
        group_pages[name] = addresses
    overlaps = len(group_pages['v051-five'] & group_pages['companion'])
    if overlaps:
        raise ValueError(f'{overlaps} overlapping IMAGE pages')
    flags = ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wshadow',
             '-Wno-misleading-indentation', '-I' + str(ROOT / 'include')]
    def compile_driver(name, sources, extra=()):
        path = output / (name + ('.exe' if os.name == 'nt' else ''))
        subprocess.run([host_compiler('clang'), *flags, *extra,
                        *(str(ROOT / s) for s in sources), '-o', str(path)],
                       check=True, capture_output=True, timeout=60)
        return path
    driver = compile_driver('compose', ('tests/test_wlm_native_driver.c', 'src/wlm_native.c',
                                       'src/memory.c', 'src/sha256.c'))
    plan = output / 'production.fnc'
    actual = subprocess.run([str(driver), '0', str(plan), target.hex(), f'{required:x}',
                             *map(str, components)], check=True, capture_output=True, text=True, timeout=180)
    stats = json.loads(actual.stdout)
    assert stats['result'] == 0 and stats['live'] == 0 and plan.read_bytes() == reference
    cold = compile_driver('cold-scan', ('tests/test_z8_wlm_cold_scan.c', 'src/wlm_bundle.c', 'src/wlm_native.c',
                                       'src/module.c', 'src/services.c', 'src/sha256.c', 'src/memory.c'),
                          ('-ffunction-sections', '-fdata-sections', '-fuse-ld=lld', '-Wl,/OPT:REF')
                          if os.name == 'nt' else ('-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections'))
    scans = []
    for fit in ('first-fit', 'best-fit'):
        scanned = output / f'{fit}.fnc'
        p = subprocess.run([str(cold), '7185013', str(scanned), target.hex(), f'{required:x}', fit,
                            *map(str, paths)], check=True, capture_output=True, text=True, timeout=180)
        row = json.loads(p.stdout)
        assert row['scan_result'] == row['compose_result'] == row['seal_result'] == 0
        assert row['scan_files'] == len(paths) and scanned.read_bytes() == reference
        scans.append(row)
    preimages = None
    if foundation:
        from build_z8_photo_menu import verify_plan_preimages
        image = foundation.read_bytes()
        assert hashlib.sha256(image).hexdigest() == manifest['foundation_main_sha256']
        preimages = verify_plan_preimages(reference, image)
    report = dict(schema='fenestra.z8-wlm-release-verification/v1', no_camera=True,
                  exact_plugin_hashes=True, plugin_count=len(paths), input_component_views=len(components),
                  records=len(patches), pages=len(pages), image_pages=len(page_counts),
                  overlapping_image_pages=overlaps,
                  overlap_scope='v051-five IMAGE pages versus companion IMAGE pages',
                  page_count_scope='all patch target pages; excludes the loader-owned revocation page',
                  composition_sha256=hashlib.sha256(reference).hexdigest(), production_c=stats,
                  python_bytes_equal=True, foundation_preimages_verified=preimages, cold_scan=scans)
    (output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wlms', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--foundation', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.wlms.resolve(), json.loads(args.manifest.read_bytes()),
                            args.output.resolve(), args.foundation), indent=2))
