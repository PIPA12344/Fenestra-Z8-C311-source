"""Pinned ABI facts for the reviewed upstream Photo Tools backends."""
import struct

UPSTREAM_COMMIT = "a21fc8c04b6dd863439c4192d387b755633287f6"
V051_UPSTREAM_COMMIT = "25fefa1e17f321ee285bea77b3c54427f9912e4b"
RUNTIME_ADDRESS = 0xA0FFED000
RUNTIME_RESERVED = 12288
RUNTIME_BYTES = 10496
RUNTIME_MAGIC = 0x42534D50
TASK_REGISTRY = 0xA03203E08
RUNTIME_OFFSETS = {
    "magic":0,"state":4,"cancel":8,"status":12,"serial":16,"claimed":20,"modal":24,
    "worker":48,"worker_id":56,"workers":60,"spawned":64,"context.rows":396,
    "context.total":400,"context.phase":404,"context.phase_first":408,"context.phase_second":412,
    "context.serial":416,"job_mode":480,"group_index":484,"group_count":488,"merged":492,
    "targets":496,"attempted":500,"seconds":504,"browser_open":1696,"operation":1700,
    "modal_serial":10464,"result":10468,"review_serial":10484,
}
SYMBOLS = {"psm_worker":0x409A44CC,"psm_camera_job":0x40995818}
V051_SYMBOLS = {
    "psm_menu_action": 0x40990000,
    "psm_photo_entry": 0x4099017C,
    "psm_photo_refresh": 0x409902EC,
    "psm_menu_available": 0x40992370,
    "psm_worker": 0x409A4550,
    "psm_camera_job": 0x40995A34,
}
V051_LAYOUT = (RUNTIME_BYTES, 4704, 10304, 584576, 501696, 145664,
               RUNTIME_OFFSETS["modal"], RUNTIME_OFFSETS["operation"],
               RUNTIME_OFFSETS["modal_serial"], RUNTIME_OFFSETS["review_serial"])


def validate_metadata(metadata, *, v051=False, code=None):
    symbols = metadata["units"]["pixelshift"]["symbols"]
    expected_symbols = V051_SYMBOLS if v051 else SYMBOLS
    for name, expected in expected_symbols.items():
        if int(symbols[name]["va"],16) != expected:
            raise ValueError("photo-tools reviewed symbol mismatch: " + name)
    if v051:
        layout = symbols["ps9_runtime_layout"]
        if (code is None or len(code) != 130664 or layout["bytes"] != 40 or
                struct.unpack_from("<10I", code, int(layout["va"], 16) - 0x40990000) != V051_LAYOUT):
            raise ValueError("photo-tools v0.5.1 reviewed runtime layout mismatch")
    return dict(runtime=hex(RUNTIME_ADDRESS), runtime_bytes=RUNTIME_BYTES,
                permanent_worker=hex(expected_symbols["psm_worker"]),
                camera_job=hex(expected_symbols["psm_camera_job"]))
