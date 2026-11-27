"""Verify or refresh the source inventory of this local Z8 adaptation; never accesses devices.

  python verify_z8_source.py             every listed file matches, and every git-tracked file is listed
  python verify_z8_source.py --update    re-hash listed files, add newly tracked files, drop removed ones
  python verify_z8_source.py --upstream  how this tree differs from the upstream SOURCE-INVENTORY.json snapshot
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
INVENTORY = 'Z8-PORT-INVENTORY.json'
UPSTREAM = 'SOURCE-INVENTORY.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load(root):
    return json.loads((root / INVENTORY).read_text(encoding='utf-8'))


def tracked(root):
    """Names in the git index, or None when this tree is not its own checkout (a source ZIP)."""
    try:
        top = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=root, capture_output=True)
        if top.returncode or Path(top.stdout.decode('utf-8').strip()).resolve() != root.resolve():
            return None
        listing = subprocess.run(['git', 'ls-files', '-z', '--cached'], cwd=root, capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return {name for name in listing.stdout.decode('utf-8').split('\0') if name} - {INVENTORY}


def differences(root, manifest):
    """Listed files whose bytes are missing, outside the tree or different."""
    found = []
    for name, expected in manifest['files'].items():
        path = (root / name).resolve()
        if not path.is_relative_to(root.resolve()):
            found.append('outside the project: ' + name)
        elif not path.is_file():
            found.append('missing: ' + name)
        elif digest(path.read_bytes()) != expected:
            found.append('source identity differs: ' + name)
    return found


def verify(root=ROOT):
    manifest = load(root)
    found = differences(root, manifest)
    names = tracked(root)
    if names is not None:
        found += ['tracked but not listed: ' + name for name in sorted(names - set(manifest['files']))]
        found += ['listed but not tracked: ' + name for name in sorted(set(manifest['files']) - names)]
    if found:
        raise ValueError('\n'.join([*found, f'{len(found)} inventory problem(s); after an intended change run: '
                                            'python verify_z8_source.py --update']))
    return len(manifest['files'])


def update(root=ROOT):
    """Rewrite the file hashes from the working tree. Existing order is kept so the diff shows only real changes."""
    names = tracked(root)
    if names is None:
        raise ValueError('--update needs the git checkout of this project: the index decides which files are source')
    gone = sorted(name for name in names if not (root / name).is_file())
    if gone:
        raise ValueError('tracked but missing from the working tree (restore or git rm): ' + ', '.join(gone))
    manifest = load(root)
    old = manifest['files']
    current = {name: digest((root / name).read_bytes()) for name in names}
    files = {name: current[name] for name in old if name in current}
    added = sorted(names - set(old))
    files.update((name, current[name]) for name in added)
    report = dict(changed=sorted(name for name in files if name in old and old[name] != files[name]),
                  added=added, removed=sorted(set(old) - names))
    if any(report.values()):
        manifest['files'] = files
        (root / INVENTORY).write_bytes((json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8'))
    return report


def upstream(root=ROOT):
    """Classify each upstream file: untouched, line endings only (CRLF checkout), or changed by the port."""
    rows = json.loads((root / UPSTREAM).read_text(encoding='utf-8'))['files']
    report = dict(identical=[], line_endings_only=[], modified=[], missing=[])
    for row in rows:
        path = root / row['path']
        if not path.is_file():
            report['missing'].append(row['path'])
            continue
        data = path.read_bytes()
        if digest(data) == row['sha256']:
            report['identical'].append(row['path'])
        elif digest(data.replace(b'\r\n', b'\n')) == row['sha256']:
            report['line_endings_only'].append(row['path'])
        else:
            report['modified'].append(row['path'])
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--update', action='store_true', help='refresh Z8-PORT-INVENTORY.json from the git-tracked working tree')
    mode.add_argument('--upstream', action='store_true', help='report drift from the upstream snapshot; changes nothing')
    args = parser.parse_args(argv)
    if args.upstream:
        report = upstream()
        print(json.dumps({key: len(value) for key, value in report.items()}))
        for key in ('modified', 'missing'):
            for name in report[key]:
                print(key + ': ' + name)
        return 0
    if args.update:
        report = update()
        for key, names in report.items():
            for name in names:
                print(key + ': ' + name)
        print('inventory ' + ('updated' if any(report.values()) else 'already current'))
    try:
        count = verify()
    except ValueError as exc:
        print(exc)
        return 1
    print('PASS: ' + str(count) + ' source files; complete Z8 port inventory')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
