"""Exercise the production JPEG SMP lease protocol with synthetic RTOS threads."""

import os
from pathlib import Path
import subprocess
import tempfile

from toolchain import host_compiler


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "modules/jpeg_lut"


def test_smp_waits_for_running_helpers_and_revokes_queued_work():
    with tempfile.TemporaryDirectory(prefix="jpeg-smp-") as folder:
        executable = Path(folder) / ("check.exe" if os.name == "nt" else "check")
        flags = [] if os.name == "nt" else ["-pthread"]
        subprocess.run([host_compiler("clang"), "-std=c11", "-O2", "-Wall", "-Wextra",
                        "-Werror", "-Wconversion", "-Wshadow", *flags,
                        "-DFX_Z8_LUT_SYNTHETIC_RELEASE=1", "-I", ROOT / "include",
                        "-I", SOURCE, SOURCE / "cube.c", SOURCE / "cube_frame.c", ROOT / "tests/test_jpeg_smp.c",
                        "-o", executable], cwd=folder, check=True, timeout=45)
        result = subprocess.run([executable], capture_output=True, text=True, check=True, timeout=15)
        assert "PASS SMP dispatch" in result.stdout
        for height, chunk, tail in ((5504, 64, 0), (1792, 64, 0), (1080, 32, 24), (1795, 64, 3)):
            assert f"PASS dynamic height {height} chunk {chunk} tail {tail}," in result.stdout
        assert "PASS dynamic mid-chunk timeout, join and output bounds" in result.stdout
        assert "PASS burst release admission and refcounted OFF frame leases" in result.stdout
        result = subprocess.run([executable, "resume-failure"], capture_output=True, text=True, check=True, timeout=15)
        assert "PASS dormant task and semaphore cleanup" in result.stdout
