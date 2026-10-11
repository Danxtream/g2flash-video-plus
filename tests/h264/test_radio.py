"""Exercise the connection-bound PHY adapter with fatal sanitizers.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RadioTests(unittest.TestCase):
    def check_native(self, platform=False):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "radio-test"
            subprocess.run([
                "clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unknown-pragmas",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-D_GNU_SOURCE",
                *(["-DVIDEO_RADIO_PLATFORM_TEST"] if platform else []),
                str(ROOT / "tests/h264/radio_test.c"), "-o", str(executable)
            ], check=True)
            subprocess.run([str(executable)], check=True, timeout=30)


    def test_epochs_fixed_mailboxes_refusals_and_stock_callback_chaining(self):
        self.check_native()

    def test_fixed_pool_classes_spares_and_command_credit_guards(self):
        self.check_native(True)


if __name__ == "__main__":
    unittest.main()
