"""Synthetic luma deblocking comparisons; lane model is not ARM execution.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DeblockCompareTests(unittest.TestCase):
    def compile_and_run(self, model):
        vendor = ROOT / 'third-party/sub0h264/components/sub0h264'
        flags = ['-D__ARM_FEATURE_MVE=1', '-I' + str(ROOT / 'tests/h264/mve_model')] if model else []
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'deblock-compare'
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g', '-DNDEBUG',
                            '-Wall', '-Wextra', '-Werror', *flags,
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I' + str(vendor / 'src'), '-I' + str(vendor / 'include'),
                            str(ROOT / 'tests/h264/deblock_compare.cpp'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=180)

    def test_zero_strength_skip_and_macroblock_traversal(self):
        self.compile_and_run(False)

    def test_mve_source_lane_model_all_strengths_thresholds_and_orientations(self):
        self.compile_and_run(True)
