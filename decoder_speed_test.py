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
from datetime import datetime
import queue
import secrets
import statistics
import struct
import subprocess
import sys
import time
import zlib

import g2flash
from g2flash import Bridge, CTRL, DroidBridgeTransport, LocalBleTransport, authenticate, crc16, parse_connection_string
from send_message_probe import LENS_BITS, make_packets, parse_acks

MAGIC = 0x44530203
CLIP_BYTES = 23921
CLIP_SHA256 = 'c80a8087c83bed781374b3475987d9e772b517177ba244b250704046b354da0b'
CLIP_CRC = 0xc81c1bdc
PROFILES = (
    dict(bytes=CLIP_BYTES,sha256=CLIP_SHA256,crc32=CLIP_CRC,dimensions=[320,192]),
)
HELLO, BEGIN, WRITE, SEAL, RUN, READ, ABORT, CLOSE, HEAPS = range(9)
SETUPS = {'full-mram': 0, 'full-ram': 1}
HEADER_WORDS, FRAMES, PASSES, WARMUPS = 128, 32, 5, 2
FRAME_WORDS = 5
RESULT_WORDS = HEADER_WORDS+FRAMES*(PASSES+WARMUPS)*FRAME_WORDS
SCAN_SECONDS = 60
OP_NAMES = dict(enumerate(('HELLO', 'BEGIN', 'WRITE', 'SEAL', 'RUN', 'READ', 'ABORT', 'CLOSE', 'HEAPS')))


def progress(text, file=None):
    """Flush each step so a long scan, upload or result read stays visible."""
    print(f'[{datetime.now().astimezone().isoformat(timespec="seconds")}] {text}', file=file, flush=True)


def exception_text(error):
    """Name silent failures, including queue.Empty and backend timeouts."""
    return f'{type(error).__name__}: {str(error) or repr(error)}'


def message(op, session, args=b''):
    """Encode a versioned, bounded experiment command inside the old transport."""
    if not 0 <= op <= HEAPS or not 0 <= session <= 65535:
        raise ValueError('invalid operation/session')
    return b'\x1fDS\x02' + bytes([op]) + struct.pack('<H', session) + args


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
    progress('Reading and checking the PC reference clip')
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
    progress('Compiling the PC decoder reference')
    subprocess.run(prefix+command, check=True)
    clip_path.write_bytes(raw)
    hashes = {}
    for skip in (0, 1):
        progress(f'PC reference: skip_chroma={skip}, seven passes')
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
    progress(f'PC reference PASS: 448 frames, both modes and seven repeats; {reference_path}')


