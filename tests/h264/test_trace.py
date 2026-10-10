"""Check callback removal, retained printf tracing and byte-identical output.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / "third-party/sub0h264"


class CallbackTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executables = {}
        for name, definitions in (("default", []),
                                  ("disabled", ["-DSUB0H264_ENABLE_CALLBACK_TRACE=0"]),
                                  ("printf", ["-DSUB0H264_ENABLE_CALLBACK_TRACE=0", "-DSUB0H264_TRACE=1"])):
            executable = Path(cls.temp.name) / name
            subprocess.run(["clang++", "-std=c++23", "-O2", "-g", "-DNDEBUG", "-Wall",
                            "-Wextra", "-Werror", "-Wno-tautological-constant-out-of-range-compare",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            *definitions, "-DSUB0H264_ENABLE_CHROMA_RECONSTRUCTION=0",
                            "-I" + str(VENDOR / "components/sub0h264/src"),
                            "-I" + str(VENDOR / "components/sub0h264/include"),
                            str(ROOT / "tests/h264/trace_probe.cpp"), "-o", str(executable)], check=True)
            cls.executables[name] = executable

    def test_default_and_disabled_callback_and_argument_construction(self):
        for name in ("default", "disabled"):
            result = subprocess.run([str(self.executables[name])], capture_output=True, check=True)
            self.assertEqual(result.stdout, b"")

    def test_printf_tracing_is_independent(self):
        result = subprocess.run([str(self.executables["printf"])], capture_output=True, check=True)
        self.assertIn(b"[TRACE] MB(0,0) END bit=7", result.stdout)

    def test_all_fixture_y_outcomes_and_cabac_contexts(self):
        clips = sorted((VENDOR / "tests/fixtures").rglob("*.h264"))
        if not clips:
            self.skipTest("optional ignored decoder fixtures are not installed")
        for clip in clips:
            with self.subTest(clip=clip.name), tempfile.TemporaryFile() as left, tempfile.TemporaryFile() as right:
                subprocess.run([str(self.executables["default"]), str(clip)], stdout=left,
                               stderr=subprocess.PIPE, check=True, timeout=60)
                subprocess.run([str(self.executables["disabled"]), str(clip)], stdout=right,
                               stderr=subprocess.PIPE, check=True, timeout=60)
                left.seek(0); right.seek(0)
                while True:
                    a, b = left.read(65536), right.read(65536)
                    self.assertEqual(a, b, clip.name)
                    if not a: break


if __name__ == "__main__":
    unittest.main()
