"""Exercise the production C3.11 release-mode predicate through synthetic reads."""

from pathlib import Path
import subprocess
import tempfile

from toolchain import host_compiler


ROOT = Path(__file__).resolve().parents[1]


def test_only_single_frame_without_self_timer_is_allowed():
    with tempfile.TemporaryDirectory() as folder:
        executable = Path(folder) / "lut-release.exe"
        subprocess.run([
            host_compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DFX_Z8_LUT_SYNTHETIC_RELEASE=1", "-Iinclude",
            "tests/test_z8_lut_release.c", "-o", executable,
        ], cwd=ROOT, check=True, timeout=45)
        subprocess.run([str(executable)], check=True, timeout=10)
