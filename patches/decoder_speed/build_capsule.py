"""Build checked uploadable decoder capsules independently of firmware C."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT/'patches'))
CAPSULE_LIMIT = 156108
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


def compile_mixed(directory, flags, source=HERE/'capsule.cpp', promotions=(), profile=False):
    """Single definition ownership with optional post-optimization cycle hooks."""
    from decoder_speed.partition_ir import partition
    from decoder_speed.profile_ir import function_names, instrument
    ir_flags = [f for f in flags if f != '-fstack-usage']
    ir_path = directory/'preopt.ll'
    subprocess.run(['clang++', *ir_flags, '-Xclang', '-disable-llvm-passes', '-S',
                    '-emit-llvm', str(source), '-o', str(ir_path)], check=True)
    hot, cold, manifest = partition(ir_path.read_text(encoding='utf-8'), promotions=promotions)
    optimized = []
    for name, text, opt in (('hot', hot, '-O2'), ('cold', cold, '-Oz')):
        path = directory/f'{name}.ll'
        path.write_text(text, encoding='utf-8')
        compile_flags = [opt if f == '-O2' else f for f in ir_flags]
        if profile:
            output = directory/f'{name}-optimized.ll'
            subprocess.run(['clang++', *compile_flags, '-S', '-emit-llvm', str(path),
                            '-o', str(output)], check=True)
            optimized.append((name, output.read_text(encoding='utf-8'), compile_flags))
        else:
            optimized.append((name, text, compile_flags))
    identifiers = {}
    if profile:
        # The controller probes these exports before installing callbacks.
        # Neither belongs to decode timing, so they must remain self-contained.
        names = sorted({n for _, text, _ in optimized for n in function_names(text)}
                       - {'ds_size','ds_selftest'})
        if len(names) > 512:
            raise ValueError('profile exceeds counter capacity; instrument a subset')
        identifiers = {name:i for i,name in enumerate(names)}
    objects = []
    for name, text, compile_flags in optimized:
        path = directory/f'{name}-final.ll'
        path.write_text(instrument(text, identifiers) if profile else text, encoding='utf-8')
        output = directory/f'{name}.o'
        no_passes = ['-Xclang', '-disable-llvm-passes'] if profile else []
        subprocess.run(['clang++', *compile_flags, *no_passes, '-ffunction-sections', '-fdata-sections', '-fstack-usage',
                        '-c', str(path), '-o', str(output)], check=True)
        objects.append(output)
    manifest['profile_functions'] = identifiers
    (directory/'membership.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return objects, manifest


def build_capsule(output, kind='mixed', promotions=()):
    """Close/link a capsule and emit bytes plus the upload manifest."""
    import build
    directory = Path(output).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    runtime = directory/'runtime.o'
    subprocess.run(['clang', *build.CFLAGS, '-c', str(HERE/'runtime.c'), '-o', str(runtime)], check=True)
    flags = cxx_flags('-Oz' if kind == 'small' else '-O2')
    profiled = kind in ('profile', 'profile-control')
    membership = {}
    if kind in ('full', 'small'):
        obj = directory/'decoder.o'
        subprocess.run(['clang++', *flags, '-c', str(HERE/'capsule.cpp'), '-o', str(obj)], check=True)
        objects = [obj]
    else:
        objects, membership = compile_mixed(directory, flags, promotions=promotions, profile=profiled)
    if profiled:
        hooks = directory/'hooks.o'
        subprocess.run(['clang', *build.CFLAGS, '-c', str(HERE/'profile_hooks.c'), '-o', str(hooks)], check=True)
        objects.append(hooks)
    closed = directory/'closed.o'
    roots = [arg for name in EXPORTS for arg in ('-u', name)]
    gc = ['--gc-sections', *roots] if kind not in ('full', 'small') else []
    subprocess.run(['ld.lld', '-r', *gc, *(str(o) for o in objects), str(runtime), '-o', str(closed)], check=True)
    blob, funcs, layout = build.link_pic_object(closed)
    if len(blob) > CAPSULE_LIMIT:
        raise build.BuildError(f'capsule exceeds fresh heap-27 budget: {len(blob)}')
    exports = {symbol:off for symbol,off,_ in funcs if symbol in EXPORTS}
    if set(exports) != set(EXPORTS):
        raise build.BuildError('missing capsule export')
    text = [(off,off+n) for name,off,n in layout if name.startswith('.text') and n]
    for off in exports.values():
        if off&1 or not any(lo<=off<hi for lo,hi in text):
            raise build.BuildError('export outside executable sections')
    name = 'mixedplus-'+format(len(blob)/1000, '.3f') if promotions else kind
    manifest = dict(abi=1, name=name, kind=kind, decoder='59c66b1', bytes=len(blob),
        flags=flags, sha256=hashlib.sha256(blob).hexdigest(), exports=exports,
        text_bounds=[min(x[0] for x in text),max(x[1] for x in text)], layout=layout,
        functions=funcs, promotions=list(promotions), profile_functions=membership.get('profile_functions', {}),
        compiler=subprocess.check_output(['clang++','--version'], text=True).splitlines()[0])
    (directory/'capsule.bin').write_bytes(blob)
    (directory/'capsule.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(f'{name}: {len(blob)} B, closed PIC, SHA256 {manifest["sha256"]}', flush=True)
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--kind', choices=('full','small','mixed','profile','profile-control'), default='mixed')
    parser.add_argument('--promotions', help='JSON array of exact pre-optimization symbols')
    args=parser.parse_args()
    promotions=json.loads(Path(args.promotions).read_text(encoding='utf-8')) if args.promotions else []
    if not isinstance(promotions,list) or not all(isinstance(n,str) for n in promotions):
        parser.error('promotions must be a JSON array of symbol names')
    build_capsule(args.output,args.kind,promotions)


if __name__=='__main__':
    main()
