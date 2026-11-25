"""Lossless coalescing of one Z8 C3.11 native capsule's destination records."""

from collections.abc import Sequence
from dataclasses import replace
from pathlib import Path
import re

from z8_native_arena import RESOURCE_ROOTS
from z8_native_capsule import HOOK, IMAGE, INSTRUCTION, NONE, POINTER, Patch
from z8_ui_profile import PROFILE


_ROOT = Path(__file__).resolve().parents[1]
_PLATFORM = _ROOT / 'src/platform/z8_c311_full'
_NATIVE_C = (_PLATFORM / 'native_capsules.c').read_text(encoding='utf-8')


def _constant(source: str, name: str) -> str:
    definition = re.search(rf'^#define\s+{name}\s+([^\n]+)', source, re.MULTILINE)
    if definition is None:
        raise ValueError(f'missing native policy constant {name}')
    return definition[1].strip()


def _unsigned(expression: str) -> int:
    literal = re.fullmatch(r'(?:UINT64_C\((0x[\da-fA-F]+|\d+)\)|(0x[\da-fA-F]+|\d+)u?)', expression)
    if literal is None:
        raise ValueError(f'unsupported native policy literal {expression!r}')
    return int(literal[1] or literal[2], 0)


PAGE_SIZE = _unsigned(_constant(_NATIVE_C, 'PAGE'))
ARENA_BEGIN = _unsigned(_constant(_NATIVE_C, 'ARENA_BEGIN'))
ARENA_END = _unsigned(_constant(_NATIVE_C, 'ARENA_END'))
CODE_BEGIN = _unsigned(_constant(_NATIVE_C, 'CODE_BEGIN'))
CODE_END = _unsigned(_constant(_NATIVE_C, 'CODE_END'))
STOCK_END = _unsigned(_constant(_NATIVE_C, 'STOCK_END'))
INIT_BEGIN = _unsigned(_constant(_NATIVE_C, 'INIT_BEGIN'))
_INIT_END = re.fullmatch(r'\(INIT_BEGIN\s*\+\s*(UINT64_C\([^)]+\))\)', _constant(_NATIVE_C, 'INIT_END'))
if _INIT_END is None:
    raise ValueError('unsupported native policy INIT_END definition')
INIT_END = INIT_BEGIN + _unsigned(_INIT_END[1])
REVOKED_PAGE = _unsigned(_constant((_PLATFORM / 'revocation.h').read_text(encoding='utf-8'), 'FX_Z8_REVOKED_PAGE'))
_CAPSULE_H = (_ROOT / 'include/fenestra_capsule.h').read_text(encoding='utf-8')
_MAX_RECORD = re.fullmatch(r'\((\d+u)\s*\*\s*(\d+u)\)', _constant(_CAPSULE_H, 'FX_CAPSULE_MAX_RECORD'))
if _MAX_RECORD is None:
    raise ValueError('unsupported native policy FX_CAPSULE_MAX_RECORD definition')
MAX_RECORD = _unsigned(_MAX_RECORD[1]) * _unsigned(_MAX_RECORD[2])

_ADMIT_C = _NATIVE_C.split('static FxResult admit(', 1)[1].split('static uint32_t survives(', 1)[0]
_CORE_ROOTS_C = re.search(r'core_roots\[\]\s*=\s*\{(.*?)\};', _ADMIT_C, re.DOTALL)
_COPY_WINDOW_C = re.search(
    r'r->address\s*<\s*(UINT64_C\([^)]+\))\s*&&\s*r->address\+r->bytes\s*>\s*(UINT64_C\([^)]+\))',
    _ADMIT_C,
)
if _CORE_ROOTS_C is None or _COPY_WINDOW_C is None:
    raise ValueError('missing protected native policy windows in admit()')
# The same checked C311 UI address mapping supplies z8_full_bindings.h.
_CORE_ROOTS = tuple(
    (PROFILE['address_map']['0x' + reference_address], _unsigned(size))
    for reference_address, size in re.findall(r'\{FX_Z8_ADDR_([\da-f]+)\s*,\s*(\d+u)\}', _CORE_ROOTS_C[1])
)
if _CORE_ROOTS_C[1].count('{') != len(_CORE_ROOTS):
    raise ValueError('unsupported native policy core_roots entry')
_COPY_WINDOW = (_unsigned(_COPY_WINDOW_C[2]), _unsigned(_COPY_WINDOW_C[1]))
_RESOURCE_ROOTS = frozenset(
    ({'pointer': POINTER}[root['record_kind']], int(root['address'], 0), root['bytes'])
    for root in RESOURCE_ROOTS
)
_RESOURCE_EXTENTS = frozenset((address, size) for _, address, size in _RESOURCE_ROOTS)
_HOOK_REGIONS = (
    ('code-before-arena', CODE_BEGIN, ARENA_BEGIN),
    ('code-after-arena', ARENA_END, CODE_END),
    ('stock-rodata', CODE_END, STOCK_END),
    ('init', INIT_BEGIN, INIT_END),
)


def _within(address: int, size: int, begin: int, end: int) -> bool:
    return begin <= address < end and size <= end - address


