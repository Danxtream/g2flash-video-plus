"""Test the inert C interface, ARM closure and runtime under ASan/UBSan.

SPDX-License-Identifier: GPL-3.0-only
Run from the root with python3 -m unittest discover -s tests/h264 -v.
All generated objects/executables live in temporary directories.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / "patches/h264"
VENDOR = ROOT / "third-party/sub0h264/components/sub0h264"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


toolchain = load_module("h264_toolchain", HERE / "toolchain.py")
build = load_module("h264_firmware_build", ROOT / "patches/build.py")
EXPORTS = [
    "g2_h264_size", "g2_h264_alignment", "g2_h264_init", "g2_h264_destroy",
    "g2_h264_decode", "g2_h264_frame",
]


def run(command, **kwargs):
    """Expose compiler/sanitizer diagnostics on failure, never hide a fallback."""
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{command}\n{result.stdout}\n{result.stderr}")
    return result


class HeaderTests(unittest.TestCase):
    def test_gcc_search_list_only(self):
        parsed = toolchain.include_search_paths(
            "diagnostic\n#include <...> search starts here:\n /headers\n /multilib\n"
            "End of search list.\ncompiler diagnostic")
        self.assertEqual(parsed, [Path("/headers"), Path("/multilib")])

    def test_missing_search_list_rejected(self):
        with self.assertRaises(RuntimeError):
            toolchain.include_search_paths("a diagnostic, not a header list")

    def test_override_requires_matching_multilib_and_c_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cxx, c = root / "cxx", root / "c"
            multilib = cxx / "arm-none-eabi" / toolchain.MULTILIB / "bits"
            multilib.mkdir(parents=True)
            c.mkdir()
            (cxx / "vector").touch()
            (multilib / "c++config.h").touch()
            (c / "stdlib.h").touch()
            with self.assertRaises(RuntimeError):
                toolchain.validate_headers(cxx, c)
            (c / "stdio.h").touch()
            self.assertEqual(toolchain.discover_headers(cxx_root=cxx, c_root=c),
                             [cxx, multilib.parent, c])
            with self.assertRaises(ValueError):
                toolchain.discover_headers(cxx_root=cxx)

    def test_decoder_flags_exclude_cpp_freestanding(self):
        flags = toolchain.compiler_flags([], "-Oz")
        self.assertNotIn("-ffreestanding", flags)
        for flag in ("-std=c++23", "-fPIC", "-DNDEBUG", "-fno-exceptions",
                     "-fno-rtti", "-mfloat-abi=softfp", "-ffp-contract=off"):
            self.assertIn(flag, flags)
        with self.assertRaises(ValueError):
            toolchain.compiler_flags([], "-O3")


class WrapperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("clang", "clang++", "ld.lld", "arm-none-eabi-g++"):
            if not shutil.which(tool):
                raise RuntimeError(f"required interface test tool missing: {tool}")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.headers = toolchain.discover_headers()
        cls.native_flags = [
            "-std=c++23", "-O2", "-g", "-DNDEBUG", "-fno-exceptions", "-fno-rtti",
            "-fno-builtin", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I" + str(VENDOR / "src"),
            "-I" + str(VENDOR / "include"),
        ]
        cls.runtime = cls.directory / "runtime-host.o"
        run(["clang", "-O2", "-g", "-fno-builtin", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-c", str(HERE / "runtime.c"), "-o", str(cls.runtime)])
        cls.smoke = cls.directory / "wrapper-smoke"
        run(["clang++", *cls.native_flags, str(HERE / "g2_h264.cpp"),
             str(ROOT / "tests/h264/wrapper_smoke.cpp"), str(cls.runtime),
             "-o", str(cls.smoke)])

    def test_sanitized_lifecycle_nals_i_p_and_preflight_refusal(self):
        result = run([str(self.smoke)])
        self.assertIn("C interface PASS", result.stdout)
        print(result.stdout, end="")

    def test_fatal_allocation_calls_nonreturning_owner(self):
        result = subprocess.run([str(self.smoke), "--fatal"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 71, result.stderr)

    def test_fixture_y_matches_unwrapped_decoder(self):
        fixture = ROOT / "third-party/sub0h264/tests/fixtures/flat_black_baseline_640x480.h264"
        if not fixture.is_file():
            self.skipTest("optional ignored decoder fixtures are not installed")
        result = run([str(self.smoke), str(fixture)])
        self.assertIn("all Y bytes identical", result.stdout)
        print(result.stdout, end="")

    def test_default_runtime_refuses_initialization(self):
        source = self.directory / "unbound.cpp"
        source.write_text("""
