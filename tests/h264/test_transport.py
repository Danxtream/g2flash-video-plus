"""Check the private transport's existing framing and explicit reply routing.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TransportTests(unittest.TestCase):
    def test_crc_persistent_inflater_mtu23_lens_replies_and_stock_fallthrough(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "transport-test"
            subprocess.run([
                "clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                str(ROOT / "tests/h264/transport_test.c"), "-lz", "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
