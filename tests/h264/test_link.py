"""Compare ARM PIC fixups with LLD and reject objects needing runtime support.

Run from the repository root: python3 -m unittest discover -s tests/h264 -v.
Requires clang and ld.lld with ARM support. All fixtures are generated in a
TemporaryDirectory; no firmware, decoder fixtures or machine paths are needed.
"""
from pathlib import Path
import importlib.util
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("firmware_build", ROOT / "patches/build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

BRANCHES = """
.syntax unified
.thumb
.section .text.first,"ax",%progbits
.global first
.type first,%function
.thumb_func
first:
    bl target+2
    bl target-2
    b.w target+2
.size first,.-first
.section .text.target,"ax",%progbits
    nop
.global target
.type target,%function
.thumb_func
target:
    nop
    nop
    bl first
    b.w first+4
    bx lr
.size target,.-target
"""
RELATIVE_POINTERS = """
.syntax unified
.thumb
.section .text.target,"ax",%progbits
.global target
.type target,%function
.thumb_func
target:
    bx lr
.size target,.-target
.section .rodata.table,"a",%progbits
.p2align 2
.global table
table:
    .word target-.
    .word target+4-.
    .word target-4-.
    .word target+1-.
    .word table+3-.
    .word table-9-.
"""


class LinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("clang", "ld.lld"):
            if not shutil.which(tool):
                raise RuntimeError(f"required ARM tool is missing: {tool}")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.here = Path(self.temp.name)

    def compile(self, source, language="c"):
        src = self.here / ("fixture." + language)
        src.write_text(source, encoding="utf-8")
        obj = self.here / "fixture.o"
        flags = build.CFLAGS
        if language == "s":
            flags = ["--target=thumbv7em-none-eabi", "-mthumb"]
        elif language == "cpp":
            flags = [f for f in build.CFLAGS if f not in ("-ffreestanding", "-fropi")]
            flags += ["-std=c++23", "-fPIC", "-fvisibility=hidden", "-fno-exceptions",
                      "-fno-rtti", "-ffunction-sections", "-fdata-sections"]
        subprocess.run(["clang", *flags, "-c", str(src), "-o", str(obj)],
                       check=True, capture_output=True)
        return obj

    def compare_lld(self, obj, addresses=(0x7BEA80, 0x438000, 0x20275000)):
        blob, funcs, layout = build.link_pic_object(obj)
        for address in addresses:
            with self.subTest(address=hex(address)):
                lines = ["SECTIONS {", f".blob 0x{address:x} : {{", "FILL(0);"]
                for name, offset, size in layout:
                    lines += [f". = ADDR(.blob) + {offset};", f"KEEP(*({name}))"]
                lines += ["}", "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*)",
                          "*(.comment) *(.note*) *(.ARM.attributes) *(.llvm_addrsig) }", "}"]
                script = self.here / f"{address:x}.ld"
                script.write_text("\n".join(lines) + "\n", encoding="utf-8")
                elf = script.with_suffix(".elf")
                subprocess.run(["ld.lld", "--no-relax", "--entry=0", "-T", str(script),
                                str(obj), "-o", str(elf)], check=True, capture_output=True)
                raw, secs = build.parse_elf(elf)
                section = build.section(secs, ".blob")
                self.assertEqual(blob, raw[section["offset"]:section["offset"] + section["size"]])
        return blob, funcs, layout

    def mutate(self, obj, change):
        raw, secs = build.parse_elf(obj)
        mutable = bytearray(raw)
        change(mutable, secs)
        obj.write_bytes(mutable)
        return obj

    def section_header(self, raw, index):
        return struct.unpack_from("<I", raw, 0x20)[0] + index * 40

    def rel32_object(self):
        return self.compile(RELATIVE_POINTERS, "s")

    def test_c_only_rodata_and_function_pointers(self):
        obj = self.compile("""
__attribute__((noinline)) int other(int n) { return n + 1; }
const char text[] = "read-only data";
int (*entry(void))(int) { return other; }
const char *data(void) { return text; }
""")
        self.compare_lld(obj, (0x7BEA64, 0x438000, 0x20275000))

    def test_forward_backward_and_addended_bl_and_bw(self):
        self.compare_lld(self.compile(BRANCHES, "s"))

    def test_rel32_signed_addends_and_thumb_bit(self):
        blob, funcs, layout = self.compare_lld(self.rel32_object())
        offset = next(off for name, off, size in layout if name == ".rodata.table")
        self.assertEqual(struct.unpack_from("<I", blob, offset)[0] & 1, 1)

    def test_cpp_pic_multiple_sections(self):
        self.compare_lld(self.compile("""
__attribute__((noinline)) int helper(int n) { return n * 3; }
__attribute__((noinline)) int entry(int n) { return helper(n) + 7; }
const int values[] = {3, 7, 11, 17};
const int *table() { return values; }
int (*function())(int) { return helper; }
""", "cpp"))

    def test_section_alignment_and_zero_padding(self):
        obj = self.compile("""
.syntax unified
.thumb
.section .text.first,"ax",%progbits
.global entry
.type entry,%function
.thumb_func
entry:
    bx lr
.size entry,.-entry
.section .text.last,"ax",%progbits
.p2align 4
    bx lr
.section .rodata.table,"a",%progbits
.p2align 5
    .word 0x12345678
""", "s")
        blob, funcs, layout = self.compare_lld(obj)
        self.assertEqual(next(off for name, off, size in layout if name == ".text.last") % 16, 0)
        offset = next(off for name, off, size in layout if name == ".rodata.table")
        self.assertEqual(offset % 32, 0)
        self.assertEqual(blob[2:16], b"\0" * 14)
        self.assertEqual(blob[18:32], b"\0" * 14)

    def test_zero_sized_function_symbols_get_section_bounded_sizes(self):
        obj = self.compile("""
.syntax unified
.thumb
.section .text.first,"ax",%progbits
.global first
.type first,%function
.thumb_func
first:
    nop
    bx lr
.global second
.type second,%function
.thumb_func
second:
    bx lr
.section .text.last,"ax",%progbits
.global last
.type last,%function
.thumb_func
last:
    bx lr
""", "s")
        blob, funcs, layout = self.compare_lld(obj)
        sizes = {name: size for name, off, size in funcs}
        self.assertEqual(sizes, {"first": 4, "second": 2, "last": 2})

    def test_writable_globals_rejected(self):
        with self.assertRaisesRegex(build.BuildError, "forbidden allocated section"):
            build.link_pic_object(self.compile("int counter; int next(void) { return ++counter; }"))

    def test_undefined_function_rejected(self):
        with self.assertRaisesRegex(build.BuildError, "undefined symbol"):
            build.link_pic_object(self.compile("extern int outside(int); int entry(int n) { return outside(n); }"))

    def test_absolute_data_and_function_pointers_rejected(self):
        for declaration in (".word target", ".word table"):
            with self.subTest(declaration=declaration):
                obj = self.compile(RELATIVE_POINTERS.replace(".word target-.", declaration), "s")
                with self.assertRaisesRegex(build.BuildError, "unsupported relocation"):
                    build.link_pic_object(obj)

    def test_got_and_startup_sections_rejected(self):
        for name in (".got", ".init", ".init_array", ".fini_array", ".ctors", ".dtors", ".ARM.extab"):
            with self.subTest(section=name):
                obj = self.compile(f'.section {name},"a",%progbits\n.word 0\n', "s")
                with self.assertRaises(build.BuildError):
                    build.link_pic_object(obj)

    def test_real_unwind_recipe_rejected(self):
        obj = self.compile("int entry(int n) { return n + 1; }")
        def change(raw, secs):
            section = next(s for s in secs if s["sname"].startswith(".ARM.exidx"))
            struct.pack_into("<I", raw, section["offset"] + 4, 0x80000000)
        with self.assertRaisesRegex(build.BuildError, "unwind tables"):
            build.link_pic_object(self.mutate(obj, change))

    def test_unknown_allocated_section_rejected(self):
        obj = self.rel32_object()
        def change(raw, secs):
            i = next(i for i, s in enumerate(secs) if s["sname"] == ".rodata.table")
            struct.pack_into("<I", raw, self.section_header(raw, i) + 4, 7)  # SHT_NOTE.
        with self.assertRaisesRegex(build.BuildError, "unknown allocated section"):
            build.link_pic_object(self.mutate(obj, change))

    def test_invalid_alignment_rejected(self):
        obj = self.rel32_object()
        def change(raw, secs):
            i = next(i for i, s in enumerate(secs) if s["sname"] == ".rodata.table")
            struct.pack_into("<I", raw, self.section_header(raw, i) + 32, 3)
        with self.assertRaisesRegex(build.BuildError, "invalid section alignment"):
            build.link_pic_object(self.mutate(obj, change))

    def test_unsupported_relocation_rejected(self):
        obj = self.rel32_object()
        def change(raw, secs):
            rel = next(s for s in secs if s["sname"] == ".rel.rodata.table")
            info = struct.unpack_from("<I", raw, rel["offset"] + 4)[0]
            struct.pack_into("<I", raw, rel["offset"] + 4, (info & ~255) | 42)  # PREL31.
        with self.assertRaisesRegex(build.BuildError, "unsupported relocation"):
            build.link_pic_object(self.mutate(obj, change))

    def test_relocation_site_outside_section_rejected(self):
        obj = self.rel32_object()
        def change(raw, secs):
            rel = next(s for s in secs if s["sname"] == ".rel.rodata.table")
            struct.pack_into("<I", raw, rel["offset"], secs[rel["info"]]["size"] - 3)
        with self.assertRaisesRegex(build.BuildError, "relocation outside"):
            build.link_pic_object(self.mutate(obj, change))

    def test_invalid_relocation_symbol_rejected(self):
        obj = self.rel32_object()
        def change(raw, secs):
            rel = next(s for s in secs if s["sname"] == ".rel.rodata.table")
            struct.pack_into("<I", raw, rel["offset"] + 4, 0xFFFF0003)
        with self.assertRaisesRegex(build.BuildError, "invalid relocation symbol"):
            build.link_pic_object(self.mutate(obj, change))

    def test_non_rel_relocation_table_rejected(self):
        obj = self.rel32_object()
        def change(raw, secs):
            i = next(i for i, s in enumerate(secs) if s["sname"] == ".rel.rodata.table")
            struct.pack_into("<I", raw, self.section_header(raw, i) + 4, build.SHT_RELA)
        with self.assertRaisesRegex(build.BuildError, "expected ARM ELF REL"):
            build.link_pic_object(self.mutate(obj, change))

    def test_branch_overflow_rejected(self):
        blob = bytearray(b"\x00\xf0\x00\xf8")
        with self.assertRaisesRegex(build.BuildError, "out of Thumb range"):
            build.resolve_thumb_branch(blob, 0, 1 << 25)

    def test_wrong_machine_or_linked_elf_rejected(self):
        for offset, value in ((18, 3), (16, 2)):
            with self.subTest(header_offset=offset):
                obj = self.rel32_object()
                self.mutate(obj, lambda raw, secs: struct.pack_into("<H", raw, offset, value))
                with self.assertRaisesRegex(build.BuildError, "ARM relocatable"):
                    build.link_pic_object(obj)

    def test_truncated_object_rejected(self):
        obj = self.rel32_object()
        obj.write_bytes(obj.read_bytes()[:60])
        with self.assertRaisesRegex(build.BuildError, "invalid section table"):
            build.link_pic_object(obj)


if __name__ == "__main__":
    unittest.main()
