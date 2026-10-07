"""Check IR ownership, frozen hot selection, real links and native Y equivalence.

SPDX-License-Identifier: GPL-3.0-only
Run from the root in Ubuntu: python3 -m unittest discover -s tests/h264 -v.
Ignored fixtures are optional; G2_H264_TEST_CLIPS adds local paths for validation.
All build products and Y dumps stay in temporary directories.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / "patches/h264"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load_module("h264_decoder_builder", HERE / "build_decoder.py")
partition = builder.partition_ir
IR = """@table = private unnamed_addr constant [2 x i32] [i32 3, i32 5]
@unused = private constant i8 1
define internal i32 @sub0h264_hot(ptr align 8 %p) #0 align 2 {
  %v = load i32, ptr @table
  %r = call i32 @helper(i32 %v)
  ret i32 %r
}
define linkonce_odr i32 @helper(i32 %x) #0 {
  ret i32 %x
}
define internal void @local() #0 {
  ret void
}
attributes #0 = { nounwind }
"""


class PartitionTests(unittest.TestCase):
    def test_every_function_and_constant_has_one_owner(self):
        hot, cold, membership = partition.partition(IR, ["hot"], [])
        self.assertEqual(set(partition.definitions(hot)), {"sub0h264_hot"})
        self.assertEqual(set(partition.definitions(cold)), {"helper", "local"})
        self.assertIn("define dso_local hidden i32 @helper", cold)
        self.assertIn("define internal void @local", cold)
        self.assertIn("@table = external dso_local hidden constant [2 x i32]", hot)
        self.assertIn("@table = dso_local hidden unnamed_addr constant", cold)
        self.assertIn("@unused = private constant i8 1", cold)
        self.assertNotIn("[i32 3, i32 5]", hot)
        self.assertNotIn("available_externally", hot)
        self.assertEqual(membership["constants"], ["table", "unused"])

    def test_cold_size_attributes_and_signature_alignment(self):
        hot, cold, _ = partition.partition(IR, ["hot"], [])
        self.assertIn("attributes #0 = { minsize optsize nounwind }", cold)
        self.assertNotIn("minsize", hot)
        self.assertIn("ptr align 8 %p", cold)
        self.assertNotIn("#0 align 2", cold)

    def test_exact_promotion_and_missing_group_rejected(self):
        hot, _, membership = partition.partition(IR, ["hot"], ["local"])
        self.assertIn("local", partition.definitions(hot))
        self.assertEqual(membership["promotions"], ["local"])
        for groups, promotions in ((["absent"], []), (["hot"], ["absent"]),
                                   (["hot"], ["local", "local"]), ([], [])):
            with self.subTest(groups=groups, promotions=promotions), self.assertRaises(ValueError):
                partition.partition(IR, groups, promotions)

    def test_unsupported_ir_rejected(self):
        inputs = [IR + line + "\n" for line in (
            "@llvm.global_ctors = appending global [0 x ptr] []",
            "@llvm.global_dtors = appending global [0 x ptr] []",
            "@data = hidden global i32 0", "@data = external global i32",
            "@alias = alias i32, ptr @table", "@f = ifunc i32 (), ptr @resolver",
            "@tls = thread_local global i32 0", "; blockaddress(@f, %label)",
            "!llvm.dbg.cu = !{}",
        )]
        inputs += [IR + "@unused = constant i8 0\n", IR + partition.definitions(IR)["local"],
                   IR.replace(" #0 align 2 {", " #0 !dbg !1 {"),
                   IR.replace("define internal void @local() #0 {", "define internal void @local(\n) #0 {"),
                   IR.replace("  ret void\n}\n", "  ret void\n"), IR.rstrip("\n")]
        for text in inputs:
            with self.subTest(text=text[-100:]), self.assertRaises(ValueError):
                partition.partition(text, ["hot"], [])

    def test_global_types_and_quoted_symbols(self):
        for value, expected in (("[2 x [3 x i8]] zeroinitializer", "[2 x [3 x i8]]"),
                                ("<{ i8, i32 }> zeroinitializer", "<{ i8, i32 }>"),
                                ('%"a type" zeroinitializer', '%"a type"'), ("i8 1", "i8")):
            self.assertEqual(partition.constant_type(value), expected)
        self.assertEqual(partition.symbol('define i32 @"a function"() {'), '"a function"')
        for value in ("", "[2 x i8", "[2 x i8}", "token none"):
            with self.assertRaises(ValueError):
                partition.constant_type(value)

    def test_frozen_selection_and_explicit_provider_mapping(self):
        config = builder.load_selection()
        self.assertEqual(len(config["promotions"]), 229)
        self.assertEqual(len(config["hot_groups"]), 12)
        self.assertEqual(config["external_promotions"], {"_ZL7importsv": "g2_h264_runtime_current"})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "selection.json"
            for key, replacement in (("schema", 2), ("promotions", ["x"]),
                                     ("hot_groups", ["x", "x"]),
                                     ("external_promotions", {"absent": "target"})):
                invalid = {**config, key: replacement}
                path.write_text(json.dumps(invalid), encoding="utf-8")
                with self.subTest(key=key), self.assertRaises(ValueError):
                    builder.load_selection(path)

    def test_provider_mapping_cannot_hide_missing_definition(self):
        config = {"promotions": ["local", "old_getter"],
                  "external_promotions": {"old_getter": "provider"}}
        mapping, external = builder.resolve_promotions(IR + "declare ptr @provider()\n", config)
        self.assertEqual(mapping, {"local": "local"})
        self.assertEqual(external, {"old_getter": "provider"})
        for text in (IR, IR + "define ptr @provider() {\n  ret ptr null\n}\n"):
            with self.assertRaises(ValueError):
                builder.resolve_promotions(text, config)
        config["promotions"] = ["absent"]
        config["external_promotions"] = {}
        with self.assertRaises(ValueError):
            builder.resolve_promotions(IR, config)

    def test_native_size_mapping_preserves_long_long(self):
        self.assertEqual(builder.canonical_native_type("array<int, 16ul> size(unsigned long)"),
                         "array<int, 16u> size(unsigned int)")
        self.assertEqual(builder.canonical_native_type("f(unsigned long long, long long)"),
                         "f(unsigned long long, long long)")

    def test_frontend_optimization_and_lto_cannot_change_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            for flags in (["-Oz"], ["-O2", "-Oz"], ["-O2", "-flto"], []):
                with self.subTest(flags=flags), self.assertRaises(ValueError):
                    builder.compile_modules(directory, flags)

    def test_selection_hash_uses_the_effective_configuration(self):
        selections = [{"hot_groups": ["hot"], "promotions": [], "external_promotions": {}},
                      {"hot_groups": ["hot"], "promotions": ["helper"], "external_promotions": {}}]
        def compile_ir(command, check):
            if "-emit-llvm" in command:
                Path(command[-1]).write_text(IR, encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(builder.subprocess, "run", side_effect=compile_ir), \
                mock.patch.object(builder, "load_selection", side_effect=AssertionError("default selection read")):
            hashes = []
            for config in selections:
                _, membership = builder.compile_modules(directory, ["-O2"], selection=config)
                expected = hashlib.sha256(json.dumps(config, sort_keys=True,
                                                     separators=(",", ":")).encode("utf-8")).hexdigest()
                self.assertEqual(membership["selection_sha256"], expected)
                hashes.append(expected)
                reordered = dict(reversed(list(config.items())))
                _, same = builder.compile_modules(directory, ["-O2"], selection=reordered)
                self.assertEqual(same["selection_sha256"], expected)
            self.assertNotEqual(*hashes)

    def test_existing_cold_helper_attributes_are_preserved(self):
        source = IR.replace("{ nounwind }", "{ minsize optsize nounwind }")
        hot, cold, _ = partition.partition(source, ["hot"], [])
        self.assertIn("{ minsize optsize nounwind }", hot)
        self.assertIn("nounwind", cold)

    def test_ambiguous_native_mapping_is_rejected(self):
        config = {"promotions": ["arm_only"], "external_promotions": {}}
        def demangle(command, input, text):
            spellings = {"arm_only": "f(unsigned int)", "helper": "f(unsigned long)",
                         "local": "f(unsigned int)", "sub0h264_hot": "hot()"}
            return "\n".join(spellings[name] for name in input.splitlines()) + "\n"
        with mock.patch.object(builder.subprocess, "check_output", side_effect=demangle):
            with self.assertRaisesRegex(ValueError, "uniquely"):
                builder.resolve_promotions(IR, config, native=True)


class PartitionBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("clang", "clang++", "ld.lld", "arm-none-eabi-g++", "c++filt"):
            if not shutil.which(tool):
                raise RuntimeError("required partition test tool missing: " + tool)
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.headers = builder.toolchain.discover_headers()
        cls.arm = cls.directory / "arm"
        cls.manifest = builder.build_decoder(cls.arm, cls.headers)
        cls.membership = json.loads((cls.arm / "membership.json").read_text(encoding="utf-8"))
        vendor = ROOT / "third-party/sub0h264/components/sub0h264"
        native_flags = ["-std=c++23", "-O2", "-DNDEBUG", "-fPIC", "-fvisibility=hidden",
                        "-fvisibility-inlines-hidden", "-fno-exceptions", "-fno-rtti",
                        "-fno-threadsafe-statics", "-fno-builtin", "-fno-jump-tables",
                        "-ffp-contract=off", "-I" + str(vendor / "include"), "-I" + str(vendor / "src")]
        for header in cls.headers[:2]:
            native_flags += ["-isystem", str(header)]
        sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        source = ROOT / "tests/h264/partition_probe.cpp"
        objects, cls.native_membership = builder.compile_modules(cls.directory / "native", native_flags,
                                                                 source, native=True, object_flags=sanitize)
        runtime = cls.directory / "runtime.o"
        subprocess.run(["clang", "-O2", "-fno-builtin", *sanitize, "-c", str(HERE / "runtime.c"),
                        "-o", str(runtime)], check=True)
        cls.mixed, cls.whole = cls.directory / "mixed", cls.directory / "whole"
        driver = ROOT / "tests/h264/partition_smoke.cpp"
        subprocess.run(["clang++", *native_flags, *sanitize, *map(str, objects),
                        str(runtime), str(driver), "-o", str(cls.mixed)], check=True)
        subprocess.run(["clang++", *native_flags, *sanitize, str(source), str(runtime),
                        str(driver), "-o", str(cls.whole)], check=True)

    def test_real_arm_selection_and_table_ownership(self):
        self.assertEqual(self.manifest["name"], "h264-decoder")
        self.assertEqual(len(self.membership["promotion_mapping"]), 228)
        self.assertEqual(sum(v == "hot" for v in self.membership["functions"].values()), 253)
        hot = (self.arm / "hot.ll").read_text(encoding="utf-8")
        cold = (self.arm / "cold.ll").read_text(encoding="utf-8")
        self.assertIn("available_externally", hot)
        self.assertNotIn("ds_profile", hot + cold)
        self.assertIn("DecodeTrace", hot)
        for name in self.membership["constants"]:
            self.assertRegex(hot, r"(?m)^@" + re.escape(name) + " = available_externally ")
            self.assertEqual(len(re.findall(r"(?m)^@" + re.escape(name) + " = ", cold)), 1)

    def object_symbols(self, path):
        data, sections = builder.linker.parse_elf(path)
        table = builder.linker.section(sections, ".symtab")
        strings = sections[table["link"]]
        result = []
        for offset in range(table["offset"], table["offset"] + table["size"], 16):
            name, value, size, info, other, index = struct.unpack_from("<IIIBBH", data, offset)
            if info & 15 == 1 and index:
                result.append(data[strings["offset"] + name:].split(b"\0", 1)[0].decode())
        return result, sections

    def assert_constant_copies(self, path):
        cold, _ = self.object_symbols(self.arm / "cold.o")
        closed, _ = self.object_symbols(self.arm / "closed.o")
        linked, _ = self.object_symbols(path)
        retained = 0
        for name in self.membership["constants"]:
            aliases = {name, ".L" + name}
            counts = [sum(symbol in aliases for symbol in objects) for objects in (cold, closed, linked)]
            self.assertTrue(all(count <= 1 for count in counts), (name, counts))
            if not name.startswith("."):
                self.assertEqual(counts[1], counts[2], name)
                if counts[1]:
                    self.assertEqual(counts[0], 1, name)
                    retained += 1
        self.assertGreater(retained, 0)

    def test_optimizer_only_constants_emit_no_hot_storage(self):
        objects, sections = self.object_symbols(self.arm / "hot.o")
        self.assertEqual(objects, [])
        self.assertFalse([s["sname"] for s in sections if s["size"] and
                          s["sname"].startswith((".rodata", ".data", ".bss"))])
        self.assert_constant_copies(self.arm / "closed.o")

    def test_real_native_promotions_map_uniquely(self):
        mapping = self.native_membership["promotion_mapping"]
        self.assertEqual(len(mapping), 228)
        self.assertEqual(len(set(mapping.values())), 228)
        self.assertEqual(sum(v == "hot" for v in self.native_membership["functions"].values()), 253)

    def test_independent_lld_at_three_addresses(self):
        blob = (self.arm / "decoder.bin").read_bytes()
        for address in (0x7BEA64, 0x7C0000, 0x20275000):
            lines = ["SECTIONS {", f".blob 0x{address:x} : {{", "FILL(0);"]
            for name, offset, size in self.manifest["layout"]:
                lines += [f". = ADDR(.blob) + {offset};", f"KEEP(*({name}))"]
            lines += ["}", "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment)",
                      "*(.note*) *(.ARM.attributes) *(.llvm_addrsig) }", "}"]
            script = self.directory / f"lld-{address:x}.ld"
            script.write_text("\n".join(lines) + "\n", encoding="utf-8")
            elf = script.with_suffix(".elf")
            subprocess.run(["ld.lld", "--no-relax", "--entry=0", "-T", str(script),
                            str(self.arm / "closed.o"), "-o", str(elf)], check=True)
            data, sections = builder.linker.parse_elf(elf)
            section = builder.linker.section(sections, ".blob")
            self.assertEqual(blob, data[section["offset"]:section["offset"] + section["size"]])
            self.assert_constant_copies(elf)

    def test_two_clean_builds_are_identical(self):
        second = self.directory / "second"
        manifest = builder.build_decoder(second, self.headers)
        self.assertEqual(manifest, self.manifest)
        for name in ("preopt.ll", "hot.ll", "cold.ll", "hot.o", "cold.o",
                     "closed.o", "decoder.bin", "membership.json"):
            self.assertEqual((self.arm / name).read_bytes(), (second / name).read_bytes(), name)
        print(f"Decoder repeat build: {manifest['bytes']} B, SHA256 {manifest['sha256']}")

    def test_fixture_and_extra_clip_y_bytes_match_whole_decoder(self):
        fixtures = ROOT / "third-party/sub0h264/tests/fixtures"
        clips = [p for p in (fixtures / "flat_black_baseline_640x480.h264",
                              fixtures / "g2_10s_700k.h264") if p.is_file()]
        extra = os.environ.get("G2_H264_TEST_CLIPS", "")
        clips += [Path(path) for path in extra.split(os.pathsep) if path]
        if not clips:
            self.skipTest("optional ignored fixtures not installed")
        for clip in clips:
            with self.subTest(clip=clip.name):
                outputs = [subprocess.run([str(exe), str(clip)], capture_output=True, check=True)
                           for exe in (self.mixed, self.whole)]
                self.assertEqual(outputs[0].stdout, outputs[1].stdout)
                self.assertEqual(outputs[0].stderr, outputs[1].stderr)
                data, offset, frames = outputs[0].stdout, 0, 0
                while offset < len(data):
                    number, width, height = struct.unpack_from("<III", data, offset)
                    frames += 1
                    self.assertEqual(number, frames)
                    offset += 12 + width * height
                self.assertEqual(offset, len(data))
                self.assertGreater(frames, 0)
                print(f"Partition Y bytes PASS: {clip.name}, {frames} pictures; "
                      + outputs[0].stderr.decode().strip() + "; " + hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    unittest.main()