class Client:
    """One direct lens, old SID-f0 framing, fresh command stream per transaction."""
    def __init__(self, transport, lens, mtu=23, timeout=20):
        self.transport, self.lens, self.mtu, self.timeout = transport, LENS_BITS[lens], mtu, timeout
        self.next_sequence = secrets.randbelow(256)
        self.results = {}
        self.session = secrets.randbelow(65535)+1

    def connect(self):
        if isinstance(self.transport, LocalBleTransport):
            progress(f'Scanning for the lens for up to {SCAN_SECONDS} s, then connecting; '
                     'both lenses must be paired with Windows')
            # Scope the longer scan to this PC measurement connection; leave
            # the upstream flasher and its normal timeout unchanged.
            previous_timeout = g2flash.SCAN_TIMEOUT
            try:
                g2flash.SCAN_TIMEOUT = SCAN_SECONDS
                self.transport.connect()
            finally:
                g2flash.SCAN_TIMEOUT = previous_timeout
        else:
            progress('Connecting through DroidBridge')
            self.transport.connect()
        progress('Connected; discovering the private service')
        if not self.transport.discover():
            raise RuntimeError('service discovery failed')
        self.max_write = self.mtu-3
        if isinstance(self.transport, LocalBleTransport):
            characteristic = self.transport.client.services.get_characteristic(CTRL[1])
            if characteristic is None or 'write-without-response' not in characteristic.properties:
                raise RuntimeError('private command characteristic unavailable')
            self.max_write = characteristic.max_write_without_response_size
        progress('Enabling private notifications')
        self.transport.set_notify(CTRL[0], CTRL[2], True)
        if not isinstance(self.transport, LocalBleTransport):
            time.sleep(2.5)
        # Stock authentication avoids 2.2.9's ~30 s disconnect for an otherwise
        # unauthenticated connection. It runs before uploading or timing.
        progress('Authenticating the lens')
        authenticate(self.transport)
        progress(f'Connected and authenticated; largest write {self.max_write} bytes')

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
        if op != READ:
            detail = f' offset={struct.unpack("<I", args[:4])[0]}' if op == WRITE else ''
            progress(f'Sending {OP_NAMES[op]}{detail}')
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
    expected_length = RESULT_WORDS
    profile = next((p for p in PROFILES if p['sha256']==reference.get('clip_sha256')),None)
    if (len(words) != expected_length or words[0] != MAGIC or words[1] != (SETUPS[setup] | skip<<8 | uncached<<16)
            or not profile or words[2] or words[3] != expected_length
            or words[4:7] != [FRAMES,PASSES,profile['crc32']]
            or words[12:15] != profile['dimensions']+[profile['bytes']]
            or words[15] != FRAME_WORDS or words[37] != WARMUPS or reference.get('decoder') != '59c66b1'
            or any(type(word) is not int or not 0 <= word <= 0xffffffff for word in words)):
        raise ValueError('run metadata invalid; timing result void')
    hashes = reference.get('hashes', {}).get(str(skip))
    if not isinstance(hashes, list) or len(hashes) != FRAMES:
        raise ValueError('PC hash reference format invalid; entire run void')
    rows, frame_rows = [], []
    for frame in range(FRAMES):
        samples = []
        for run in range(PASSES+WARMUPS):
            i = HEADER_WORDS+(run*FRAMES+frame)*FRAME_WORDS
            cycles, hashed, ticks, before, after = words[i:i+FRAME_WORDS]
            if hashed != hashes[frame]:
                raise ValueError(f'frame {frame}, repeat {run}: Y mismatch; entire run void')
            flags = []
            rate = (before+after)/2 if 1000 <= before <= 500000 and 1000 <= after <= 500000 else None
            if rate is None:
                flags.append('clock calibration unavailable/out of range')
            elif abs(after-before) > before*.04:
                flags.append('clock changed by more than 4%')
            if not 0 < cycles <= 1000000000 or ticks > 2000:
                flags.append('cycle/tick delta out of range')
            if rate is not None and (cycles > rate*(ticks+2) or (ticks>2 and cycles < rate*(ticks-2))):
                flags.append('cycles and ticks disagree (2 ms tick tolerance)')
            row = dict(frame=frame, repeat=run, warmup=run<WARMUPS, cycles=cycles, ticks=ticks, hash=hashed,
                clock_before=before, clock_after=after, clock_before_mhz=before/1000, clock_after_mhz=after/1000,
                cycles_per_ms=rate, effective_mhz=rate/1000 if rate else None,
                ms=cycles/rate if rate else None, cycle_ms=cycles/rate if rate else None,
                tick_ms=float(ticks), flagged=bool(flags), timing_flags=flags)
            rows.append(row)
            if not row['warmup'] and not flags:
                samples.append(row)
        frame_rows.append(dict(frame=frame, type='IDR' if frame == 0 else 'P',
            included=len(samples), excluded=PASSES-len(samples), **timing_summary(samples)))
    measured = [row for row in rows if not row['warmup']]
    included = [row for row in measured if not row['flagged']]
    return dict(valid=True, timing_valid=bool(included), setup=setup, skip_chroma=bool(skip), uncached_frames_tables=bool(uncached),
        dimensions=profile['dimensions'], pool_slots=[words[i:i+2] for i in range(40,104,2) if words[i]],
        samples=rows, frames=frame_rows, **timing_summary(included),
        measured_frames=len(measured), included_frames=len(included), excluded_frames=len(measured)-len(included),
        flagged_frames=sum(row['flagged'] for row in rows),
        warmup_flagged_frames=sum(row['flagged'] for row in rows if row['warmup']),
        heap_stats_before=words[16:22], heap_stats_reserved=words[22:28],
        memory_addresses=words[28:34], hot_bytes=words[34], state_bytes=words[35],
        pool_highwater=words[36], stack_highwater=words[38], capsule_bytes=words[7], mpu_control=words[9],
        cpu_control=words[8], cache_control=words[10], cache_type=words[11],
        clock_source='DWT CYCCNT, calibrated against donor 1 ms tick; interrupts/preemption included',
        cold_note='Fresh decoder per repeat: first IDR/P include lazy DPB/table initialization from reserved slots')


