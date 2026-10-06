"""Check definition ownership and execute cross-module calls after partitioning."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('partition', ROOT/'patches/decoder_speed/partition_ir.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)

IR = '''@table = internal constant [2 x i32] [i32 7, i32 9], align 4
define internal i32 @sub0h264_hot(i32 %x) #0 {
  %p = getelementptr [2 x i32], ptr @table, i32 0, i32 1
  %v = load i32, ptr %p
  %n = call i32 @cold(i32 %x)
  %sum = add i32 %v, %n
  ret i32 %sum
}
define internal i32 @cold(i32 %x) #0 {
  %sum = add i32 %x, 7
  ret i32 %sum
}
define i32 @entry(i32 %x) #0 {
  %v = call i32 @sub0h264_hot(i32 %x)
  ret i32 %v
}
attributes #0 = { nounwind }
'''


class PartitionTests(unittest.TestCase):
    def test_exact_promotion_has_one_owner_and_unknown_symbol_is_rejected(self):
        hot,cold,membership=module.partition(IR,('_hot',),promotions=('cold',))
        self.assertEqual(membership['functions']['cold'],'hot')
        self.assertEqual(membership['promotions'],['cold'])
        self.assertEqual(sum(line.startswith('define ') and module.symbol(line)=='cold'
                             for line in (hot+cold).splitlines()),1)
        self.assertNotIn('minsize',hot)
        with self.assertRaises(ValueError):module.partition(IR,('_hot',),promotions=('absent',))

    def test_single_owner_hidden_cross_calls_and_constants(self):
        hot, cold, membership = module.partition(IR, ('_hot',))
        self.assertEqual(membership['functions'], {'sub0h264_hot':'hot','cold':'cold','entry':'cold'})
        self.assertIn('@table = external dso_local hidden constant [2 x i32]', hot)
        self.assertIn('@table = dso_local hidden', cold)
        for name in membership['functions']:
            self.assertEqual(sum(line.startswith('define ') and module.symbol(line)==name
                                 for line in (hot+cold).splitlines()), 1)
        self.assertIn('minsize optsize', cold)
        self.assertNotIn('minsize', hot)
        with tempfile.TemporaryDirectory() as temporary:
            here=Path(temporary)
            for name, text, opt in (('hot',hot,'-O2'),('cold',cold,'-Oz')):
                (here/f'{name}.ll').write_text(text,encoding='utf-8')
                subprocess.run(['clang',opt,'-c',str(here/f'{name}.ll'),'-o',str(here/f'{name}.o')],check=True,capture_output=True)
            (here/'main.c').write_text('extern int entry(int); int main(void) { return entry(26)!=42; }\n',encoding='utf-8')
            subprocess.run(['clang',str(here/'main.c'),str(here/'hot.o'),str(here/'cold.o'),'-o',str(here/'check')],check=True)
            subprocess.run([str(here/'check')],check=True)

    def test_nested_named_and_scalar_constant_types(self):
        for text, expected in (('[2 x {i32, [4 x i8]}] zeroinitializer','[2 x {i32, [4 x i8]}]'),
                               ('%"struct.example" zeroinitializer','%"struct.example"'),('i32 17','i32')):
            self.assertEqual(module.constant_type(text),expected)

    def test_unsupported_ir_stops_before_compilation(self):
        for change in ('@counter = internal global i32 0\n', '@llvm.global_ctors = appending global [0 x ptr] []\n',
                       '@other = alias i32, ptr @cold\n', '@local = thread_local global i32 0\n'):
            with self.assertRaises(ValueError): module.partition(change+IR,('_hot',))
        with self.assertRaises(ValueError): module.partition(IR,('missing',))
        with self.assertRaises(ValueError): module.partition(IR.replace(' #0 {',' #0 !dbg !1 {'),('_hot',))
        with self.assertRaises(ValueError): module.partition(IR+IR,('_hot',))

    def test_unreferenced_inline_linkage_is_preserved(self):
        extra='define linkonce_odr hidden i32 @unused() #0 {\n  ret i32 1\n}\n'
        _, cold, _=module.partition(IR+extra,('_hot',))
        self.assertIn('define linkonce_odr hidden i32 @unused',cold)


if __name__=='__main__': unittest.main()
