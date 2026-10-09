"""Compare optional chroma builds under sanitizers, including every fixture.

SPDX-License-Identifier: GPL-3.0-only
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / "third-party/sub0h264"


class ChromaBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        common = ["-std=c++23", "-O2", "-g", "-DNDEBUG", "-Wall", "-Wextra",
                  "-Werror", "-Wno-tautological-constant-out-of-range-compare",
                  "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        objects = []
        for name, enabled in (("reference", 1), ("trimmed", 0)):
            obj = directory / (name + ".o")
            subprocess.run(["clang++", *common,
                            "-I" + str(VENDOR / "components/sub0h264/src"),
                            "-I" + str(VENDOR / "components/sub0h264/include"),
                            f"-DSUB0H264_ENABLE_CHROMA_RECONSTRUCTION={enabled}",
                            f"-Dsub0h264=chroma_{name}_decoder",
                            f"-DCHROMA_PROBE_PREFIX=chroma_{name}_", "-c",
                            str(VENDOR / "tests/chroma_build_probe.cpp"), "-o", str(obj)], check=True)
            objects.append(obj)
        cls.exe = directory / "chroma-compare"
        subprocess.run(["clang++", *common, str(VENDOR / "tests/chroma_build_compare.cpp"),
                        *map(str, objects), "-o", str(cls.exe)], check=True)

    def test_disabled_storage_dpb_and_deblocking_threshold(self):
        subprocess.run([str(self.exe)], check=True, timeout=60)

    def test_every_fixture_y_and_entropy_state(self):
        clips = sorted((VENDOR / "tests/fixtures").rglob("*.h264"))
        if not clips:
            self.skipTest("optional ignored decoder fixtures are not installed")
        subprocess.run([str(self.exe), *map(str, clips)], check=True, timeout=1200)

    def test_caller_supplied_clips_preserve_y_and_entropy_state(self):
        clips = [s for s in os.environ.get("G2_H264_TEST_CLIPS", "").split(os.pathsep) if s]
        if not clips:
            self.skipTest("no optional comparison clips supplied")
        subprocess.run([str(self.exe), *clips], check=True, timeout=1200)

    def test_disabled_public_library_builds_on_generic_and_x86(self):
        component = VENDOR / "components/sub0h264"
        for platform in ("generic", "x86"):
            with self.subTest(platform=platform):
                for source in (component / "src/sub0h264_unity.cpp",
                               component / f"platform/{platform}/sub0h264_platform_{platform}.cpp"):
                    output = Path(self.temp.name) / (platform + "-" + source.stem + ".o")
                    subprocess.run(["clang++", "-std=c++23", "-O2", "-DNDEBUG", "-Wall",
                                    "-Wextra", "-Werror",
                                    "-Wno-tautological-constant-out-of-range-compare",
                                    "-DSUB0H264_ENABLE_CHROMA_RECONSTRUCTION=0",
                                    "-I" + str(component / "include"),
                                    "-I" + str(component / "src"), "-c", str(source),
                                    "-o", str(output)], check=True)


if __name__ == "__main__":
    unittest.main()
