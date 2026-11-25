"""Repack WLM v4 native records without changing the frontend or target bytes."""

import argparse
import hashlib
import json
from pathlib import Path

from package_wlm_native_components import locale_block_cuts
from wlm_bundle import decode_bundle, encode_bundle
from wlm_native import chunks_at, decode_component, encode_component
from wlm_record_coalesce import coalesce_patches
from z8_native_capsule import decode as decode_capsule, encode as encode_capsule


def _matches_block_cuts(component, variants, cuts):
    blocks = component['blocks']
    return all(
        [len(part) for part in chunks_at(raw['capsule'], cuts)]
        == [blocks[index][0] for index in original['indices']]
        for raw, original in zip(variants, component['variants'])
    )


def _repack_component(component):
    data = component['data']
    name = component['id']
    before = []
    compacted = []
    counts = []
    for variant in component['variants']:
        raw = decode_component(data, selection=variant['value'])['capsule']
        capsule = decode_capsule(raw)
        if (capsule['stock_main_sha256'] != component['target'].hex()
                or capsule['requires_id'] != component['requires_id']):
            raise ValueError(f'capsule target or dependency differs for {name}')
        patches, stats = coalesce_patches(capsule['patches'])
        selector = dict(mask=variant['mask'], value=variant['value'])
        before.append(dict(selector, capsule=raw))
        compacted.append((selector, patches, capsule['package_id'], capsule['entry']))
        counts.append(dict(selector, before_records=stats['before'], after_records=stats['after']))

    updated = data
    strategy = 'unchanged'
    if any(row['before_records'] != row['after_records'] for row in counts):
        required = component['requires_id']
        after = [dict(selector, capsule=encode_capsule(patches, package_id, required, entry))
                 for selector, patches, package_id, entry in compacted]
        cuts = ()
        strategy = 'content-defined'
        if len(before) > 1 and not _matches_block_cuts(component, before, ()):
            if (name != 'shared.locale' or len(before) != 2
                    or len(before[0]['capsule']) != len(before[1]['capsule'])
                    or not _matches_block_cuts(component, before, locale_block_cuts(before))):
                raise ValueError(f'unsupported shared block cuts for {name}')
            cuts = locale_block_cuts(after)
            strategy = 'locale-shared-runs'
        updated = encode_component(name, component['version'], component['selector_bit'],
                                   component['rank'], after, target=component['target'],
                                   requires_id=required, cut_points=cuts)

    return updated, dict(id=name, before_bytes=len(data), after_bytes=len(updated),
                         before_sha256=component['sha256'], after_sha256=hashlib.sha256(updated).hexdigest(),
                         block_strategy=strategy, variants=counts)


def repack_bundle(data: bytes):
    """Return the new bundle bytes and per-component, per-variant record counts."""
    bundle = decode_bundle(data)
    payloads = []
    reports = []
    for component in bundle['components']:
        payload, report = _repack_component(component)
        payloads.append(payload)
        reports.append(report)
    if any(payload != component['data'] for payload, component in zip(payloads, bundle['components'])):
        data = encode_bundle(bundle['frontend'], bundle['module_id'], payloads, bundle['target'])
    return data, reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    original = args.input.read_bytes()
    updated, components = repack_bundle(original)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as output:
        output.write(updated)
    print(json.dumps(dict(input=str(args.input), output=str(args.output), before_bytes=len(original),
                          after_bytes=len(updated), before_sha256=hashlib.sha256(original).hexdigest(),
                          after_sha256=hashlib.sha256(updated).hexdigest(), components=components), indent=2))


if __name__ == '__main__':
    main()
