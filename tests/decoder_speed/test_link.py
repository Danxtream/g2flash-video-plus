"""Independent ARM LLD checks for the movable capsule and surrounding C blob."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build', ROOT/'patches/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def lld_bytes(obj, address, directory):
    blob, _, layout = build.link_pic_object(obj)
    script = directory / f'{address:x}.ld'
    lines = ['SECTIONS {', f'.blob 0x{address:x} : {{', 'FILL(0);']
    for name, off, _ in layout:
        lines += [f'. = ADDR(.blob) + {off};', f'KEEP(*({name}))']
    lines += ['}', '/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.ARM.attributes) *(.llvm_addrsig) }', '}']
    script.write_text('\n'.join(lines)+'\n')
    elf = directory / f'{address:x}.elf'
    linked=subprocess.run(['ld.lld','--no-relax','-T',str(script),str(obj),'-o',str(elf)],capture_output=True,text=True)
    if linked.returncode: raise AssertionError(linked.stderr)
    raw, secs = build.parse_elf(elf)
    section = build.section(secs,'.blob')
    return blob, raw[section['offset']:section['offset']+section['size']]


class LinkTests(unittest.TestCase):
    def compile(self, text):
        src = self.here/'fixture.c'
        src.write_text(text)
        obj = self.here/'fixture.o'
        subprocess.run(['clang',*build.CFLAGS,'-c',str(src),'-o',str(obj)],check=True,capture_output=True)
        return obj

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.here = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_capsule_identical_at_three_load_addresses(self):
        for address in (0x7cb000,0x20275000,0x20278000):
            actual, reference = lld_bytes(ROOT/'obj/ds_closed.o',address,self.here)
            self.assertEqual(actual,reference)

    def test_c_blob_multiple_sections_identical_to_lld(self):
        for address in (0x7bea80,0x438000):
            actual, reference = lld_bytes(ROOT/'obj/patches_main.o',address,self.here)
            self.assertEqual(actual,reference)

    def test_backward_and_forward_branches_across_sections(self):
        obj=self.compile('''
__attribute__((noinline,section(".text.first"))) int first(int n) { return n+1; }
__attribute__((noinline,section(".text.middle"))) int middle(int n) { return first(n)+1; }
__attribute__((noinline,section(".text.last"))) int last(int n) { return middle(n)+first(n); }
''')
        for address in (0x7bea64,0x20275000):
            actual,reference=lld_bytes(obj,address,self.here)
            self.assertEqual(actual,reference)

    def test_rodata_and_thumb_function_addresses(self):
        obj=self.compile('''
__attribute__((noinline,section(".text.other"))) int other(int n) { return n+1; }
const char text[]="read-only alignment";
int (*entry(void))(int) { return other; }
const char *data(void) { return text; }
''')
        actual,reference=lld_bytes(obj,0x20275000,self.here)
        self.assertEqual(actual,reference)

    def test_writable_globals_rejected(self):
        obj=self.compile('int counter; int next(void) { return ++counter; }')
        with self.assertRaises(build.BuildError): build.link_pic_object(obj)

    def test_undefined_function_rejected(self):
        obj=self.compile('extern int outside(int); int entry(int n) { return outside(n); }')
        with self.assertRaises(build.BuildError): build.link_pic_object(obj)

    def test_absolute_pointer_rejected(self):
        obj=self.compile('const char text[]="data"; const char *const ptr=text; const char **entry(void) { return (const char **)&ptr; }')
        with self.assertRaises(build.BuildError): build.link_pic_object(obj)


if __name__=='__main__': unittest.main()
