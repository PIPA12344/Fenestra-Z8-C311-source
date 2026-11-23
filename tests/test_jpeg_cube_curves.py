"""Compile the real CUBE implementation and exercise the DSP curve compiler."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from toolchain import host_compiler


ROOT = Path(__file__).resolve().parents[1]


class JpegCubeCurveTests(unittest.TestCase):
    def test_curve_compilation(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / ("curves.exe" if os.name == "nt" else "curves")
            subprocess.run([host_compiler(), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Wconversion", "-Wshadow", "-Imodules/jpeg_lut", "modules/jpeg_lut/cube.c",
                            "tests/test_jpeg_cube_curves.c", "-o", str(exe)], cwd=ROOT, timeout=45, check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: XM6 CUBE curves", result.stdout)


if __name__ == "__main__":
    unittest.main()
