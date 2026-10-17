"""Compare separable interpolation with the original C kernel, no fixtures.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MotionCompareTests(unittest.TestCase):
    def test_all_fractions_shapes_edges_and_random_pixels(self):
        vendor = ROOT / 'third-party/sub0h264/components/sub0h264'
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'motion-compare'
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g', '-DNDEBUG',
                            '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I' + str(vendor / 'src'), '-I' + str(vendor / 'include'),
                            str(ROOT / 'tests/h264/motion_compare.cpp'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=180)

    def test_mve_source_lane_model_not_arm_instructions(self):
        vendor = ROOT / 'third-party/sub0h264/components/sub0h264'
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'motion-lane-model'
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g', '-DNDEBUG',
                            '-D__ARM_FEATURE_MVE=1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I' + str(ROOT / 'tests/h264/mve_model'),
                            '-I' + str(vendor / 'src'), '-I' + str(vendor / 'include'),
                            str(ROOT / 'tests/h264/motion_compare.cpp'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=180)
