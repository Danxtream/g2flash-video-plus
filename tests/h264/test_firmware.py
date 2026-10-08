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
            (output / "decoder.bin").write_bytes(blob)
            path = output / "decoder.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with mock.patch.object(firmware, "SCRIPT_DIR", str(patches)), \
                    mock.patch.object(firmware.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "", "")
                self.assertEqual(firmware.build_decoder_blob(), (blob, manifest))
                self.assertEqual(run.call_args.args[0],
                                 [sys.executable, str(patches / "h264/build_decoder.py"),
                                  "--output", str(output)])
                for key, value in (("bytes", 1), ("sha256", "0" * 64),
                                   ("alignment", 0), ("alignment", 3), ("alignment", 8)):
                    path.write_text(json.dumps({**manifest, key: value}), encoding="utf-8")
                    with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                        firmware.build_decoder_blob()
                run.return_value = subprocess.CompletedProcess([], 1, "", "compiler failed")
                with self.assertRaisesRegex(SystemExit, "compiler failed"):
                    firmware.build_decoder_blob()


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
        cls.manifest = builder.build_decoder(cls.directory)
        cls.decoder = (cls.directory / "decoder.bin").read_bytes()
        cls.c_build = builder.linker.build_dict(str(ROOT / "patches/patches_main.c"))
        cls.c_blob = bytes.fromhex(cls.c_build["text"])

    def layout(self, decoder=None, ceiling=None):
        if decoder is None:
            decoder = self.decoder
        if ceiling is None:
            ceiling = firmware.APP_MAX_END
        with mock.patch.object(firmware, "build_blob", return_value=self.c_build), \
                mock.patch.object(firmware, "build_decoder_blob", return_value=(decoder, self.manifest)), \
                mock.patch.object(firmware, "APP_MAX_END", ceiling), \
                contextlib.redirect_stdout(io.StringIO()):
            return firmware.layout(self.base)

    def test_c_prefix_decoder_bytes_and_patch_sites_are_unchanged(self):
        append, patches, (_, _, old_size) = self.layout()
        c_only, old_patches, _ = self.layout(b"")
        self.assertEqual(patches, old_patches)
        self.assertEqual(len(patches), 30)
        self.assertEqual(append[:len(c_only)], c_only)
        self.assertEqual(c_only[:len(self.c_blob)], self.c_blob)
        start = firmware.align_up(old_size + len(c_only), firmware.BLOB_ALIGN) - old_size
        self.assertEqual(append[start:], self.decoder)
        self.assertEqual(hashlib.sha256(append[start:]).hexdigest(), self.manifest["sha256"])
        self.assertEqual(firmware.mram_addr(old_size + start) % self.manifest["alignment"], 0)
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

    def test_lld_reproduces_both_blobs_at_three_addresses(self):
        c_object = ROOT / "obj/patches_main.o"
        _, _, c_layout = builder.linker.link_pic_object(c_object)
        prefixed = self.directory / "decoder-prefixed.o"
        # Independent LLD needs unique symbols for the two closed helper sets.
        # Renaming only this validation object changes no emitted instructions.
        subprocess.run(["arm-none-eabi-objcopy", "--prefix-symbols=h264_",
                        str(self.directory / "closed.o"), str(prefixed)], check=True)
        offset = firmware.align_up(len(self.c_blob), firmware.BLOB_ALIGN)
        expected = self.c_blob + b"\0" * (offset - len(self.c_blob)) + self.decoder
        _, _, (_, _, old_size) = self.layout()
        actual = firmware.mram_addr(firmware.align_up(old_size, firmware.BLOB_ALIGN))
        for address in (actual, 0x7C0000, 0x20275000):
            lines = ["SECTIONS {", f".blob 0x{address:x} : {{", "FILL(0);"]
            for obj, start, layout in ((c_object, 0, c_layout),
                                       (prefixed, offset, self.manifest["layout"])):
                for name, position, size in layout:
                    lines += [f". = ADDR(.blob) + {start + position};",
                              f"KEEP({obj}({name}))"]
            lines += ["}", "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment)",
                      "*(.note*) *(.ARM.attributes) *(.llvm_addrsig) }", "}"]
            script = self.directory / f"firmware-{address:x}.ld"
            script.write_text("\n".join(lines) + "\n", encoding="utf-8")
            elf = script.with_suffix(".elf")
            subprocess.run(["ld.lld", "--no-relax", "--entry=0", "-T", str(script),
                            str(c_object), str(prefixed), "-o", str(elf)], check=True)
            data, sections = builder.linker.parse_elf(elf)
            section = builder.linker.section(sections, ".blob")
            self.assertEqual(data[section["offset"]:section["offset"] + section["size"]], expected)


if __name__ == "__main__":
    unittest.main()
