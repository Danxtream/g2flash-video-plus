"""Validate shared-shadow packing independently of asynchronous display services.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "pack-test"
        subprocess.run(["clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(ROOT / "tests/h264/pack_test.c"), "-o", str(cls.exe)], check=True)

    def test_all_brightness_values_top_down_native_and_doubled_with_padding(self):
        subprocess.run([str(self.exe), "pixels"], check=True)

    def test_invalid_overlap_geometry_and_sizes_leave_destination_untouched(self):
        subprocess.run([str(self.exe), "reject"], check=True)

    def test_stock_lens_side_maps_to_transport_without_mirroring_pixels(self):
        subprocess.run([str(self.exe), "sides"], check=True)


if __name__ == "__main__":
    unittest.main()
