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

    def test_tv_table_and_video_black_floor_with_both_scales(self):
        subprocess.run([str(self.exe), "range"], check=True)

    def test_dither_quarter_levels_and_final_panel_phase_native_and_double(self):
        subprocess.run([str(self.exe), "dither"], check=True)

    def test_dither_average_tracks_plain_with_monotonic_pixels_and_exact_endpoints(self):
        subprocess.run([str(self.exe), "dither"], check=True)

    def test_double_dither_cells_match_scalar_for_every_range_and_row_phase(self):
        subprocess.run([str(self.exe), "cells"], check=True)

    def test_double_plain_cells_match_every_packed_pair_with_odd_geometry_padding_and_guards(self):
        subprocess.run([str(self.exe), "plain-cells"], check=True)

    def test_margins_only_clear_survives_scale_mode_odd_centering_and_old_menu(self):
        subprocess.run([str(self.exe), "margins"], check=True)

    def test_picture_rect_keeps_bars_unlit_and_black_content_lit_for_all_shapes_and_modes(self):
        subprocess.run([str(self.exe), "picture-margins"], check=True)

    def test_four_floors_keep_default_and_bright_curve_and_dark_bars_for_both_scales(self):
        subprocess.run([str(self.exe), "picture-styles"], check=True)

    def test_mid_sizes_fill_the_panel_width_with_centre_sampling_and_unlit_bars(self):
        subprocess.run([str(self.exe), "full-width"], check=True)


if __name__ == "__main__":
    unittest.main()
