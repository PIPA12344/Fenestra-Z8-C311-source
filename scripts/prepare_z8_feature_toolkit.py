"""Verify and unpack the optional, separately licensed Z8 feature dependency."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'vendor/NikonFirmwarePatch-features.lock.json'
OUTPUT = ROOT / 'vendor/NikonFirmwarePatch-features'
PREFIX = 'NikonFirmwarePatch-v0.4.0-rc.2/'
ARCHIVE_BYTES = 8046605
V051_LOCK = ROOT / 'vendor/NikonFirmwarePatch-features-v051.lock.json'
V051_OUTPUT = ROOT / 'vendor/NikonFirmwarePatch-features-v051'
V051_PREFIX = 'NikonFirmwarePatch-v0.5.1/'
V051_ARCHIVE_BYTES = 882762


def sha(data):
    return hashlib.sha256(data).hexdigest()


def prepare(toolkit, *, v051=False):
    lock, output, prefix, size = (V051_LOCK, V051_OUTPUT, V051_PREFIX, V051_ARCHIVE_BYTES) if v051 else (LOCK, OUTPUT, PREFIX, ARCHIVE_BYTES)
    lock_data = lock.read_bytes()
    metadata = json.loads(lock_data)
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    with toolkit.open('rb') as stream:
        data = stream.read(size + 1)
    if len(data) != size or sha(data) != metadata['release_toolkit_sha256']:
        raise ValueError('upstream toolkit length or SHA-256 differs')
    with ZipFile(io.BytesIO(data)) as archive, tempfile.TemporaryDirectory(dir=output.parent) as temp:
        staged = Path(temp) / output.name
        for name, digest in metadata['files'].items():
            relative = PurePosixPath(name)
            if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name:
                raise ValueError(f'unsafe dependency path {name!r}')
            payload = archive.read(prefix + name)
            if sha(payload) != digest:
                raise ValueError(f'upstream toolkit member differs: {name}')
            path = staged / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
        if v051:
            (staged / 'FENESTRA-PROVENANCE.json').write_bytes(lock_data)
        staged.rename(output)
    return len(metadata['files'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('toolkit', type=Path)
    parser.add_argument('--v051', action='store_true', help='install the v0.5.1 toolkit')
    args = parser.parse_args()
    count = prepare(args.toolkit, v051=args.v051)
    output = V051_OUTPUT if args.v051 else OUTPUT
    print(f'Installed {count} verified files in {output.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
