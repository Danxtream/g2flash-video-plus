#!/usr/bin/env python3
"""Measure the local Faceclaw/16 decoder experiment through one lens.

Prepare the clip/reference on the PC first. Run connects and uploads only;
this tool never flashes firmware. Firmware A accepts the verified 32-frame
Tokyo segment, and supports full-mram/full-ram plus an MRAM-only uncached run.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import secrets
import statistics
import struct
import subprocess
import sys
import time
import zlib

from g2flash import Bridge, CTRL, DroidBridgeTransport, LocalBleTransport, authenticate, crc16, parse_connection_string
from send_message_probe import LENS_BITS, make_packets, parse_acks

MAGIC = 0x44530103
CLIP_BYTES = 23921
CLIP_SHA256 = 'c80a8087c83bed781374b3475987d9e772b517177ba244b250704046b354da0b'
CLIP_CRC = 0xc81c1bdc
PROFILES = (
    dict(bytes=CLIP_BYTES,sha256=CLIP_SHA256,crc32=CLIP_CRC,dimensions=[320,192]),
)
HELLO, BEGIN, WRITE, SEAL, RUN, READ, ABORT, CLOSE, HEAPS = range(9)
SETUPS = {'full-mram': 0, 'full-ram': 1}
HEADER_WORDS, FRAMES, PASSES, WARMUPS = 128, 32, 5, 2


def message(op, session, args=b''):
    """Encode a versioned, bounded experiment command inside the old transport."""
    if not 0 <= op <= HEAPS or not 0 <= session <= 65535:
        raise ValueError('invalid operation/session')
    return b'\x1fDS\x01' + bytes([op]) + struct.pack('<H', session) + args


def parse_result(frame):
    """Validate a minimum-MTU result notification; return lens/token/index/value."""
    if (len(frame) != 20 or frame[:2] != b'\xaa\x12' or frame[3] != 12
            or frame[4:8] != bytes((1, 1, 0xf0, 0))):
        return None
    body = frame[8:-2]
    if body[0] != 5 or body[1] not in (1, 2) or crc16(body) != frame[-2:]:
        return None
    return body[1], *struct.unpack('<HHI', body[2:])


def check_clip(raw):
    for profile in PROFILES:
        if (len(raw)==profile['bytes'] and hashlib.sha256(raw).hexdigest()==profile['sha256']
                and zlib.crc32(raw)==profile['crc32']):
            return profile
    raise ValueError('firmware A requires the verified 23,921-byte Tokyo segment')


def linux_path(path):
    """Translate a Windows argument for WSL without building a shell command."""
    p = Path(path).resolve()
    if os.name != 'nt':
        return str(p)
    return subprocess.check_output(['wsl', '-d', 'Ubuntu', '--exec', 'wslpath', '-u', p.as_posix()], text=True).strip()


def prepare(source, clip_path, reference_path):
    """Freeze the approved segment and calculate both modes with the real capsule."""
    raw = Path(source).read_bytes()[:CLIP_BYTES]
    profile = check_clip(raw)
    clip_path = Path(clip_path).resolve()
    reference_path = Path(reference_path).resolve()
    clip_path.parent.mkdir(parents=True, exist_ok=True)
    reference_path.parent.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parent
    executable = reference_path.parent / 'decoder-reference'
    decoder = root / 'third-party/sub0h264/components/sub0h264'
    command = ['clang++', '-std=c++23', '-O2', '-DNDEBUG', '-DDS_HOST_TEST',
        '-Wno-tautological-constant-out-of-range-compare',
        '-I'+linux_path(decoder/'include'), '-I'+linux_path(decoder/'src'),
        linux_path(root/'patches/decoder_speed/capsule.cpp'),
        linux_path(root/'tests/decoder_speed/reference.cpp'), '-o', linux_path(executable)]
    prefix = ['wsl', '-d', 'Ubuntu', '--exec'] if os.name == 'nt' else []
    subprocess.run(prefix+command, check=True)
    clip_path.write_bytes(raw)
    hashes = {}
    for skip in (0, 1):
        p = subprocess.run(prefix+[linux_path(executable), linux_path(clip_path), str(skip)],
                           capture_output=True, text=True, check=True)
        (reference_path.parent/f'reference-skip-{skip}.log').write_text(p.stderr+p.stdout, encoding='utf-8')
        rows = [json.loads(line) for line in p.stdout.splitlines()]
        if len(rows) != 7*FRAMES:
            raise ValueError('wrong PC frame count')
        first = [row['hash'] for row in rows[:FRAMES]]
        if any(row['pass'] != i//FRAMES or row['frame'] != i%FRAMES or row['hash'] != first[i%FRAMES]
               for i, row in enumerate(rows)):
            raise ValueError('PC repeats disagree')
        hashes[str(skip)] = first
    if hashes['0'] != hashes['1']:
        raise ValueError('PC chroma modes disagree on Y')
    reference = dict(protocol=1, clip_sha256=profile['sha256'], clip_crc32=profile['crc32'],
        dimensions=profile['dimensions'], frames=FRAMES, decoder='59c66b1', hashes=hashes)
    reference_path.write_text(json.dumps(reference, indent=2)+'\n', encoding='utf-8')
    print(f'PC reference PASS: 448 frames, both modes and seven repeats; {reference_path}')


class Client:
    """One direct lens, old SID-f0 framing, fresh command stream per transaction."""
    def __init__(self, transport, lens, mtu=23, timeout=20):
        self.transport, self.lens, self.mtu, self.timeout = transport, LENS_BITS[lens], mtu, timeout
        self.next_sequence = secrets.randbelow(256)
        self.results = {}
        self.session = secrets.randbelow(65535)+1

    def connect(self):
        self.transport.connect()
        if not self.transport.discover():
            raise RuntimeError('service discovery failed')
        self.max_write = self.mtu-3
        if isinstance(self.transport, LocalBleTransport):
            characteristic = self.transport.client.services.get_characteristic(CTRL[1])
            if characteristic is None or 'write-without-response' not in characteristic.properties:
                raise RuntimeError('private command characteristic unavailable')
            self.max_write = characteristic.max_write_without_response_size
        self.transport.set_notify(CTRL[0], CTRL[2], True)
        if not isinstance(self.transport, LocalBleTransport):
            time.sleep(2.5)
        # Stock authentication avoids 2.2.9's ~30 s disconnect for an otherwise
        # unauthenticated connection. It runs before uploading or timing.
        authenticate(self.transport)

    def receive(self, wait):
        characteristic, frame = self.transport.notes.get(timeout=wait)
        if characteristic.lower() != CTRL[2]:
            return None
        result = parse_result(frame)
        if result and result[:2] == (self.lens, self.session):
            _, _, index, value = result
            self.results[index] = value
        return frame

    def command(self, op, args=b'', result_index=None):
        if result_index is not None:
            self.results.pop(result_index, None)
        payload = message(op, self.session, args)
        seq = self.next_sequence
        self.next_sequence = (seq+17)&255
        packets = make_packets([payload], self.mtu, seq, self.lens, max_write=self.max_write)
        for packet in packets:
            self.transport.write(CTRL[0], CTRL[1], packet.hex(), 1)
        expected = (seq, 0, self.lens, len(payload), int.from_bytes(crc16(payload), 'little'))
        deadline = time.monotonic()+self.timeout
        accepted = False
        while not accepted or (result_index is not None and result_index not in self.results):
            left = deadline-time.monotonic()
            if left <= 0:
                raise TimeoutError(f'command {op} missing ACK/result')
            frame = self.receive(left)
            acks = parse_acks(frame) if frame else None
            if acks and any(row[:3] == expected[:3] for row in acks) and frame[8] == 3:
                raise RuntimeError(f'command {op} rejected (NACK); no valid timing result')
            if acks and expected in acks:
                accepted = True
        return self.results.get(result_index)

    def read(self, index):
        return self.command(READ, struct.pack('<H', index), result_index=index)

    def close_session(self):
        """Wait briefly for the worker to park before releasing its code/stack."""
        deadline = time.monotonic()+3
        while True:
            try:
                self.command(CLOSE)
                return [self.command(HEAPS,struct.pack('<H',i),result_index=i) for i in range(6)]
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.1)


def summarize(words, reference, setup, skip, uncached):
    """Void the entire run on any identity, status, count or Y-hash mismatch."""
    expected_length = HEADER_WORDS+FRAMES*(PASSES+WARMUPS)*4
    profile = next((p for p in PROFILES if p['sha256']==reference.get('clip_sha256')),None)
    if (len(words) != expected_length or words[0] != MAGIC or words[1] != (SETUPS[setup] | skip<<8 | uncached<<16)
            or not profile or words[2] or words[3] != expected_length
            or words[4:7] != [FRAMES,PASSES,profile['crc32']]
            or words[12:15] != profile['dimensions']+[profile['bytes']]
            or words[37] != WARMUPS or reference.get('decoder') != '59c66b1'):
        raise ValueError('run metadata invalid; timing result void')
    rows, frame_rows = [], []
    for frame in range(FRAMES):
        samples = []
        for run in range(PASSES+WARMUPS):
            i = HEADER_WORDS+(run*FRAMES+frame)*4
            cycles, hashed, ticks, rate = words[i:i+4]
            if (hashed != reference['hashes'][str(skip)][frame] or not 1000 <= rate <= 500000
                    or not 0 < cycles <= 1000000000 or ticks > 2000
                    or cycles > rate*(ticks+2) or (ticks>2 and cycles < rate*(ticks-2))):
                raise ValueError(f'frame {frame}, repeat {run}: Y/clock mismatch; entire run void')
            ms = cycles/rate
            if run < WARMUPS:
                continue
            samples.append(ms)
            rows.append(dict(frame=frame, repeat=run, cycles=cycles, ticks=ticks, hash=hashed,
                             cycles_per_ms=rate, effective_mhz=rate/1000, ms=ms))
        frame_rows.append(dict(frame=frame, type='IDR' if frame == 0 else 'P',
            median_ms=statistics.median(samples), min_ms=min(samples), max_ms=max(samples)))
    return dict(valid=True, setup=setup, skip_chroma=bool(skip), uncached_frames_tables=bool(uncached),
        dimensions=profile['dimensions'], pool_slots=[words[i:i+2] for i in range(40,104,2) if words[i]],
        samples=rows, frames=frame_rows, median_ms=statistics.median([row['ms'] for row in rows]),
        min_ms=min(row['ms'] for row in rows), max_ms=max(row['ms'] for row in rows),
        heap_stats_before=words[16:22], heap_stats_reserved=words[22:28],
        memory_addresses=words[28:34], hot_bytes=words[34], state_bytes=words[35],
        pool_highwater=words[36], stack_highwater=words[38], capsule_bytes=words[7], mpu_control=words[9],
        cpu_control=words[8], cache_control=words[10], cache_type=words[11],
        clock_source='DWT CYCCNT, calibrated against donor 1 ms tick; interrupts/preemption included',
        cold_note='Fresh decoder per repeat: first IDR/P include lazy DPB/table initialization from reserved slots')


def run(args):
    raw = Path(args.clip).read_bytes()
    profile = check_clip(raw)
    reference = json.loads(Path(args.reference).read_text(encoding='utf-8'))
    if args.uncached and args.setup != 'full-mram':
        raise ValueError('uncached frames/tables option is full-mram only')
    connection = parse_connection_string(args.connection)
    bridge = None
    client = None
    session_open = False
    try:
        if connection['method'] == 'droidbridge':
            bridge = Bridge(connection['base'],connection['token']); bridge.start_ws()
            deadline = time.monotonic()+20
            while not bridge.ws_open:
                if time.monotonic() >= deadline: raise TimeoutError('DroidBridge websocket did not open')
                time.sleep(.05)
        transport = (DroidBridgeTransport(bridge,connection[args.lens]) if bridge else
            LocalBleTransport(connection[args.lens],connection['address_type'],side=args.lens))
        client = Client(transport,args.lens,args.mtu)
        client.connect()
        if client.command(HELLO,result_index=0xffff) != MAGIC:
            raise RuntimeError('wrong experiment firmware')
        client.command(BEGIN,struct.pack('<IIHH',profile['bytes'],profile['crc32'],*profile['dimensions']))
        session_open = True
        for offset in range(0,len(raw),2048):
            client.command(WRITE,struct.pack('<I',offset)+raw[offset:offset+2048])
        client.command(SEAL)
        client.results.pop(0xffff,None)
        client.command(RUN,bytes([SETUPS[args.setup],args.skip_chroma,5,args.uncached]))
        # Stay silent while decoding. All result reads follow completion or the
        # 60-second worker deadline; reads never perturb a valid timed run.
        deadline = time.monotonic()+75
        while 0xffff not in client.results and time.monotonic() < deadline:
            try: client.receive(min(1,deadline-time.monotonic()))
            except queue.Empty: pass
        phase = client.results.get(0xffff)
        if phase is None: phase = client.read(0xffff)
        count = client.read(3)
        if count > 1024 or count < HEADER_WORDS:
            raise RuntimeError('invalid result bound')
        words = [client.read(i) for i in range(count)]
        output = Path(args.output); output.parent.mkdir(parents=True,exist_ok=True)
        raw_path = output.with_suffix('.raw.json')
        raw_path.write_text(json.dumps(dict(phase=phase, words=words),indent=2)+'\n',encoding='utf-8')
        if phase != 4:
            raise RuntimeError(f'firmware run failed: phase={phase}, error={words[2]}; raw results {raw_path}')
        summary = summarize(words,reference,args.setup,args.skip_chroma,args.uncached)
        summary.update(lens=args.lens, clip_sha256=profile['sha256'], hardware_measurement=True)
        summary['heap_stats_released'] = client.close_session()
        session_open = False
        output.write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
        print(f'Y hashes PASS: {FRAMES*(PASSES+WARMUPS)} frames. Decode median {summary["median_ms"]:.3f} ms, '
              f'min {summary["min_ms"]:.3f}, max {summary["max_ms"]:.3f}. {output}')
    finally:
        if client:
            if session_open:
                try: client.close_session()
                except Exception as error: print(f'Close pending; session expires automatically: {error}',file=sys.stderr)
            client.transport.close()
        if bridge:
            bridge.close()
            if getattr(bridge,'ws',None) is not None: bridge.ws.close()


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='action',required=True)
    prep=sub.add_parser('prepare',help='make the approved ignored clip and PC hash reference; no connection')
    prep.add_argument('--source',required=True); prep.add_argument('--clip',required=True); prep.add_argument('--reference',required=True)
    measure=sub.add_parser('run',help='upload and measure one setup/mode on one lens; never flash')
    measure.add_argument('-c','--connection',required=True)
    measure.add_argument('--lens',choices=('left','right'),required=True)
    measure.add_argument('--setup',choices=tuple(SETUPS),required=True)
    measure.add_argument('--skip-chroma',type=int,choices=(0,1),required=True)
    measure.add_argument('--uncached',type=int,choices=(0,1),default=0)
    measure.add_argument('--mtu',type=int,default=23)
    measure.add_argument('--clip',required=True); measure.add_argument('--reference',required=True); measure.add_argument('--output',required=True)
    args=parser.parse_args(argv)
    try:
        if args.action=='prepare': prepare(args.source,args.clip,args.reference)
        else: run(args)
    except Exception as error:
        print(f'decoder speed test failed: {error}',file=sys.stderr); return 1
    return 0


if __name__=='__main__':
    raise SystemExit(main())
