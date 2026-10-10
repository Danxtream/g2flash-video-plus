"""Build offline luma references using the firmware's configured C decoder.

No device transport is created. SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]


def read_nals(path):
    """Preserve every Annex-B NAL in order, including non-picture inputs."""
    data = Path(path).read_bytes()
    if not data or len(data) >= 30000000:
        raise ValueError('clip must be nonempty and below 30 MB')
    starts = list(re.finditer(rb'\x00\x00(?:\x00)?\x01', data))
    if not starts or any(data[:starts[0].start()]):
        raise ValueError('expected Annex-B start codes')
    nals = [data[m.end():starts[i + 1].start() if i + 1 < len(starts) else len(data)]
            for i, m in enumerate(starts)]
    if any(not nal or len(nal) > 4086 or nal[0] & 0x80 for nal in nals):
        raise ValueError('empty, forbidden or oversized NAL')
    return data, nals


def build_reference(directory):
    """Compile the current C interface and runtime with fatal sanitizers."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    common = ['-O2', '-g', '-fno-builtin', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    runtime = directory / 'runtime.o'
    subprocess.run(['clang', *common, '-c', str(ROOT / 'patches/h264/runtime.c'), '-o', str(runtime)], check=True)
    exe = directory / 'video-reference'
    vendor = ROOT / 'third-party/sub0h264/components/sub0h264'
    subprocess.run(['clang++', *common, '-std=c++23', '-DNDEBUG', '-fno-exceptions', '-fno-rtti',
                    '-Wall', '-Wextra', '-Werror', '-I' + str(vendor / 'src'), '-I' + str(vendor / 'include'),
                    str(ROOT / 'patches/h264/g2_h264.cpp'), str(ROOT / 'tests/h264/video_reference.cpp'),
                    str(runtime), '-o', str(exe)], check=True)
    return exe


def decoder_identity():
    """Pin decoder and configuration sources, excluding unrelated firmware."""
    files = sorted((ROOT / 'third-party/sub0h264/components/sub0h264').rglob('*.hpp'))
    files += sorted((ROOT / 'third-party/sub0h264/components/sub0h264').rglob('*.h'))
    files += [ROOT / 'patches/h264' / name for name in
              ('config.hpp', 'g2_h264.cpp', 'g2_h264.h', 'runtime.c', 'runtime.h', 'timing.hpp')]
    rows = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    return hashlib.sha256(json.dumps(rows, sort_keys=True).encode()).hexdigest(), rows


def make_reference(path, exe, output=None, previews=None):
    """Return actual completed pictures, active-plane CRC and contributing NALs."""
    data, nals = read_nals(path)
    wire = b''.join(struct.pack('<I', len(nal)) + nal for nal in nals)
    with tempfile.TemporaryFile() as raw:
        result = subprocess.run([str(exe)], input=wire, stdout=raw, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError(f'decoder reference failed ({result.returncode}): ' + result.stderr.decode(errors='replace'))
        raw.seek(0)
        if raw.read(4) != b'G2Y1':
            raise ValueError('invalid decoder reference header')
        frames = []
        while True:
            tag = raw.read(1)
            if tag == b'E':
                break
            if tag != b'F':
                raise ValueError('invalid decoder reference tag')
            row = raw.read(24)
            if len(row) != 24:
                raise ValueError('truncated decoder frame')
            ordinal, first, last, width, height, nal_type = struct.unpack('<6I', row)
            pixels = raw.read(width * height)
            if (ordinal != len(frames) + 1 or (width, height) != (320, 192) or
                    len(pixels) != width * height or not 0 <= first <= last < len(nals) or
                    nal_type not in (1, 5) or (frames and first != frames[-1]['last_nal'] + 1)):
                raise ValueError('inconsistent decoder frame output')
            frames.append(dict(ordinal=ordinal, first_nal=first, last_nal=last, width=width, height=height,
                               crc=zlib.crc32(pixels), kind='IDR' if nal_type == 5 else 'P'))
            if previews is not None and ordinal == 1:
                Path(previews).write_bytes(f'P5\n{width} {height}\n255\n'.encode() + pixels)
        tail = raw.read(8)
        if len(tail) != 8 or struct.unpack('<II', tail) != (len(nals), len(frames)) or raw.read(1) or not frames:
            raise ValueError('invalid decoder reference footer/count')
    profiles = sorted({nal[1] for nal in nals if nal[0] & 31 == 7 and len(nal) > 1})
    identity, sources = decoder_identity()
    reference = dict(schema=1, source_sha256=hashlib.sha256(data).hexdigest(), source_bytes=len(data),
                     decoder_identity=identity, decoder_sources=sources, profiles=profiles,
                     nal_sizes=list(map(len, nals)), nal_count=len(nals), frame_count=len(frames), frames=frames,
                     format='progressive 320x192 I/P; ref1; two DPB; single slice; no crop/FMO/weighted prediction/I_PCM',
                     skip_chroma=True)
    if output is not None:
        Path(output).write_text(json.dumps(reference, indent=2) + '\n', encoding='utf-8')
    return reference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        result = make_reference(args.clip, build_reference(directory), args.output)
    print(f"{result['nal_count']} NALs, {result['frame_count']} pictures; all active Y references saved")


if __name__ == '__main__':
    main()