#include "g2_h264.h"
#include <cstdlib>
int main() {
    void *memory = std::malloc(g2_h264_size());
    void *handle = g2_h264_init(memory, g2_h264_size());
    std::free(memory);
    return handle != nullptr;
}
""", encoding="utf-8")
        exe = self.directory / "unbound"
        run(["clang++", *self.native_flags, "-I" + str(HERE),
             str(source), str(HERE / "g2_h264.cpp"), str(self.runtime), "-o", str(exe)])
        run([str(exe)])

    def test_sanitized_runtime_shift_and_memory_helpers(self):
        exe = self.directory / "runtime-smoke"
        run(["clang", "-O2", "-g", "-fno-builtin", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", str(ROOT / "tests/h264/runtime_smoke.c"),
             str(self.runtime), "-o", str(exe)])
        self.assertIn("EABI helpers PASS", run([str(exe)]).stdout)

    def test_header_is_a_c_interface(self):
        source = self.directory / "header.c"
        source.write_text("""
#include "g2_h264.h"
#include "runtime.h"
_Static_assert(G2_H264_MAX_NAL_BYTES + 10 == G2_H264_RECORD_BYTES, "record overhead");
g2_h264_result decode(void *p, const uint8_t *n, uint32_t s) {
    return g2_h264_decode(p, n, s);
}
""", encoding="utf-8")
        run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(HERE),
             "-c", str(source), "-o", str(self.directory / "header.o")])

    def test_decoder_config_keeps_callback_trace(self):
        result = run(["clang++", *toolchain.compiler_flags(self.headers),
                      "-E", "-dM", str(HERE / "g2_h264.cpp")])
        for macro in ("SUB0H264_TRACE 0", "SUB0H264_MAX_SPS_COUNT 1U",
                      "SUB0H264_MAX_PPS_COUNT 1U", "SUB0H264_DISABLE_LEGACY_CURRENT_FRAME 1"):
            self.assertIn("#define " + macro, result.stdout)
        ir = self.directory / "preopt.ll"
        run(["clang++", *toolchain.compiler_flags(self.headers), "-Xclang",
             "-disable-llvm-passes", "-S", "-emit-llvm", str(HERE / "g2_h264.cpp"),
             "-o", str(ir)])
        text = ir.read_text(encoding="utf-8")
        self.assertIn("DecodeTrace", text)
        self.assertNotIn("steady_clock", text)
        self.assertNotIn("ds_profile", text)

    def test_arm_o2_and_oz_closure_and_independent_lld(self):
        for optimization in ("-O2", "-Oz"):
            with self.subTest(optimization=optimization):
                here = self.directory / optimization[1:]
                here.mkdir()
                wrapper, runtime, memory, closed = [here / name for name in
                                                    ("wrapper.o", "runtime.o", "memory.o", "closed.o")]
                run(["clang++", *toolchain.compiler_flags(self.headers, optimization),
                     "-ffunction-sections", "-fdata-sections", "-c",
                     str(HERE / "g2_h264.cpp"), "-o", str(wrapper)])
                for source, obj in ((HERE / "runtime.c", runtime),
                                    (ROOT / "patches/memory.c", memory)):
                    run(["clang", *build.CFLAGS, "-ffunction-sections", "-fdata-sections",
                         "-c", str(source), "-o", str(obj)])
                roots = [arg for name in EXPORTS for arg in ("-u", name)]
                run(["ld.lld", "-r", "--gc-sections", *roots, str(wrapper),
                     str(runtime), str(memory), "-o", str(closed)])
                blob, functions, layout = build.link_pic_object(closed)
                for name in EXPORTS:
                    self.assertIn(name, [function[0] for function in functions])
                for address in (0x7BEA64, 0x7C0000, 0x20275000):
                    lines = ["SECTIONS {", f".blob 0x{address:x} : {{", "FILL(0);"]
                    for name, offset, size in layout:
                        lines += [f". = ADDR(.blob) + {offset};", f"KEEP(*({name}))"]
                    lines += ["}", "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*)",
                              "*(.comment) *(.note*) *(.ARM.attributes) *(.llvm_addrsig) }", "}"]
                    script = here / f"{address:x}.ld"
                    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
                    elf = script.with_suffix(".elf")
                    run(["ld.lld", "--no-relax", "--entry=0", "-T", str(script),
                         str(closed), "-o", str(elf)])
                    raw, sections = build.parse_elf(elf)
                    loaded = build.section(sections, ".blob")
                    self.assertEqual(blob, raw[loaded["offset"]:loaded["offset"] + loaded["size"]])
                print(f"ARM {optimization}: {len(blob)} B closed; three independent LLD matches")


if __name__ == "__main__":
    unittest.main()
