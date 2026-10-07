"""Check optimizer-only initializer values and cold table ownership.

SPDX-License-Identifier: GPL-3.0-only
Run from the root in Ubuntu: python3 -m unittest discover -s tests/h264 -v.
"""
import importlib.util
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parents[2] / "patches/h264"
spec = importlib.util.spec_from_file_location("h264_constants", HERE / "constant_visibility.py")
constants = importlib.util.module_from_spec(spec)
spec.loader.exec_module(constants)

HOT = """$unused = comdat any
@table = external dso_local hidden constant [2 x i32]
@unused = external dso_local hidden constant i8
@outside = external constant i32
define internal i32 @hot() {
  %value = load i32, ptr @table
  ret i32 %value
}
"""
COLD = """$unused = comdat any
@table = dso_local hidden unnamed_addr constant [2 x i32] [i32 3, i32 5], align 8
@unused = linkonce_odr constant i8 1, comdat($unused), align 1
@outside = external constant i32
declare i32 @hot()
"""


class ConstantVisibilityTests(unittest.TestCase):
    def test_initializer_values_and_other_hot_ir_are_preserved(self):
        result = constants.expose(HOT, COLD, ["table", "unused"])
        self.assertIn("available_externally dso_local hidden unnamed_addr constant [2 x i32] [i32 3, i32 5], align 8", result)
        self.assertIn("available_externally dso_local hidden  constant i8 1, align 1", result)
        without_globals = lambda text: "".join(line for line in text.splitlines(keepends=True)
                                               if not line.startswith("@"))
        self.assertEqual(without_globals(result), without_globals(HOT))
        self.assertIn("@outside = external constant i32\n", result)
        self.assertEqual(result.count(" = available_externally "), 2)

    def test_comdat_removed_only_from_optimizer_initializers(self):
        cold = COLD
        result = constants.expose(HOT, cold, ["table", "unused"])
        self.assertNotRegex(result, r"(?m)^@.*comdat")
        self.assertIn("$unused = comdat any", result)
        self.assertEqual(cold, COLD)
        self.assertIn("comdat($unused)", cold)

    def test_quoted_constant_names_are_preserved(self):
        hot = '@"a table" = external dso_local hidden constant i32\n'
        cold = '@"a table" = private local_unnamed_addr constant i32 9\n'
        result = constants.expose(hot, cold, ['"a table"'])
        self.assertEqual(result, '@"a table" = available_externally dso_local hidden local_unnamed_addr constant i32 9\n')

    def test_missing_or_duplicate_owners_are_rejected(self):
        for hot, cold, names in ((HOT, COLD, ["table", "unused", "unused"]),
                                 (HOT, COLD, ["table"]),
                                 (HOT, COLD + COLD.splitlines()[1] + "\n", ["table", "unused"]),
                                 (HOT.replace(HOT.splitlines()[1] + "\n", ""), COLD, ["table", "unused"]),
                                 (HOT + HOT.splitlines()[1] + "\n", COLD, ["table", "unused"])):
            with self.subTest(hot=hot, cold=cold, names=names), self.assertRaises(ValueError):
                constants.expose(hot, cold, names)

    def test_definitions_or_mismatched_hot_types_are_rejected(self):
        for hot in (HOT.replace("external dso_local hidden constant", "constant", 1),
                    HOT.replace("constant [2 x i32]", "constant [3 x i32]", 1)):
            with self.subTest(hot=hot), self.assertRaises(ValueError):
                constants.expose(hot, COLD, ["table", "unused"])

    def test_writable_or_unsupported_cold_globals_are_rejected(self):
        for cold in (COLD.replace(" constant ", " global ", 1),
                     COLD.replace("unnamed_addr constant", "available_externally constant", 1),
                     COLD.replace(COLD.splitlines()[1] + "\n", "")):
            with self.subTest(cold=cold), self.assertRaises(ValueError):
                constants.expose(HOT, cold, ["table", "unused"])

    def test_empty_constant_selection_leaves_ir_unchanged(self):
        self.assertEqual(constants.expose("declare void @f()\n", "declare void @f()\n", []),
                         "declare void @f()\n")


if __name__ == "__main__":
    unittest.main()
