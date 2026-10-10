"""Exercise the actual singleton helper with fatal native sanitizers.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ContextTests(unittest.TestCase):
    def test_first_use_warm_reads_refusal_and_competing_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "context-test"
            subprocess.run(["clang", "-std=c11", "-D_GNU_SOURCE", "-O2", "-pthread",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", str(ROOT / "tests/h264/context_test.c"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
