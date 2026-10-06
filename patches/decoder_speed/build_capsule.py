"""Build firmware B's small/mixed movable decoders; jim's C flags stay intact."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXPORTS = ('ds_size', 'ds_selftest', 'ds_init', 'ds_destroy', 'ds_decode', 'ds_frame')


def cxx_flags(optimization):
    import build
    decoder = ROOT / 'third-party/sub0h264/components/sub0h264'
    includes = os.environ.get('SUB0H264_ARM_CXX_INCLUDES',
        '/usr/include/newlib/c++/14.2.1:'
        '/usr/include/newlib/c++/14.2.1/arm-none-eabi/thumb/v7e-m+fp/softfp:'
        '/usr/include/newlib').split(os.pathsep)
    flags = ['--target=thumbv7em-none-eabi', '-mthumb', '-mfpu=fpv4-sp-d16',
        '-mfloat-abi=softfp', '-ffp-contract=off', '-fno-jump-tables',
        '-fomit-frame-pointer', '-fno-builtin', '-mno-unaligned-access',
        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-Wall', '-Wextra',
        optimization, '-std=c++23', '-DNDEBUG', '-fPIC', '-fvisibility=hidden',
        '-fvisibility-inlines-hidden', '-fno-exceptions', '-fno-rtti',
        '-fno-threadsafe-statics', '-fstack-usage']
    for directory in includes:
        if not (Path(directory).is_dir()):
            raise build.BuildError(f'ARM C++ include directory missing: {directory}')
        flags += ['-isystem', directory]
    flags += ['-I'+str(decoder/'include'), '-I'+str(decoder/'src')]
    return flags


def compile_mixed(directory, flags, source=HERE/'capsule.cpp'):
    """Compile each function once, with O2 hot loops and Oz cold code, no LTO."""
    from decoder_speed.partition_ir import partition
    ir_flags = [f for f in flags if f != '-fstack-usage']
    ir_path = directory/'ds_preopt.ll'
    subprocess.run(['clang++', *ir_flags, '-Xclang', '-disable-llvm-passes', '-S',
                    '-emit-llvm', str(source), '-o', str(ir_path)], check=True)
    hot, cold, manifest = partition(ir_path.read_text(encoding='utf-8'))
    objects = []
    for name, text, opt in (('hot', hot, '-O2'), ('cold', cold, '-Oz')):
        path = directory/f'ds_{name}.ll'
        path.write_text(text, encoding='utf-8')
        output = directory/f'ds_{name}.o'
        compile_flags = [opt if f == '-O2' else f for f in flags]
        subprocess.run(['clang++', *compile_flags, '-ffunction-sections', '-fdata-sections',
                        '-c', str(path), '-o', str(output)], check=True)
        objects.append(output)
    (directory/'ds_membership.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return objects


def build_capsule():
    import build
    obj = ROOT / 'obj'
    obj.mkdir(exist_ok=True)
    subprocess.run(['clang', *build.CFLAGS, '-c', str(HERE/'runtime.c'), '-o', str(obj/'ds_runtime.o')], check=True)
    small_flags = cxx_flags('-Oz')
    subprocess.run(['clang++', *small_flags, '-c', str(HERE/'capsule.cpp'), '-o', str(obj/'ds_small.o')], check=True)
    hot_flags = cxx_flags('-O2')
    mixed_objects = compile_mixed(obj, hot_flags)
    header = ['/* Generated movable capsules; never edit or commit this file. */', '#pragma once']
    roots = [arg for name in EXPORTS for arg in ('-u', name)]
    for name, objects, flags in (('small', [obj/'ds_small.o'], small_flags), ('mixed', mixed_objects, hot_flags)):
        closed = obj/f'ds_{name}_closed.o'
        gc = ['--gc-sections', *roots] if name == 'mixed' else []
        subprocess.run(['ld.lld', '-r', *gc, *(str(o) for o in objects), str(obj/'ds_runtime.o'),
                        '-o', str(closed)], check=True)
        blob, funcs, layout = build.link_pic_object(closed)
        # Real glasses: 133,924 free minus 16 KiB reserve and 128 B copy slack.
        # Live allocator/free/max checks are still required before every run.
        if len(blob) > 117412:
            raise build.BuildError(f'{name} capsule exceeds measured heap-27 budget: {len(blob)}')
        exports = {symbol: off for symbol, off, _ in funcs if symbol in EXPORTS}
        if set(exports) != set(EXPORTS):
            raise build.BuildError(f'{name}: missing capsule export')
        header += [f'#define DS_{name.upper()}_BYTES {len(blob)}U',
                   f'static const unsigned int ds_{name}_offsets[] = {{'+','.join(str(exports[s])+'U' for s in EXPORTS)+'};',
                   f'static const unsigned char ds_{name}_image[] __attribute__((aligned(32))) = {{']
        header += [','.join(f'0x{v:02x}' for v in blob[i:i+16])+',' for i in range(0,len(blob),16)]
        header += ['};', '']
        (obj/f'ds_{name}_capsule.bin').write_bytes(blob)
        (obj/f'ds_{name}_capsule.json').write_text(json.dumps(dict(bytes=len(blob), flags=flags,
            sha256=hashlib.sha256(blob).hexdigest(), exports=exports, layout=layout,
            functions=funcs), indent=2)+'\n', encoding='utf-8')
        print(f'Decoder speed {name} capsule: {len(blob)} bytes (closed PIC)', file=sys.stderr)
    (obj/'decoder_speed_capsule.h').write_text('\n'.join(header), encoding='utf-8')
