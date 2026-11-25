"""Photo Tools v0.5.1 recipe for the cold-retry3 arena."""
from functools import lru_cache
from hashlib import sha256
from importlib import import_module, util
from pathlib import Path
import os
import re
import subprocess
import tempfile

from wlm_bundle import decode_bundle
from wlm_native import decode_component
from z8_feature_suite_v051 import VENDOR, checked_upstream, compiled_metadata
from z8_native_arena import ARENA_BEGIN, ARENA_END, BASE, COPY_END_SITE
from z8_native_arena import INIT_FILE_BEGIN, INIT_FILE_END, INIT_RAM_BEGIN
from z8_native_capsule import EXEC, IMAGE, decode


ROOT = Path(__file__).resolve().parents[1]
R10_RELEASE = ROOT / "dist/photo-tools-r10-c1-20261120/photo_tools.wlm"
R10_SHA256 = "aaefcb935612b0aff2a175d19aaca4c08265095add3c8c854917fca9d6eb9b0a"
R10_CAPTURE_SHA256 = "6151e2ead32a3f37a37c4be53af908b9a59cfd62a4ee2ceaed5230752466ecc1"
R10_CONTROL = {0x40920000, 0x40923000}
PIXELSHIFT_RECORDS = frozenset((
    0x405D9714, 0x40990000, 0x409D8000, 0x409DA000, 0x409EC800,
    0x43DCFF4C, 0x43E27BE0, 0x43E27C00, 0x43E27C60, 0x43E27CA0,
    0xA028CFA40, 0xA028E6670, 0xA028E6690, 0xA028E6850, 0xA028E8380, 0xA028F57F0,
))
RAW_MENU = 0x409FC000
HEIF_RX = 0x40BA0000
HEIF_MENU = 0x40BA2000
AREA_RX = 0x40BA4000
AREA_GEOMETRY = 0x40BA7000
AREA_MENU_CODE = 0x40BA8000
AREA_ICONS_RO = 0x40BA9000
AREA_QUICK_CODE = 0x40BAA000
AREA_THUMB_CODE = AREA_QUICK_CODE + 0x800
AREA_QUICK_GLUE = AREA_QUICK_CODE + 0xC00

PHOTO_CODE_RANGES = (
    (0x40990000, 0x409B0000), (0x409D8000, 0x409D9000),
    (0x409F0000, 0x409FB000), (RAW_MENU, RAW_MENU + 0x1000),
    (0x40B82000, 0x40B83000), (0x40B84000, 0x40B85000),
    (HEIF_RX, HEIF_RX + 0x2000), (AREA_RX, AREA_RX + 0x3000),
    (AREA_MENU_CODE, AREA_MENU_CODE + 0x1000), (AREA_QUICK_CODE, AREA_QUICK_CODE + 0x1000),
)
PHOTO_RO_RANGES = (
    (0x409DA000, 0x409DB000), (0x409EC000, 0x409ED000),
    (0x40B80000, 0x40B81000), (0x40B90000, 0x40B93000), (0x40B95000, 0x40B97000),
    (HEIF_MENU, HEIF_MENU + 0x1000), (AREA_GEOMETRY, AREA_GEOMETRY + 0x1000),
    (AREA_ICONS_RO, AREA_ICONS_RO + 0x1000),
)
PHOTO_WRITE_RANGES = (
    (0x409FD000, 0x409FE000), (0x40B50000, 0x40B52000), (0x40BD0000, 0x40BD1000),
)
_HEIF_DELTA = HEIF_RX - 0x40B70000
_AREA_DELTA = AREA_RX - 0x40B7C000


def _replace(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"Upstream photo source no longer matches {before!r}")
    return source.replace(before, after)


