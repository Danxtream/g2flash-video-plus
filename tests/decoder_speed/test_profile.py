"""Independent digest vectors, nested cycle accounting, and executable IR hooks."""
from pathlib import Path
import ctypes
import hashlib
import importlib.util
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'patches'))
from decoder_speed.profile_ir import instrument, function_names


class ProfileTests(unittest.TestCase):
    def test_sha256_padding_blocks_and_upload_lengths(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            source=folder/'hash.c'
            source.write_text(f'#include "{(ROOT/"patches/decoder_speed/sha256.h").as_posix()}"\n'
                'void digest(const unsigned char *p,unsigned n,unsigned char *out) { ds_sha256(p,n,out); }\n')
            library=folder/'hash.so'
            subprocess.run(['clang','-shared','-fPIC','-O2',str(source),'-o',str(library)],check=True)
            digest=ctypes.CDLL(str(library)).digest
            digest.argtypes=[ctypes.c_char_p,ctypes.c_uint32,ctypes.c_void_p]
            inputs=[b'',b'abc',b'a'*1000000]+[bytes((i*37)&255 for i in range(n))
                for n in (1,31,55,56,63,64,65,127,128,129,2048,66600,136888,156108)]
            for data in inputs:
                out=ctypes.create_string_buffer(32)
                digest(data,len(data),out)
                self.assertEqual(out.raw,hashlib.sha256(data).digest())

    def test_nested_cycles_wrap_and_corrupt_or_deep_profiles(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            source=folder/'profile.c'
            source.write_text(f'#include "{(ROOT/"patches/decoder_speed/profile.h").as_posix()}"\n'+'''
#include <assert.h>
#include <string.h>
int main(void) {
    ds_profile p={0}; p.count=2;
    ds_profile_enter_at(&p,0,100,1); ds_profile_enter_at(&p,1,120,1);
    ds_profile_exit_at(&p,1,150,1); ds_profile_exit_at(&p,0,180,2);
    assert(!p.flags && !p.depth && p.highwater==2);
    assert(p.rows[0].calls==1 && p.rows[0].inclusive==80 && p.rows[0].exclusive==50);
    assert(p.rows[1].inclusive==30 && p.rows[1].exclusive==30);
    ds_profile_enter_at(&p,1,0xfffffff0U,3); ds_profile_exit_at(&p,1,0x30,4);
    assert(!p.flags && p.rows[1].inclusive==94);
    ds_profile_enter_at(&p,0,0,5); ds_profile_exit_at(&p,0,1,8005); assert(p.flags&4);
    memset(&p,0,sizeof(p)); p.count=1;
    for (unsigned i=0;i<65;++i) ds_profile_enter_at(&p,0,i,0);
    assert(p.flags&1 && p.depth==64);
    memset(&p,0,sizeof(p)); p.count=1;
    ds_profile_exit_at(&p,0,0,0); assert(p.flags&2);
    memset(&p,0,sizeof(p)); p.count=1; p.rows[0].calls=0xffffffffU;
    ds_profile_enter_at(&p,0,0,0); ds_profile_exit_at(&p,0,1,0); assert(p.flags&8);
    return 0;
}
''')
            executable=folder/'profile'
            subprocess.run(['clang','-O1','-g','-fsanitize=address,undefined',str(source),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)

    def test_hooks_execute_at_entry_and_every_return(self):
        ir='''define i32 @twice(i32 %value) {
entry:
  %answer = add i32 %value, %value
  ret i32 %answer
}
'''
        self.assertEqual(function_names(ir),['twice'])
        hooked=instrument(ir,{'twice':7})
        self.assertNotIn('call void @ds_profile_',instrument(ir,{}))
        self.assertNotIn('readnone',instrument(ir+'attributes #0 = { readnone nosync }\n',{'twice':7}))
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            (folder/'hooked.ll').write_text(hooked)
            (folder/'main.c').write_text('''
static unsigned entered,exited;
void ds_profile_enter(unsigned id) { entered+=id; }
void ds_profile_exit(unsigned id) { exited+=id; }
extern int twice(int);
int main(void) { return twice(21)!=42 || entered!=7 || exited!=7; }
''')
            subprocess.run(['clang','-O2',str(folder/'hooked.ll'),str(folder/'main.c'),'-o',str(folder/'check')],check=True)
            subprocess.run([str(folder/'check')],check=True)
        for token in ('musttail ', 'invoke ', 'resume ', 'catchswitch ', 'cleanupret '):
            with self.assertRaises(ValueError): instrument(ir.replace('  ret ', '  '+token+'ret '),{'twice':7})


if __name__=='__main__': unittest.main()