def _admit(kind: int, flags: int, address: int, size: int) -> bool:
    end = address + size
    if any(address < root + width and end > root for root, width in _CORE_ROOTS):
        return False
    if address < _COPY_WINDOW[1] and end > _COPY_WINDOW[0]:
        return False
    if address < ARENA_END and end > REVOKED_PAGE:
        return False
    if kind == IMAGE:
        return _within(address, size, ARENA_BEGIN, ARENA_END)
    if kind == HOOK and (flags, address, size) in _RESOURCE_ROOTS:
        return True
    return (_within(address, size, CODE_BEGIN, ARENA_BEGIN)
            or _within(address, size, ARENA_END, STOCK_END)
            or (flags != INSTRUCTION and _within(address, size, INIT_BEGIN, INIT_END)))


def admit(patch: Patch) -> bool:
    """Mirror the platform's admit() callback after capsule format checks."""
    return _admit(patch.kind, patch.flags, patch.address, len(patch.new))


def _survives(address: int, size: int) -> bool:
    return (_within(address, size, CODE_BEGIN, ARENA_BEGIN)
            or _within(address, size, ARENA_END, CODE_END)
            or (address, size) in _RESOURCE_EXTENTS)


def survives(patch: Patch) -> bool:
    """Report whether a HOOK is revoked when retained RAM survives shutdown."""
    return patch.kind == HOOK and _survives(patch.address, len(patch.new))


def is_resource_root(patch: Patch) -> bool:
    return patch.kind == HOOK and (patch.flags, patch.address, len(patch.new)) in _RESOURCE_ROOTS


def _region(kind: int, flags: int, address: int, size: int) -> str | None:
    if kind == IMAGE:
        return 'arena' if _within(address, size, ARENA_BEGIN, ARENA_END) else None
    if kind == HOOK:
        if (flags, address, size) in _RESOURCE_ROOTS:
            return 'resource-root'
        for name, begin, end in _HOOK_REGIONS:
            if _within(address, size, begin, end):
                return name
    return None


def record_region(patch: Patch) -> str | None:
    """Return the single policy interval containing the entire record, if any."""
    return _region(patch.kind, patch.flags, patch.address, len(patch.new))


def _merge_policy(kind: int, flags: int, address: int, size: int) -> tuple[str, bool] | None:
    if not 0 < size <= MAX_RECORD:
        return None
    alignment = {INSTRUCTION: 4, POINTER: 8}.get(flags, 1) if kind == HOOK else 1
    if (address | size) & (alignment - 1):
        return None
    region = _region(kind, flags, address, size)
    if region in (None, 'resource-root') or not _admit(kind, flags, address, size):
        return None
    return region, kind == HOOK and _survives(address, size)


def recompute_dependencies(patches: Sequence[Patch]) -> list[Patch]:
    """Assign earlier IMAGE indices as the capsule and WLM composers do."""
    result = []
    images = []
    for index, patch in enumerate(patches):
        target = None
        if patch.kind == IMAGE:
            images.append((index, patch.address, patch.address + len(patch.new)))
        elif patch.kind == HOOK:
            if patch.flags == INSTRUCTION and len(patch.new) == 4:
                word = int.from_bytes(patch.new, 'little')
                if word & 0x7c000000 == 0x14000000:  # AArch64 B or BL immediate.
                    immediate = (word & 0x3ffffff) - ((word >> 25 & 1) << 26)
                    target = patch.address + immediate * 4
            elif patch.flags == POINTER and len(patch.new) == 8:
                target = int.from_bytes(patch.new, 'little')
        dependency = NONE
        if target is not None:
            dependency = next((i for i, begin, end in images if begin <= target < end), NONE)
        result.append(replace(patch, depends=dependency) if patch.depends != dependency else patch)
    return result


def _join(run: list[Patch]) -> Patch:
    first = run[0]
    if len(run) == 1:
        return first
    return Patch(first.kind, first.flags, first.address,
                 b''.join(patch.old for patch in run), b''.join(patch.new for patch in run))


def coalesce_patches(patches: Sequence[Patch]) -> tuple[list[Patch], dict[str, int]]:
    """Merge strictly adjacent eligible records within one component variant.

    Consider candidates in (kind, flags, address) order. Return capsule order
    (kind, address) with all dependency indices recomputed for the new table.
    """
    result = []
    run = []
    run_end = 0
    policy = None
    for patch in sorted(patches, key=lambda p: (p.kind, p.flags, p.address)):
        size = len(patch.new)
        end = patch.address + size
        current = _merge_policy(patch.kind, patch.flags, patch.address, size) if len(patch.old) == size else None
        if run:
            first = run[0]
            if (policy is not None and current == policy and patch.address == run_end
                    and (patch.kind, patch.flags) == (first.kind, first.flags)
                    and _merge_policy(first.kind, first.flags, first.address, end - first.address) == policy):
                run.append(patch)
                run_end = end
                continue
            result.append(_join(run))
        run = [patch]
        run_end = end
        policy = current
    if run:
        result.append(_join(run))
    result.sort(key=lambda p: (p.kind, p.address))
    result = recompute_dependencies(result)
    return result, dict(before=len(patches), after=len(result), merged=len(patches) - len(result))