def _heif_source(name, source):
    if name == "patcher":
        source = _replace(
            source,
            "end, detail = menu(img, entries, (CODE_AT+len(code)+15) & ~15)\n"
            "    require(end <= 0x40B718D0, 'HEIF menu outside its allocation')",
            f"end, detail = menu(img, entries, {HEIF_MENU:#x})\n"
            "    require(end <= CODE_LIMIT, 'HEIF menu outside its allocation')",
        )
    source = re.sub(r"0x40B7[01][0-9A-F]{3}\b",
                    lambda token: hex(int(token[0], 16) + _HEIF_DELTA), source)
    if name == "patcher":
        source = _replace(source, f"CODE_AT, CODE_LIMIT = {HEIF_RX:#x}, {HEIF_RX + 0x18D0:#x}",
                          f"CODE_AT, CODE_LIMIT = {HEIF_RX:#x}, {HEIF_MENU + 0x1000:#x}")
    return source


def _area_source(name, source):
    if name == "geometry":
        return _replace(source, "0x40B7C000, 0x40B7D000",
                        f"{AREA_GEOMETRY:#x}, {AREA_GEOMETRY + 0x400:#x}")
    if name == "focus_icons":
        source = _replace(source, "0x40B7C700", f"{AREA_GEOMETRY + 0x400:#x}")
        source = _replace(source, "png_cave + len(new) <= CAVE_END",
                          f"png_cave + len(new) <= {AREA_GEOMETRY + 0x500:#x}")
    if name == "live_view":
        source = _replace(source, "blocks_end <= CAVE_A and CAVE_B + 4 * len(cb) <= A.DATA_END",
                          "blocks_end <= A.DATA_END and CAVE_B + 4 * len(cb) <= CAVE_A + 0x80")
    if name == "playback_wide":
        source = _replace(source, "limit=0x40B80000):",
                          f"limit={AREA_RX + 0x3000:#x}):")
        source = _replace(source, "tab = al.take(len(data), 'decode-size table (8 categories x 4 tiers)')",
                          f"tab = {AREA_GEOMETRY + 0x500:#x}")
    if name == "wide":
        source = _replace(
            source, "CAVE_AT, CAVE_END = (int(x, 16) for x in ALLOC['regions']['pipeline'][:2])",
            f"CAVE_AT, CAVE_END = {AREA_RX + 0x1000:#x}, {AREA_RX + 0x2000:#x}",
        )
    if name == "menu":
        source = _replace(source, "ok = area.take(16)",
                          f"ok = {AREA_MENU_CODE:#x} + 16 * (value == 6)")
        source = _replace(source, "cv = area.take(4 * 25)", f"cv = {AREA_MENU_CODE + 0x20:#x}")
    if name == "icons_wide":
        source = _replace(source, "icons, json_sha, json_name = pngs(icons_json);",
                          f"data_at = {AREA_ICONS_RO:#x}\n    icons, json_sha, json_name = pngs(icons_json);")
        for before, after in (
            ("p = at + PNGS", "p = data_at + PNGS"),
            ("end = at + PNGS + len(blob); assert end <= limit",
             f"end = data_at + PNGS + len(blob); assert end <= {AREA_ICONS_RO + 0x1000:#x}"),
            ("rec = at + RECORDS", "rec = data_at + RECORDS"),
            ("img.put(at + PNGS", "img.put(data_at + PNGS"),
            ("tab = at + MENU_TAB", "tab = data_at + MENU_TAB"),
        ):
            source = _replace(source, before, after)
    if name == "quick_picker":
        source = _replace(source, "AT = 0x40B91800", f"AT = {AREA_QUICK_CODE:#x}")
        source = _replace(source, "variants, trampolines = 0x40B95000,0x40B92000",
                          f"variants, trampolines = 0x40B95000,{AREA_QUICK_GLUE:#x}")
        source = _replace(source, "trampolines+len(glue)<=0x40B92800",
                          f"trampolines+len(glue)<={AREA_QUICK_CODE + 0x1000:#x}")
        source = _replace(source, "TOGGLE+len(table)<=AT", "TOGGLE+len(table)<=0x40B91800")
    if name == "thumbnail":
        source = _replace(source, "THUMB = 0x40B91200", f"THUMB = {AREA_THUMB_CODE:#x}")
    return re.sub(r"0x40B7[C-F][0-9A-F]{3}\b",
                  lambda token: hex(int(token[0], 16) + _AREA_DELTA), source)


