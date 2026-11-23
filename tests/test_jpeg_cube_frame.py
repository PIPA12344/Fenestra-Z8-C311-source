"""Compile the production 4:2:2 frame-cache renderer and check the pixel oracle."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from toolchain import host_compiler


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "modules/jpeg_lut"


class CubeFrameTests(unittest.TestCase):
    def test_warm_cold_collisions_inplace_and_row_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="jpeg-cube-frame-") as folder:
            exe = Path(folder) / ("check.exe" if os.name == "nt" else "check")
            subprocess.run([host_compiler("clang"), "-std=c11", "-Os", "-Wall", "-Wextra",
                            "-Werror", "-Wconversion", "-Wshadow", "-I", SOURCE, SOURCE / "cube.c",
                            SOURCE / "cube_frame.c", ROOT / "tests/test_jpeg_cube_frame.c",
                            "-o", exe], cwd=folder, check=True, timeout=45)
            ran = subprocess.run([exe], capture_output=True, text=True, check=True, timeout=30)
            self.assertIn("PASS grid 33 max Y error 0", ran.stdout)
