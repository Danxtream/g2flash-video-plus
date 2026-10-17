"""Keep the integer-vector target confined to the hot decoder module.

SPDX-License-Identifier: GPL-3.0-only
"""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "hot_target_builder", ROOT / "patches/h264/build_decoder.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

IR = """@table = private constant i32 7
define internal i32 @sub0h264_hot() #0 {
  %v = load i32, ptr @table
  ret i32 %v
}
define internal i32 @cold() #0 {
  ret i32 1
}
attributes #0 = { nounwind "target-cpu"="baseline" }
"""


class HotTargetTests(unittest.TestCase):
    def test_integer_mve_flags_keep_base_abi_and_header_multilib(self):
        headers = [Path("headers/cxx"), Path("headers/softfp"), Path("headers/c")]
        baseline = builder.toolchain.compiler_flags(headers)
        hot = builder.toolchain.hot_compiler_flags(headers)
        expected = [flag for flag in baseline
                    if not flag.startswith(("--target=", "-mfpu="))]
        self.assertEqual(hot, ["--target=arm-none-eabi", "-mcpu=cortex-m55+nofp", *expected])
        self.assertIn("-mfloat-abi=softfp", hot)
        self.assertEqual(hot.count("-O2"), 1)
        self.assertEqual(builder.toolchain.compiler_flags(headers), baseline)

    def compile(self, directory, hot_flags=None):
        commands = []
        def run(command, check):
            commands.append(command)
            if "-emit-llvm" in command:
                text = IR.replace('"baseline"', '"cortex-m55"') if "-mcpu=cortex-m55+nofp" in command else IR
                Path(command[-1]).write_text(text, encoding="utf-8")
        config = {"hot_groups": ["hot"], "promotions": [], "external_promotions": {}}
        with mock.patch.object(builder.subprocess, "run", side_effect=run):
            builder.compile_modules(directory, ["-O2"], selection=config, hot_flags=hot_flags)
        return commands

    def test_separate_frontend_leaves_cold_ir_and_flags_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.compile(root / "baseline")
            commands = self.compile(root / "vector", ["-O2", "-mcpu=cortex-m55+nofp"])
            self.assertEqual((root / "baseline/cold.ll").read_bytes(),
                             (root / "vector/cold.ll").read_bytes())
            hot = (root / "vector/hot.ll").read_text(encoding="utf-8")
            self.assertIn('"target-cpu"="cortex-m55"', hot)
            objects = [command for command in commands if "-c" in command]
            self.assertIn("-mcpu=cortex-m55+nofp", objects[0])
            self.assertNotIn("-mcpu=cortex-m55+nofp", objects[1])
            self.assertIn("-Oz", objects[1])
            membership = json.loads((root / "vector/membership.json").read_text())
            self.assertIn("hot_preopt_sha256", membership)

    def test_default_compilation_has_no_additional_frontend(self):
        with tempfile.TemporaryDirectory() as directory:
            commands = self.compile(directory)
        self.assertEqual(sum("-emit-llvm" in command for command in commands), 1)

    def test_target_cannot_change_frozen_function_ownership(self):
        def run(command, check):
            if "-emit-llvm" in command:
                text = IR.replace("@cold()", "@different()") if "-mcpu=cortex-m55+nofp" in command else IR
                Path(command[-1]).write_text(text, encoding="utf-8")
        config = {"hot_groups": ["hot"], "promotions": [], "external_promotions": {}}
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(builder.subprocess, "run", side_effect=run):
            with self.assertRaisesRegex(ValueError, "ownership"):
                builder.compile_modules(directory, ["-O2"], selection=config,
                                        hot_flags=["-O2", "-mcpu=cortex-m55+nofp"])