@lru_cache(maxsize=None)
def _relocated_patcher(package):
    """Load only the files with allocation substitutions under a private package."""
    import sys
    from importlib.abc import MetaPathFinder, Loader

    transform = {"z8area": _area_source, "z8heif": _heif_source}[package]
    private = f"_fenestra_v051_{package}"
    directory = VENDOR / package

    class SourceLoader(Loader):
        def __init__(self, path):
            self.path = path

        def exec_module(self, module):
            source = transform(self.path.stem, self.path.read_text(encoding="utf-8"))
            exec(compile(source, str(self.path), "exec"), module.__dict__)

    class PhotoSources(MetaPathFinder):
        def find_spec(self, fullname, path=None, target=None):
            if fullname == private:
                filename = directory / "__init__.py"
                return util.spec_from_file_location(fullname, filename, submodule_search_locations=[str(directory)])
            if fullname.startswith(private + "."):
                filename = directory / (fullname.rsplit(".", 1)[1] + ".py")
                return util.spec_from_file_location(fullname, filename, loader=SourceLoader(filename))
            return None

    finder = PhotoSources()
    sys.meta_path.insert(0, finder)
    try:
        return import_module(private + ".patcher")
    finally:
        sys.meta_path.remove(finder)


ASSEMBLY_BASES = {
    "heif-format": (HEIF_RX, HEIF_RX + 0x1000),
    "heif-capture": (HEIF_RX + 0x18D0, HEIF_RX + 0x1950),
    "heif-srgb": (HEIF_RX + 0x1950, HEIF_RX + 0x1A00),
    "heif-drive": (HEIF_RX + 0x1B00, HEIF_RX + 0x1D00),
    "heif-icc": (HEIF_RX + 0x1D00, HEIF_RX + 0x2000),
    "area-formula": (AREA_RX + 0x1800, AREA_RX + 0x2000),
    "area-sizes": (0x40B82000, 0x40B83000),
    "area-quick": (AREA_QUICK_CODE, AREA_THUMB_CODE),
    "area-thumbnail": (AREA_THUMB_CODE, AREA_QUICK_GLUE),
    "precapture-menu": (RAW_MENU, RAW_MENU + 0x1000),
}


@lru_cache(maxsize=None)
def _assembly(name, compiler):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from z8patch.asm import link_elf
    from z8patch.compiled import SPECS, load

    reviewed, metadata = load(name, "prebuilt", "clang")
    spec = SPECS[name]
    base, limit = ASSEMBLY_BASES[name]
    source_path = VENDOR / spec["source"]
    source = source_path.read_text(encoding="utf-8")
    source = re.sub(r"original_(40B7[01][0-9A-F]{3})\b",
                    lambda m: f"original_{int(m[1], 16) + _HEIF_DELTA:08X}", source)
    with tempfile.TemporaryDirectory(prefix="fenestra-v051-photo-") as folder:
        assembly = Path(folder) / "hooks.S"
        obj = Path(folder) / "hooks.o"
        # Verify the compiler/source reproduces the released bytes before linking
        # the same code at the candidate address.
        assembly.write_text(source_path.read_text(encoding="utf-8"), encoding="utf-8")
        command = [compiler, *spec["flags"], "-c", str(assembly), "-o", str(obj)]
        subprocess.run(command, check=True, capture_output=True)
        baseline, symbols, reference_calls = link_elf(obj.read_bytes(), spec["asm_base"])
        expected = {k: int(v["va"], 16) for k, v in metadata["symbols"].items()}
        if baseline != reviewed or symbols != expected:
            raise ValueError(f"Compiler did not reproduce upstream {name}")
        if source != assembly.read_text(encoding="utf-8"):
            assembly.write_text(source, encoding="utf-8")
            subprocess.run(command, check=True, capture_output=True)
        code, symbols, calls = link_elf(obj.read_bytes(), base)
    if base + len(code) > limit or len(code) != len(reviewed):
        raise ValueError(f"Relocated {name} crossed its allocation")
    if any(symbols[label] - base != value - spec["asm_base"] for label, value in expected.items()):
        raise ValueError(f"Relocated {name} symbol drift")
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for reference, call in zip(reference_calls, calls, strict=True):
        target, pc = int(reference["target"], 16), int(call["pc"], 16)
        if not reference["symbol"].startswith("original_"):
            target += base - spec["asm_base"]
        elif 0x40B70000 <= target < 0x40B72000:
            target += _HEIF_DELTA
        instruction = next(disassembler.disasm(code[pc - base:pc - base + 4], pc))
        expected_branch = "bl" if reference["kind"] == 283 else "b"
        if (pc - base != int(reference["pc"], 16) - spec["asm_base"] or
                int(call["target"], 16) != target or instruction.mnemonic != expected_branch or
                instruction.op_str != f"#{target:#x}"):
            raise ValueError(f"Relocated {name} branch drift at {pc:#x}")
    return code, symbols, dict(base=hex(base), bytes=len(code), sha256=sha256(code).hexdigest(),
                               symbols={k: hex(v) for k, v in symbols.items()}, branch_relocations=calls)


