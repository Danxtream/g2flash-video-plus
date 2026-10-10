"""Check inert decoder inclusion, byte guards, placement and firmware limits.

SPDX-License-Identifier: GPL-3.0-only
Run in Ubuntu with the other tests in this directory. The real-image checks
use the optional ignored base image; all generated files stay temporary.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "patches"))
import patch_compress as firmware

spec = importlib.util.spec_from_file_location("decoder_build", ROOT / "patches/h264/build_decoder.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class DecoderBlobTests(unittest.TestCase):
    def test_decoder_build_and_manifest_validation(self):
        blob = bytes.fromhex("7047")
        manifest = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
                    "alignment": 4}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            patches = root / "patches"
            output = root / "obj/h264"
            output.mkdir(parents=True)
            (output / "firmware.bin").write_bytes(blob)
            path = output / "firmware.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with mock.patch.object(firmware, "SCRIPT_DIR", str(patches)), \
                    mock.patch.object(firmware.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "", "")
                self.assertEqual(firmware.build_firmware_blob(), {**manifest, "text": blob.hex()})
                self.assertEqual(run.call_args.args[0],
                                 [sys.executable, str(patches / "h264/build_decoder.py"),
                                  "--output", str(output), "--firmware-c", str(patches / "patches_main.c")])
                for key, value in (("bytes", 1), ("sha256", "0" * 64),
                                   ("alignment", 0), ("alignment", 3), ("alignment", 8)):
                    path.write_text(json.dumps({**manifest, key: value}), encoding="utf-8")
                    with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                        firmware.build_firmware_blob()
                run.return_value = subprocess.CompletedProcess([], 1, "", "compiler failed")
                with self.assertRaisesRegex(SystemExit, "compiler failed"):
                    firmware.build_firmware_blob()


class FirmwareInclusionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base_path = ROOT / "g2_2.2.9.22.bin"
        if not cls.base_path.is_file():
            raise unittest.SkipTest("optional ignored base firmware not installed")
        cls.base = cls.base_path.read_bytes()
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.manifest = builder.build_firmware(cls.directory, ROOT / "patches/patches_main.c")
        cls.blob = (cls.directory / "firmware.bin").read_bytes()
        cls.built = {**cls.manifest, "text": cls.blob.hex()}

    def layout(self, ceiling=None):
        if ceiling is None:
            ceiling = firmware.APP_MAX_END
        with mock.patch.object(firmware, "build_firmware_blob", return_value=self.built), \
                mock.patch.object(firmware, "APP_MAX_END", ceiling), \
                contextlib.redirect_stdout(io.StringIO()):
            return firmware.layout(self.base)

    def test_combined_bytes_exports_and_stock_patch_guards(self):
        append, patches, (_, _, old_size) = self.layout()
        self.assertEqual(len(patches), 33)
        guards = {offset: old for offset, old, _, _ in patches}
        for address, old in ((0x4abfd0, "0168491e"),
                             (0x445c74, "fef711f9"),
                             (0x445c86, "fef708f9")):
            self.assertEqual(bytes.fromhex(guards[firmware.g2f(address)]), bytes.fromhex(old))
        start = firmware.align_up(old_size, firmware.BLOB_ALIGN) - old_size
        self.assertEqual(append[start:], self.blob)
        self.assertEqual(hashlib.sha256(append[start:]).hexdigest(), self.manifest["sha256"])
        self.assertEqual(firmware.mram_addr(old_size + start) % self.manifest["alignment"], 0)
        names = [item["name"] for item in self.manifest["functions"]]
        for name in (*builder.EXPORTS, *builder.WORKER_EXPORTS, "g2_h264_runtime_current",
                     "memcpy", "memmove", "memset"):
            self.assertEqual(names.count(name), 1, name)
        self.assertIn(".text.video", [item[0] for item in self.manifest["layout"]])
        symbols = subprocess.check_output(["arm-none-eabi-nm", str(self.directory / "firmware-closed.o")], text=True)
        self.assertRegex(symbols, r"(?m)^[0-9a-f]+ T g2_h264_runtime_current$")
        self.assertNotRegex(symbols, r"(?m)^[0-9a-f]+ W g2_h264_runtime_current$")
        for offset, old, new, description in patches:
            expected = bytes.fromhex(old)
            self.assertEqual(self.base[offset:offset + len(expected)], expected, description)

    def test_ceiling_includes_the_decoder_and_accepts_the_exact_end(self):
        append, _, (_, _, old_size) = self.layout()
        end = firmware.mram_addr(old_size + len(append))
        self.assertLessEqual(end, firmware.APP_MAX_END)
        self.layout(ceiling=end)
        with self.assertRaisesRegex(SystemExit, "past the safe ceiling"):
            self.layout(ceiling=end - 1)

    def test_stock_byte_and_tlsf_tail_guards_remain_active(self):
        damaged = bytearray(self.base)
        site, old = firmware.MESSAGE_RX_BL_SITE
        offset = firmware.g2f(site)
        damaged[offset] ^= 1
        with self.assertRaises(ValueError):
            firmware.build_patch_ops(bytes(damaged))
        # Unknown pointers into the reserved tail must fail before compilation.
        struct.pack_into("<I", damaged, 0, firmware.CFW_RESERVED_BASE)
        with mock.patch.object(firmware, "validate_ring_battery_stock"), \
                mock.patch.object(firmware, "validate_message_transport_stock"), \
                mock.patch.object(firmware, "validate_compass_calibration_stock"), \
                self.assertRaisesRegex(AssertionError, "absolute references"):
            firmware.layout(bytes(damaged))

    def test_lld_reproduces_combined_blob_at_three_addresses(self):
        obj = self.directory / "firmware-closed.o"
        _, _, (_, _, old_size) = self.layout()
        actual = firmware.mram_addr(firmware.align_up(old_size, firmware.BLOB_ALIGN))
        for address in (actual, 0x7C0000, 0x20275000):
            lines = ["SECTIONS {", f".blob 0x{address:x} : {{", "FILL(0);"]
            for name, position, size in self.manifest["layout"]:
                lines += [f". = ADDR(.blob) + {position};", f"KEEP({obj}({name}))"]
            lines += ["}", "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment)",
                      "*(.note*) *(.ARM.attributes) *(.llvm_addrsig) }", "}"]
            script = self.directory / f"firmware-{address:x}.ld"
            script.write_text("\n".join(lines) + "\n", encoding="utf-8")
            elf = script.with_suffix(".elf")
            subprocess.run(["ld.lld", "--no-relax", "--entry=0", "-T", str(script),
                            str(obj), "-o", str(elf)], check=True)
            data, sections = builder.linker.parse_elf(elf)
            section = builder.linker.section(sections, ".blob")
            self.assertEqual(data[section["offset"]:section["offset"] + section["size"]], self.blob)


if __name__ == "__main__":
    unittest.main()
