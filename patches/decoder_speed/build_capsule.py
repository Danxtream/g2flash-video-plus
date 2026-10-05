"""Build firmware A's single O2 movable decoder, without changing jim's C flags."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def build_capsule():
    import build
    obj = ROOT / 'obj'
    obj.mkdir(exist_ok=True)
    decoder = ROOT / 'third-party/sub0h264/components/sub0h264'
    includes = os.environ.get('SUB0H264_ARM_CXX_INCLUDES',
        '/usr/include/newlib/c++/14.2.1:'
        '/usr/include/newlib/c++/14.2.1/arm-none-eabi/thumb/v7e-m+fp/softfp:'
        '/usr/include/newlib').split(os.pathsep)
    flags = ['--target=thumbv7em-none-eabi', '-mthumb', '-mfpu=fpv4-sp-d16',
        '-mfloat-abi=softfp', '-ffp-contract=off', '-fno-jump-tables',
        '-fomit-frame-pointer', '-fno-builtin', '-mno-unaligned-access',
        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Wall', '-Wextra',
        '-O2', '-std=c++23', '-DNDEBUG', '-fPIC', '-fvisibility=hidden',
        '-fvisibility-inlines-hidden', '-fno-exceptions', '-fno-rtti',
        '-fno-threadsafe-statics', '-fstack-usage']
    for directory in includes:
        if not (Path(directory).is_dir()):
            raise build.BuildError(f'ARM C++ include directory missing: {directory}')
        flags += ['-isystem', directory]
    flags += ['-I'+str(decoder/'include'), '-I'+str(decoder/'src')]
    subprocess.run(['clang++', *flags, '-c', str(HERE/'capsule.cpp'), '-o', str(obj/'ds_decoder.o')], check=True)
    subprocess.run(['clang', *build.CFLAGS, '-c', str(HERE/'runtime.c'), '-o', str(obj/'ds_runtime.o')], check=True)
    subprocess.run(['ld.lld', '-r', str(obj/'ds_decoder.o'), str(obj/'ds_runtime.o'),
                    '-o', str(obj/'ds_closed.o')], check=True)
    blob, funcs, layout = build.link_pic_object(obj/'ds_closed.o')
    if len(blob) > 146000:
        raise build.BuildError(f'RAM capsule exceeds reserved executable budget: {len(blob)}')
    exports = {name: off for name, off, _ in funcs if name.startswith('ds_')}
    header = ['/* Generated movable capsule; never edit or commit this file. */', '#pragma once',
              f'#define DS_CAPSULE_BYTES {len(blob)}U']
    for name in ('ds_size', 'ds_selftest', 'ds_init', 'ds_destroy', 'ds_decode', 'ds_frame'):
        header.append(f'#define DS_OFFSET_{name.upper()} {exports[name]}U')
    header.append('static const unsigned char ds_capsule_image[] __attribute__((aligned(32))) = {')
    header += [','.join(f'0x{v:02x}' for v in blob[i:i+16])+',' for i in range(0,len(blob),16)]
    header += ['};', '']
    (obj/'decoder_speed_capsule.h').write_text('\n'.join(header), encoding='utf-8')
    (obj/'ds_capsule.bin').write_bytes(blob)
    (obj/'ds_capsule.json').write_text(json.dumps(dict(bytes=len(blob), flags=flags,
        sha256=hashlib.sha256(blob).hexdigest(), exports=exports, layout=layout), indent=2)+'\n')
    print(f'Decoder speed capsule: {len(blob)} bytes (-O2, closed PIC)', file=sys.stderr)