def _runtime_offset(address):
    if INIT_RAM_BEGIN <= address < INIT_RAM_BEGIN + INIT_FILE_END - INIT_FILE_BEGIN:
        return address - INIT_RAM_BEGIN + INIT_FILE_BEGIN
    return address - BASE


def _private_records(blob):
    bundle = decode_bundle(blob, module_id="photo_tools")
    private = next(r["data"] for r in bundle["components"] if r["id"] == "photo_tools")
    component = decode_component(private, selection=16)
    return component, decode(component["capsule"])["patches"]


@lru_cache(maxsize=1)
def _r10_records(path):
    blob = path.read_bytes()
    if sha256(blob).hexdigest() != R10_SHA256:
        raise ValueError("Stable R10-C1 release SHA differs")
    _, records = _private_records(blob)
    capture = next(p for p in records if p.address == 0x409F0000)
    if len(records) != 82 or sha256(capture.new).hexdigest() != R10_CAPTURE_SHA256:
        raise ValueError("Stable R10-C1 private component differs")
    return records


def _preserved_records(reference_path):
    reference = _r10_records(Path(reference_path).resolve())
    skipped = R10_CONTROL | PIXELSHIFT_RECORDS
    if not skipped <= {p.address for p in reference}:
        raise ValueError("Stable R10-C1 record ownership differs")
    return [record for record in reference if record.address not in skipped]


def _patch_window(ordered, address, size, kind, flags, name):
    end = address + size
    old, new = bytearray(), bytearray()
    for record in ordered:
        first = max(address, record.address)
        last = min(end, record.address + len(record.new))
        if first >= last:
            continue
        if first != address + len(new) or (record.kind, record.flags) != (kind, flags):
            raise ValueError(f"{name} permissions or coverage differ at {first:#x}")
        begin, limit = first - record.address, last - record.address
        old.extend(record.old[begin:limit])
        new.extend(record.new[begin:limit])
    if len(new) != size:
        raise ValueError(f"{name} coverage differs at {address:#x}")
    return bytes(old), bytes(new)


