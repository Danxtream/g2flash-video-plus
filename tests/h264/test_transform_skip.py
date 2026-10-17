"""Count-aware transform tests without clip fixtures.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TransformSkipTests(unittest.TestCase):
    def test_zero_dc_random_strides_and_in_place(self):
        vendor = ROOT / 'third-party/sub0h264/components/sub0h264'
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'transform-skip'
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g', '-DNDEBUG',
                            '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I' + str(vendor / 'src'), '-I' + str(vendor / 'include'),
                            str(ROOT / 'tests/h264/transform_skip.cpp'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=60)
