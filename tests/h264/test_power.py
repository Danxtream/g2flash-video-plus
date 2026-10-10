"""Paired display leases and the production refusal adapter, with fatal checks.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PowerTests(unittest.TestCase):
    def check_native(self, adapter):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'power-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-unused-variable',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            *(['-DPOWER_ADAPTER'] if adapter else []),
                            str(ROOT / 'tests/h264/power_test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_bounded_stock_adapter_refuses_and_rolls_back_without_asserting(self):
        self.check_native(True)

    def test_both_lens_finite_leases_and_callback_deferral(self):
        self.check_native(False)


if __name__ == '__main__':
    unittest.main()