def verify_packaged_photo(path, reference_path=R10_RELEASE):
    """Verify the released capture bytes and new Pixel Shift ABI in a WLM."""
    from z8_photo_tools_profile import validate_metadata

    blob = Path(path).read_bytes()
    component, records = _private_records(blob)
    ordered = sorted(records, key=lambda p: p.address)
    reference = _preserved_records(reference_path)
    for stock in reference:
        old, new = _patch_window(ordered, stock.address, len(stock.new), stock.kind, stock.flags,
                                 "R10-C1 capture")
        if (old, new) != (stock.old, stock.new):
            raise ValueError(f"R10-C1 capture bytes differ at {stock.address:#x}")

    metadata = compiled_metadata()
    reviewed = metadata["units"]["pixelshift"]
    _, psm = _patch_window(ordered, 0x40990000, reviewed["bytes"], IMAGE, EXEC, "Pixel Shift")
    if sha256(psm).hexdigest() != reviewed["sha256"]:
        raise ValueError("Candidate Pixel Shift image differs from v0.5.1")
    return dict(bundle_sha256=sha256(blob).hexdigest(), component_sha256=component["sha256"],
                component_version=component["version"], records=len(records),
                preserved_r10_c1_records=len(reference), r10_c1_sha256=R10_SHA256,
                capture_sha256=R10_CAPTURE_SHA256,
                pixelshift=dict(bytes=len(psm), sha256=reviewed["sha256"],
                                **validate_metadata(metadata, v051=True, code=psm)))


def emit_photo(stock_eg, strings, unit, reference_path=R10_RELEASE):
    """Compose stock-checked still-photo features and retain exact R10 capture."""
    checked_upstream()
    from z8patch.compiled import load
    from z8patch.image import Image
    from z8precapture.menu import sites
    from z8pixelshift.patcher import build_main as build_pixelshift
    from z8_photo_tools_profile import validate_metadata

    default = "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang.exe"
    compiler = os.environ.get("FENESTRA_V051_CLANG", default if os.name == "nt" else "clang")
    evidence = {}

    def build_unit(name):
        code, symbols, evidence[name] = _assembly(name, compiler)
        return code, symbols.copy()

    img = Image(stock_eg)
    code, metadata = load("pixelshift", "prebuilt", "clang")
    if len(code) != 130664:
        raise ValueError("v0.5.1 Pixel Shift image length differs")
    abi = validate_metadata({"units": {"pixelshift": metadata}}, v051=True, code=code)
    af, _ = unit("pixelshift-af")
    psm, psm_detail = build_pixelshift(stock_eg, code, metadata["symbols"], af, strings)
    if {e["offset"] for e in psm.edits} != {_runtime_offset(a) for a in PIXELSHIFT_RECORDS}:
        raise ValueError("Pixel Shift replacement does not cover the exact R10-C1 Pixel Shift record set")
    reference = _preserved_records(reference_path)
    for record in reference:
        img.put(_runtime_offset(record.address), record.new, "R10-C1 stable capture", expect=record.old, offset=True)

    def merge(edits):
        for e in edits:
            if e["offset"] != COPY_END_SITE - BASE:
                img.put(e["offset"], e["new"], e["purpose"], expect=e["old"], offset=True)

    merge(psm.edits)
    del psm
    menu, labels = build_unit("precapture-menu")
    original_menu_base = sites.BASE
    sites.BASE = RAW_MENU
    try:
        for address, old, new in sites.apply(stock_eg, menu, labels):
            img.put(address, new, "C15/C30 native RAW recording menu policy", expect=old)
    finally:
        sites.BASE = original_menu_base

    heif, heif_detail = _relocated_patcher("z8heif").build_main(stock_eg, strings, build_unit)
    merge(heif.edits)
    del heif
    area, area_detail = _relocated_patcher("z8area").build_main(stock_eg, strings, build_unit)
    merge(area.edits)
    ranges = (*PHOTO_CODE_RANGES, *PHOTO_RO_RANGES, *PHOTO_WRITE_RANGES)
    for e in img.edits:
        address = BASE + e["offset"]
        if ARENA_BEGIN <= address < ARENA_END:
            if sum(lo <= address and BASE + e["end"] <= hi for lo, hi in ranges) != 1:
                raise ValueError(f"Photo Tools image lacks a unique permission at {address:#x}")
    return img.edits, {
        "r10_c1_sha256": R10_SHA256, "capture_sha256": R10_CAPTURE_SHA256,
        "stable_capture_records": len(reference),
        "pixelshift": psm_detail, "pixelshift_abi": abi, "heif": heif_detail,
        "area": area_detail, "relocated_assembly": evidence,
    }