def timing_summary(samples):
    """Return both time sources, or null summaries when all samples were flagged."""
    values = {}
    for field, suffix in (('ms', 'ms'), ('tick_ms', 'tick_ms')):
        times = [row[field] for row in samples]
        values.update({f'median_{suffix}': statistics.median(times) if times else None,
                       f'min_{suffix}': min(times) if times else None,
                       f'max_{suffix}': max(times) if times else None})
    return values


def run(args):
    progress('Reading and validating the clip and PC Y-hash reference')
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
            progress('Opening the DroidBridge websocket')
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
        progress(f'Uploading {len(raw)} clip bytes in 2 KiB chunks')
        for offset in range(0,len(raw),2048):
            client.command(WRITE,struct.pack('<I',offset)+raw[offset:offset+2048])
        client.command(SEAL)
        progress('Upload sealed; reserving and starting the decoder worker')
        client.results.pop(0xffff,None)
        client.command(RUN,bytes([SETUPS[args.setup],args.skip_chroma,5,args.uncached]))
        # Stay silent while decoding. All result reads follow completion or the
        # 60-second worker deadline; reads never perturb a valid timed run.
        deadline = time.monotonic()+75
        next_progress = time.monotonic()
        while 0xffff not in client.results and time.monotonic() < deadline:
            if time.monotonic() >= next_progress:
                progress(f'Waiting for completion ({max(0, deadline-time.monotonic()):.0f} s remaining); no BLE polling')
                next_progress = time.monotonic()+5
            try: client.receive(min(1,deadline-time.monotonic()))
            except queue.Empty: pass
        phase = client.results.get(0xffff)
        if phase is None: phase = client.read(0xffff)
        progress(f'Worker phase {phase}; reading result bounds')
        count = client.read(3)
        if count > RESULT_WORDS or count < HEADER_WORDS:
            raise RuntimeError('invalid result bound')
        words = []
        for i in range(count):
            if i == 0 or (i+1)%100 == 0 or i+1 == count:
                progress(f'Reading results: value {i+1}/{count}')
            words.append(client.read(i))
        output = Path(args.output); output.parent.mkdir(parents=True,exist_ok=True)
        raw_path = output.with_suffix('.raw.json')
        raw_path.write_text(json.dumps(dict(phase=phase, words=words),indent=2)+'\n',encoding='utf-8')
        progress(f'Raw evidence saved: {raw_path}')
        if phase != 4:
            raise RuntimeError(f'firmware run failed: phase={phase}, error={words[2]}; raw results {raw_path}')
        progress('Checking every Y hash and flagging timing anomalies')
        summary = summarize(words,reference,args.setup,args.skip_chroma,args.uncached)
        summary.update(lens=args.lens, clip_sha256=profile['sha256'], hardware_measurement=True)
        progress('Closing the session and checking released heaps')
        summary['heap_stats_released'] = client.close_session()
        session_open = False
        output.write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
        progress(f'Y hashes PASS: {FRAMES*(PASSES+WARMUPS)} frames; '
                 f'excluded {summary["excluded_frames"]}/{summary["measured_frames"]} measured frames '
                 f'for timing anomalies ({summary["warmup_flagged_frames"]} flagged warmups).')
        if summary['timing_valid']:
            progress(f'Decode cycles: median {summary["median_ms"]:.3f} ms, min {summary["min_ms"]:.3f}, '
                     f'max {summary["max_ms"]:.3f}; ticks: median {summary["median_tick_ms"]:.3f} ms, '
                     f'min {summary["min_tick_ms"]:.3f}, max {summary["max_tick_ms"]:.3f}.')
        else:
            progress('No reliable timing samples remain; Y correctness passed and raw evidence is retained.')
        progress(f'Summary saved: {output}')
    finally:
        if client:
            if session_open:
                try: client.close_session()
                except Exception as error:
                    progress(f'Close pending; session expires automatically: {exception_text(error)}', file=sys.stderr)
            progress('Disconnecting the lens')
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
        progress(f'Decoder speed test failed: {exception_text(error)}', file=sys.stderr); return 1
    return 0


if __name__=='__main__':
    raise SystemExit(main())
