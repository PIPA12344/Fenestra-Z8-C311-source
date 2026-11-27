"""Execute the compiled Photo Tools STOP/RESUME/GUI_POLL crop guard with synthetic DS calls."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONTROLS = Path(os.environ.get('FX_WLM_CONTROLS', ROOT / 'build/release-20261128/controls'))


class CropShutdownTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (CONTROLS / 'photo_tools/controls.json').is_file():
            raise unittest.SkipTest('build the v051 controls or set FX_WLM_CONTROLS')
        path = ROOT / 'nativepacks/wlm_controls/tests/run_checks.py'
        spec = importlib.util.spec_from_file_location('crop_control_checks', path)
        cls.checks = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.checks)
        cls.metas = {n: json.loads((CONTROLS / n / 'controls.json').read_bytes())
                     for n in cls.checks.build.BASES}
        if cls.metas['photo_tools'].get('suite') != 'v051':
            raise unittest.SkipTest('v051 controls required')

    def harness(self, initial):
        c = self.checks

        class PropertyHarness(c.Harness):
            area = initial
            writes = 0

            def hook(self, u, address, size, data):
                if address in (0x403ec398, 0x403ec39c) and u.reg_read(c.UC_ARM64_REG_X0) == 0x07000603:
                    if address == 0x403ec398:
                        u.reg_write(c.UC_ARM64_REG_X0, self.area)
                    else:
                        self.area = u.reg_read(c.UC_ARM64_REG_X1)
                        self.writes += 1
                        u.reg_write(c.UC_ARM64_REG_X0, 0)
                    return
                super().hook(u, address, size, data)

        h = PropertyHarness(self.metas, 16)
        h.stage_all()
        h.publish()
        return h

    def test_stock_values_survive_and_custom_values_park_until_first_poll(self):
        for value in (0, 1, 2, 3, 4, 5, 6, 8, 99):
            with self.subTest(value=value):
                h = self.harness(value)
                self.assertEqual(h.event('photo_tools', 2), 0)
                parked = value if value in (0, 1, 4, 8) else 0
                self.assertEqual(h.area, parked)
                self.assertEqual(h.event('photo_tools', 2), 0)
                self.assertEqual(h.event('photo_tools', 4), 0)
                self.assertEqual(h.area, parked)
                self.assertEqual(h.event('photo_tools', 1), 0)
                self.assertEqual(h.area, value if value in (2, 3, 6) else parked)
                writes = h.writes
                self.assertEqual(h.event('photo_tools', 1), 0)
                self.assertEqual(h.writes, writes)

    def test_resume_does_not_overwrite_user_selected_area(self):
        h = self.harness(3)
        self.assertEqual(h.event('photo_tools', 2), 0)
        self.assertEqual(h.event('photo_tools', 4), 0)
        h.area = 1
        self.assertEqual(h.event('photo_tools', 1), 0)
        self.assertEqual(h.area, 1)

    def test_second_shutdown_before_first_poll_preserves_pending_crop(self):
        h = self.harness(6)
        for _ in range(2):
            self.assertEqual(h.event('photo_tools', 2), 0)
            self.assertEqual(h.area, 0)
            self.assertEqual(h.event('photo_tools', 4), 0)
        self.assertEqual(h.event('photo_tools', 1), 0)
        self.assertEqual(h.area, 6)


if __name__ == '__main__':
    unittest.main()
