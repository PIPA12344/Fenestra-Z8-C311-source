"""Pinned v0.5.1 feature inputs and isolated access to upstream Python modules."""

import binascii
from functools import cache
import hashlib
import importlib
import json
import pickle
from pathlib import Path
import subprocess
import sys


HOST = Path(__file__).resolve().parent
VENDOR = HOST.parent / 'vendor/NikonFirmwarePatch-features-v051'
SOURCE_COMMIT = '25fefa1e17f321ee285bea77b3c54427f9912e4b'
PROVENANCE_SHA256 = '74787fbf4724fc2802b397750d40ed3dba22236b68882c39db9cfedbabd7306a'
COMPILED_SHA256 = 'b25fb25e0486215966ca7229e66e40d0d1af3e9b9eb37328c8b9bc9271d9c775'
COMBINATIONS_SHA256 = '43afb8ec95b202faa0ec0b7f44f235a762cd4bf7ed482e7040c6b32cf592aeaf'
FEATURES = ('video-recording', 'log-colour', 'still-photo-tools', 'raw-precapture')
_WORKER_ACTIVE = False


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _pinned_json(path, expected):
    data = (VENDOR / path).read_bytes()
    if _sha(data) != expected:
        raise ValueError(f'v0.5.1 upstream digest differs: {path}')
    return json.loads(data)


@cache
def _verify_snapshot():
    provenance = _pinned_json('FENESTRA-PROVENANCE.json', PROVENANCE_SHA256)
    if (provenance['source_commit'] != SOURCE_COMMIT or
            provenance['compiled_metadata_sha256'] != COMPILED_SHA256 or
            provenance['combinations_sha256'] != COMBINATIONS_SHA256):
        raise ValueError('v0.5.1 upstream source pins differ')
    for name, digest in provenance['files'].items():
        source = (VENDOR / name).resolve(strict=True)
        if not source.is_relative_to(VENDOR) or _sha(source.read_bytes()) != digest:
            raise ValueError('v0.5.1 upstream file differs: ' + name)
    return provenance


def compiled_metadata():
    """Read the symbol and binary manifest without importing upstream code."""
    return _pinned_json('resources/compiled.json', COMPILED_SHA256)


def checked_upstream():
    """Return the verified provenance inside a v0.5.1 recipe process."""
    if not _WORKER_ACTIVE:
        raise RuntimeError('use run_isolated() to import v0.5.1 upstream modules')
    return _verify_snapshot()


def _invoke(function, args, kwargs):
    module_name, separator, attribute = function.partition(':')
    if (not separator or not attribute.isidentifier() or
            not all(part.isidentifier() for part in module_name.split('.'))):
        raise ValueError('expected an importable host module:function')
    module = importlib.import_module(module_name)
    if not Path(module.__file__).resolve().is_relative_to(HOST):
        raise ValueError('v0.5.1 worker entry point is outside host')
    return getattr(module, attribute)(*args, **kwargs)


def run_isolated(function, *args, **kwargs):
    """Run a trusted host producer with only the v0.5.1 upstream imports.

    Pass an importable host function name and picklable arguments. Call at the
    outermost build loop to keep stock images and intermediate data in one
    process. Nested calls reuse that process.
    """
    if _WORKER_ACTIVE:
        return _invoke(function, args, kwargs)
    request = pickle.dumps((function, args, kwargs), protocol=pickle.HIGHEST_PROTOCOL)
    worker = subprocess.run([sys.executable, '-I', '-B', str(Path(__file__).resolve())],
                            input=request, capture_output=True, check=False)
    if worker.returncode:
        detail = worker.stderr.decode('utf-8', errors='replace').rstrip()
        raise RuntimeError(f'v0.5.1 recipe worker failed ({worker.returncode})\n{detail}')
    return pickle.loads(worker.stdout)


def _compose(stock, features):
    from features import compose as upstream_compose

    eg = stock + binascii.crc_hqx(stock, 0).to_bytes(2, 'big')
    image, detail, rows = upstream_compose(eg, list(features), code_source='prebuilt')
    return bytes(image.new[:-2]), detail, rows


def compose(stock, features=FEATURES):
    """Reproduce the reviewed upstream feature-group composition for comparison."""
    return run_isolated('z8_feature_suite_v051:_compose', stock, features)


def _worker_main():
    global _WORKER_ACTIVE
    _verify_snapshot()
    # A host module can import the same adapter after this script has started.
    sys.modules['z8_feature_suite_v051'] = sys.modules[__name__]
    sys.modules['host.z8_feature_suite_v051'] = sys.modules[__name__]
    sys.path[:0] = [str(VENDOR), str(HOST), str(HOST.parent)]
    _WORKER_ACTIVE = True
    result_output = sys.stdout.buffer
    # Keep stdout available for the result even when build tools print progress.
    sys.stdout = sys.stderr
    function, args, kwargs = pickle.load(sys.stdin.buffer)
    result = _invoke(function, args, kwargs)
    pickle.dump(result, result_output, protocol=pickle.HIGHEST_PROTOCOL)


if __name__ == '__main__':
    _worker_main()
