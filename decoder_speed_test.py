#!/usr/bin/env python3
"""Measure the local Faceclaw/16 decoder experiment through one lens.

Prepare the clip/reference on the PC first. Run connects and uploads only;
this tool never flashes firmware. B also accepts the matched deblocking pair.
Batch keeps one authenticated connection for several clips and measurements.
"""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
from datetime import datetime
from types import SimpleNamespace
import queue
import re
import secrets
import statistics
import struct
import subprocess
import sys
import time
import zlib

import g2flash
from g2flash import Bridge, CTRL, DATA, DroidBridgeTransport, LocalBleTransport, authenticate, crc16, parse_connection_string
from send_message_probe import LENS_BITS, make_packets, parse_acks

MAGIC = 0x44530203
B_MAGIC = 0x4453020f
CLIP_BYTES = 23921
CLIP_SHA256 = 'c80a8087c83bed781374b3475987d9e772b517177ba244b250704046b354da0b'
CLIP_CRC = 0xc81c1bdc
PROFILES = (
    dict(name='tokyo-original',bytes=CLIP_BYTES,sha256=CLIP_SHA256,crc32=CLIP_CRC,dimensions=[320,192],deblocking='on'),
    dict(name='tokyo-deblock-on',bytes=22888,
         sha256='3e31c73ac4208be4b53f9cffc4b7921098cfa17de0f6d4c18730e5bb6bae4215',
         crc32=0xa7e2ccf1,dimensions=[320,192],deblocking='on'),
    dict(name='tokyo-deblock-off',bytes=23296,
         sha256='c68f8a3a113b6b0e3cf2aee3a5b9082ea52548a83f8927eddbec3b920e3756e8',
         crc32=0x65ebfb75,dimensions=[320,192],deblocking='off'),
)
C_MAGIC = 0x44530301
C_PROFILES = PROFILES + (
    dict(name='tokyo15-deblock-on', bytes=19038,
         sha256='88af67490140ac9355371f9dcb008339eafc958943feab35f97ca1ff655b6aa0',
         crc32=1344729742, dimensions=[320,192], deblocking='on'),
    dict(name='tokyo15-deblock-off', bytes=19672,
         sha256='52d8a8f474d9138480f37b58bb0c4b8b86a56fb5b0f010851aa2e11fac9315fb',
         crc32=2435330633, dimensions=[320,192], deblocking='off'),
)
HELLO, BEGIN, WRITE, SEAL, RUN, READ, ABORT, CLOSE, HEAPS, CAP_WRITE, CAP_SEAL, PROFILE_READ = range(12)
SETUPS = {'full-mram': 0, 'full-ram': 1, 'small-mram': 0, 'small-ram': 1,
          'mixed-mram': 2, 'mixed-ram': 3, 'capsule-ram': 4}
HEADER_WORDS, FRAMES, PASSES, WARMUPS = 128, 32, 5, 2
FRAME_WORDS = 5
RESULT_WORDS = HEADER_WORDS+FRAMES*(PASSES+WARMUPS)*FRAME_WORDS
OP_NAMES = dict(enumerate(('HELLO', 'BEGIN', 'WRITE', 'SEAL', 'RUN', 'READ', 'ABORT', 'CLOSE', 'HEAPS', 'CAP_WRITE', 'CAP_SEAL', 'PROFILE_READ')))


def progress(text, file=None):
    """Flush each step so a long scan, upload or result read stays visible."""
    print(f'[{datetime.now().astimezone().isoformat(timespec="seconds")}] {text}', file=file, flush=True)


def exception_text(error):
    """Name silent failures, including queue.Empty and backend timeouts."""
    return f'{type(error).__name__}: {str(error) or repr(error)}'


def message(op, session, args=b'', nonce=None):
    """Encode a versioned, bounded experiment command inside the old transport."""
    if not 0 <= op <= PROFILE_READ or not 0 <= session <= 65535:
        raise ValueError('invalid operation/session')
    if nonce is None:
        return b'\x1fDS\x02' + bytes([op]) + struct.pack('<H', session) + args
    if not 0 < nonce <= 0xffffffff:
        raise ValueError('invalid session nonce')
    return b'\x1fDS\x03' + bytes([op]) + struct.pack('<HI', session, nonce) + args


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
    for profile in C_PROFILES:
        if (len(raw)==profile['bytes'] and hashlib.sha256(raw).hexdigest()==profile['sha256']
                and zlib.crc32(raw)==profile['crc32']):
            return profile
    raise ValueError('the experiment requires a verified 32-frame reference clip')


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
        clip_name=profile['name'], deblocking=profile['deblocking'],
        dimensions=profile['dimensions'], frames=FRAMES, decoder='59c66b1', hashes=hashes)
    reference_path.write_text(json.dumps(reference, indent=2)+'\n', encoding='utf-8')
    progress(f'PC reference PASS: 448 frames, both modes and seven repeats; {reference_path}')


class MeasurementBleTransport(LocalBleTransport):
    """Windows paired-address connection; upstream's flasher stays unchanged."""
    def connect(self):
        if os.name != 'nt':
            # CoreBluetooth needs the scanned BLEDevice backend details.
            previous = g2flash.SCAN_TIMEOUT
            try:
                g2flash.SCAN_TIMEOUT = 60
                return super().connect()
            finally:
                g2flash.SCAN_TIMEOUT = previous
        from bleak import BleakClient
        from bleak.backends.device import BLEDevice

        async def connect_direct():
            device = BLEDevice(self.address, f'Even G2 {self.side} lens', None)
            self.disconnected_event.clear()
            self.disconnected_at = self.disconnect_detail = None
            self._disconnect_requested = False
            def on_disconnect(_client):
                self.disconnected_at = time.monotonic()
                self.disconnected_event.set()
            # Preserve Windows' default service cache. Uncached discovery on
            # paired lenses failed with "Catastrophic failure" on real hardware.
            self.client = BleakClient(device, disconnected_callback=on_disconnect)
            await self.client.connect(timeout=30)
            self._install_disconnect_error_hook()
        progress(f'Connecting directly to the paired {self.side} lens; no scan')
        self._call(connect_direct())


def firmware_magic(setup):
    """Keep A/B compatibility; uploaded capsules select C's nonce protocol."""
    if setup=='capsule-ram': return C_MAGIC
    return B_MAGIC if setup.startswith(('small-','mixed-')) else MAGIC


class Client:
    """One direct lens, old SID-f0 framing, fresh command stream per transaction."""
    def __init__(self, transport, lens, mtu=23, timeout=20):
        self.transport, self.lens, self.mtu, self.timeout = transport, LENS_BITS[lens], mtu, timeout
        self.next_sequence = secrets.randbelow(256)
        self.results = {}
        self.session = secrets.randbelow(65535)+1
        self.nonce = None

    def connect(self):
        last_error = None
        for connection in range(1, 4):
            try:
                progress(f'Opening connection {connection} of 3; both lenses must be paired with Windows')
                self.transport.connect()
                progress('Connected; checking the private services')
                if not self.transport.discover():
                    raise RuntimeError('service discovery failed')
                self.max_write = self.mtu-3
                if isinstance(self.transport, LocalBleTransport):
                    characteristic = self.transport.client.services.get_characteristic(CTRL[1])
                    if characteristic is None or 'write-without-response' not in characteristic.properties:
                        raise RuntimeError('Windows reports incomplete private command services')
                    self.max_write = characteristic.max_write_without_response_size
                progress('Enabling data and control notifications; settling for 2.5 s')
                self.transport.set_notify(DATA[0], DATA[2], True)
                self.transport.set_notify(CTRL[0], CTRL[2], True)
                time.sleep(2.5)
                for login in range(1, 4):
                    while True:
                        try: self.transport.notes.get_nowait()
                        except queue.Empty: break
                    progress(f'Authenticating: login {login} of 3 on connection {connection}')
                    try:
                        authenticate(self.transport)
                        progress(f'Logged in; largest write {self.max_write} bytes')
                        return
                    except TimeoutError as error:
                        last_error = error
                        progress(f'Login unanswered: {exception_text(error)}')
                raise TimeoutError('three login attempts received no reply') from last_error
            except (Exception, asyncio.CancelledError) as error:
                last_error = error
                progress(f'Connection {connection} failed: {exception_text(error)}; rebuilding the connection')
                try: self.transport.disconnect()
                except (Exception, asyncio.CancelledError): pass
                if connection < 3:
                    time.sleep(10)
        raise RuntimeError(f'lens connection/login failed after three connections: {exception_text(last_error)}') from last_error

    def new_session(self):
        """Clear old results while retaining the authenticated link and sequence."""
        previous = self.session
        self.session = (previous + secrets.randbelow(65534)+1) % 65535 or 65535
        self.results.clear()
        if self.nonce is not None:
            self.nonce = secrets.randbelow(0xffffffff)+1

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
        if op not in (READ, PROFILE_READ):
            detail = f' offset={struct.unpack("<I", args[:4])[0]}' if op in (WRITE, CAP_WRITE) else ''
            progress(f'Sending {OP_NAMES[op]}{detail}')
        if result_index is not None:
            self.results.pop(result_index, None)
        payload = message(op, self.session, args, self.nonce)
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
    c_mode = setup == 'capsule-ram'
    if c_mode and (not skip or uncached):
        raise ValueError('C requires cached chroma-skipped decoding')
    passes = 1 if c_mode and len(words)>114 and words[114] in (1,2) else PASSES
    warmups = 1 if passes == 1 else WARMUPS
    expected_length = HEADER_WORDS+FRAMES*(passes+warmups)*FRAME_WORDS
    profile = next((p for p in C_PROFILES if p['sha256']==reference.get('clip_sha256')),None)
    if (len(words) != expected_length or words[0] != firmware_magic(setup) or words[1] != (SETUPS[setup] | skip<<8 | uncached<<16)
            or not profile or words[2] or words[3] != expected_length
            or words[4:7] != [FRAMES,passes,profile['crc32']]
            or words[12:15] != profile['dimensions']+[profile['bytes']]
            or words[15] != FRAME_WORDS or words[37] != warmups or reference.get('decoder') != '59c66b1'
            or any(type(word) is not int or not 0 <= word <= 0xffffffff for word in words)):
        raise ValueError('run metadata invalid; timing result void')
    hashes = reference.get('hashes', {}).get(str(skip))
    if not isinstance(hashes, list) or len(hashes) != FRAMES:
        raise ValueError('PC hash reference format invalid; entire run void')
    rows, frame_rows = [], []
    for frame in range(FRAMES):
        samples = []
        for run in range(passes+warmups):
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
            row = dict(frame=frame, repeat=run, warmup=run<warmups, cycles=cycles, ticks=ticks, hash=hashed,
                clock_before=before, clock_after=after, clock_before_mhz=before/1000, clock_after_mhz=after/1000,
                cycles_per_ms=rate, effective_mhz=rate/1000 if rate else None,
                ms=cycles/rate if rate else None, cycle_ms=cycles/rate if rate else None,
                tick_ms=float(ticks), flagged=bool(flags), timing_flags=flags)
            rows.append(row)
            if not row['warmup'] and not flags:
                samples.append(row)
        frame_rows.append(dict(frame=frame, type='IDR' if frame == 0 else 'P',
            included=len(samples), excluded=passes-len(samples), **timing_summary(samples)))
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


def run_case(client, args, raw, profile, reference):
    """One bounded session; successful CLOSE is required before the next case."""
    session_open = False
    capsule = getattr(args, 'capsule_data', None)
    try:
        begin = capsule_begin(profile, capsule) if capsule else struct.pack('<IIHH',profile['bytes'],profile['crc32'],*profile['dimensions'])
        client.command(BEGIN, begin)
        session_open = True
        if capsule:
            code, manifest = capsule
            progress(f'Uploading sealed capsule {manifest["name"]}: {len(code)} bytes')
            for offset in range(0,len(code),2048):
                client.command(CAP_WRITE,struct.pack('<I',offset)+code[offset:offset+2048])
            client.command(CAP_SEAL)
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
        summary.update(lens=args.lens, clip_sha256=profile['sha256'], clip_name=profile['name'],
                       deblocking=profile['deblocking'], hardware_measurement=True)
        if capsule:
            code, manifest = capsule
            if (words[7]!=len(code) or words[115]!=client.nonce
                    or words[114]!={'profile':1,'profile-control':2}.get(manifest['kind'],0)
                    or words[110]!=len(manifest['profile_functions'])
                    or b''.join(struct.pack('<I',w) for w in words[116:124]).hex()!=manifest['sha256']):
                raise ValueError('capsule identity/nonce mismatch; entire run void')
            summary.update(candidate=manifest['name'], capsule_sha256=manifest['sha256'], capsule_kind=manifest['kind'])
            if manifest['profile_functions']:
                summary['function_profile'] = read_profile(client,words,manifest)
        progress('Closing the session and checking released heaps')
        summary['heap_stats_released'] = client.close_session()
        session_open = False
        output.write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
        progress(f'Y hashes PASS: {len(summary["samples"])} frames; '
                 f'excluded {summary["excluded_frames"]}/{summary["measured_frames"]} measured frames '
                 f'for timing anomalies ({summary["warmup_flagged_frames"]} flagged warmups).')
        if summary['timing_valid']:
            progress(f'Decode cycles: median {summary["median_ms"]:.3f} ms, min {summary["min_ms"]:.3f}, '
                     f'max {summary["max_ms"]:.3f}; ticks: median {summary["median_tick_ms"]:.3f} ms, '
                     f'min {summary["min_tick_ms"]:.3f}, max {summary["max_tick_ms"]:.3f}.')
        else:
            progress('No reliable timing samples remain; Y correctness passed and raw evidence is retained.')
        progress(f'Summary saved: {output}')
        return summary
    finally:
        if session_open:
            try: client.close_session()
            except Exception as error:
                progress(f'Close pending; session expires automatically: {exception_text(error)}', file=sys.stderr)


def measure(args, cases):
    """Authenticate once; stop on a failed case without replaying a timed run."""
    progress('Reading and validating every clip and PC Y-hash reference')
    inputs = {}
    for case in cases:
        capsule_path=getattr(case,'capsule',None)
        if capsule_path:
            if case.setup!='capsule-ram' or case.skip_chroma!=1 or case.uncached:
                raise ValueError('C capsules require capsule-ram, skip_chroma=1 and cached data')
            case.capsule_data=load_capsule(capsule_path)
        elif case.setup=='capsule-ram':
            raise ValueError('capsule-ram requires a checked capsule manifest')
        if case.uncached and case.setup != 'full-mram':
            raise ValueError('uncached frames/tables option is full-mram only')
        key = (str(Path(case.clip).resolve()), str(Path(case.reference).resolve()))
        if key not in inputs:
            raw = Path(case.clip).read_bytes()
            profile = check_clip(raw)
            reference = json.loads(Path(case.reference).read_text(encoding='utf-8'))
            hashes = reference.get('hashes', {})
            if (reference.get('decoder') != '59c66b1' or reference.get('clip_sha256') != profile['sha256']
                    or reference.get('clip_crc32') != profile['crc32']
                    or reference.get('dimensions') != profile['dimensions'] or reference.get('frames') != FRAMES
                    or any(len(hashes.get(str(skip), [])) != FRAMES for skip in ((1,) if capsule_path else (0,1)))
                    or (not capsule_path and hashes.get('0') != hashes.get('1'))):
                raise ValueError('PC reference identity/format invalid')
            inputs[key] = (raw, profile, reference)
        if case.setup.startswith('full-') and inputs[key][1]['name'] != 'tokyo-original':
            raise ValueError('firmware A accepts only the original Tokyo clip')
    if len({firmware_magic(case.setup) for case in cases}) != 1:
        raise ValueError('batch cases must belong to the same firmware (A or B)')
    connection = parse_connection_string(args.connection)
    if cases[0].setup=='capsule-ram' and connection['method']!='local':
        raise ValueError('uploaded capsules require a direct local lens connection')
    bridge = client = None
    try:
        if connection['method'] == 'droidbridge':
            progress('Opening the DroidBridge websocket')
            bridge = Bridge(connection['base'],connection['token']); bridge.start_ws()
            deadline = time.monotonic()+20
            while not bridge.ws_open:
                if time.monotonic() >= deadline: raise TimeoutError('DroidBridge websocket did not open')
                time.sleep(.05)
        transport = (DroidBridgeTransport(bridge,connection[args.lens]) if bridge else
            MeasurementBleTransport(connection[args.lens],connection['address_type'],side=args.lens))
        client = Client(transport,args.lens,args.mtu)
        if cases[0].setup=='capsule-ram': client.nonce=secrets.randbelow(0xffffffff)+1
        client.connect()
        if client.command(HELLO,result_index=0xffff) != firmware_magic(cases[0].setup):
            raise RuntimeError('wrong experiment firmware for the selected setups')
        summaries = []
        for index, case in enumerate(cases):
            key = (str(Path(case.clip).resolve()), str(Path(case.reference).resolve()))
            raw, profile, reference = inputs[key]
            client.new_session()
            progress(f'Batch case {index+1}/{len(cases)}: {profile["name"]}, {case.setup}, '
                     f'skip_chroma={case.skip_chroma}; same connection')
            summaries.append(run_case(client,case,raw,profile,reference))
            if index+1 < len(cases):
                progress('Session released; pausing 2 s before the next case')
                time.sleep(2)
        return summaries
    finally:
        if client:
            progress('Disconnecting the lens')
            client.transport.close()
        if bridge:
            bridge.close()
            if getattr(bridge,'ws',None) is not None: bridge.ws.close()


def run(args):
    return measure(args, [args])


def deblocking_comparisons(summaries):
    """Compare the matched re-encodes only when both runs have valid timing."""
    runs = {(row.get('setup'), row.get('skip_chroma'), row.get('clip_name')): row for row in summaries}
    comparisons = []
    for setup, skip in dict.fromkeys((row.get('setup'), row.get('skip_chroma')) for row in summaries):
        on, off = (runs.get((setup,skip,'tokyo-deblock-'+mode)) for mode in ('on','off'))
        if on is None or off is None:
            continue
        row = dict(setup=setup, skip_chroma=skip,
                   timing_valid=bool(on.get('timing_valid') and off.get('timing_valid')),
                   excluded_on=on['excluded_frames'], excluded_off=off['excluded_frames'])
        for field, label in (('median_ms','cycles'), ('median_tick_ms','ticks')):
            before, after = on[field], off[field]
            row[label] = dict(on_ms=before, off_ms=after,
                saving_percent=100*(before-after)/before if row['timing_valid'] and before and after is not None else None)
        comparisons.append(row)
    return comparisons


def batch(args):
    """Run a clip/setup/mode matrix on one lens over one connection."""
    if getattr(args,'capsule',None):
        return capsule_batch(args)
    directory = Path(args.output_dir)
    if directory.exists() and not directory.is_dir():
        raise ValueError('batch output must be a directory')
    cases = []
    clips = [args.clip] if isinstance(args.clip, (str, Path)) else args.clip
    references = [args.reference] if isinstance(args.reference, (str, Path)) else args.reference
    if len(clips) != len(references):
        raise ValueError('supply one PC reference for each clip, in the same order')
    profiles = [check_clip(Path(clip).read_bytes()) for clip in clips]
    if len({profile['name'] for profile in profiles}) != len(profiles):
        raise ValueError('batch contains the same clip more than once')
    for setup in dict.fromkeys(args.setups):
        for skip in dict.fromkeys(args.skip_chroma):
            for clip, reference, profile in zip(clips, references, profiles):
                case = SimpleNamespace(**vars(args))
                case.clip, case.reference = clip, reference
                case.setup, case.skip_chroma, case.uncached = setup, skip, 0
                case.output = directory/(f'{args.lens}-{profile["name"]}-{setup}-'+('skip' if skip else 'color')+'.json')
                cases.append(case)
    if ((directory/'batch-summary.json').exists() or (directory/'deblocking-comparison.json').exists()
            or any(case.output.exists() or case.output.with_suffix('.raw.json').exists() for case in cases)):
        raise FileExistsError('batch evidence already exists; choose a new output directory')
    summaries = measure(args, cases)
    Path(args.output_dir, 'batch-summary.json').write_text(json.dumps(summaries, indent=2)+'\n', encoding='utf-8')
    comparisons = deblocking_comparisons(summaries)
    if comparisons:
        (directory/'deblocking-comparison.json').write_text(json.dumps(comparisons, indent=2)+'\n', encoding='utf-8')
        for row in comparisons:
            timing = row['cycles']
            if row['timing_valid'] and timing['saving_percent'] is not None:
                progress(f'Deblocking {row["setup"]}, skip_chroma={int(row["skip_chroma"])}: '
                         f'{timing["on_ms"]:.3f} -> {timing["off_ms"]:.3f} ms; '
                         f'{timing["saving_percent"]:.1f}% saving; '
                         f'excluded on/off {row["excluded_on"]}/{row["excluded_off"]}.')
            else:
                progress(f'Deblocking {row["setup"]}, skip_chroma={int(row["skip_chroma"])}: '
                         'no reliable timing comparison; Y correctness passed.')
    progress(f'Batch complete: {len(summaries)} valid runs over one connection')



CAPSULE_EXPORTS = ('ds_size','ds_selftest','ds_init','ds_destroy','ds_decode','ds_frame')


def load_capsule(path):
    """Validate bytes and matching PC proofs before opening any connection."""
    path=Path(path)
    manifest=json.loads(path.read_text(encoding='utf-8'))
    raw=path.with_name('capsule.bin').read_bytes()
    checks=json.loads(path.with_name('checks.json').read_text(encoding='utf-8'))
    if (not isinstance(manifest.get('name'),str) or not re.fullmatch(r'[a-z0-9][a-z0-9.-]*',manifest['name'])
            or manifest.get('abi')!=1 or manifest.get('decoder')!='59c66b1'
            or not 16<=len(raw)<=156108 or manifest.get('bytes')!=len(raw)
            or hashlib.sha256(raw).hexdigest()!=manifest.get('sha256')
            or checks.get('result')!='PASS' or checks.get('sha256')!=manifest['sha256']
            or checks.get('lld') is not True or checks.get('pc_y') is not True):
        raise ValueError('capsule identity or PC proofs invalid')
    lo,hi=manifest['text_bounds']; exports=manifest['exports']
    if type(lo)is not int or type(hi)is not int or not 0<=lo<hi<=len(raw) or set(exports)!=set(CAPSULE_EXPORTS):
        raise ValueError('capsule executable/export bounds invalid')
    offsets=[exports[name] for name in CAPSULE_EXPORTS]
    if len(set(offsets))!=6 or any(type(x)is not int or x&1 or not lo<=x<hi for x in offsets):
        raise ValueError('capsule export offset invalid')
    functions=manifest['profile_functions']
    if (not isinstance(functions,dict) or any(type(i)is not int for i in functions.values())
            or manifest['kind'] not in ('full','small','mixed','profile','profile-control')
            or len(functions)>512 or sorted(functions.values())!=list(range(len(functions)))
            or bool(functions)!=(manifest['kind'] in ('profile','profile-control'))):
        raise ValueError('capsule profile manifest invalid')
    return raw,manifest


def capsule_begin(profile, capsule):
    code,m=capsule
    kind={'profile':1,'profile-control':2}.get(m['kind'],0)
    return struct.pack('<IIHHIIIII6I',profile['bytes'],profile['crc32'],*profile['dimensions'],
        len(code),*m['text_bounds'],(1<<16)|kind,len(m['profile_functions']),
        *(m['exports'][n] for n in CAPSULE_EXPORTS))+bytes.fromhex(m['sha256'])


def read_profile(client, words, manifest):
    """Pull counters after decoding; report corrupt/deep/wrapped profiles as unusable."""
    if words[110]!=len(manifest['profile_functions']):
        raise ValueError('profile count mismatch')
    rows=[]
    for name,identifier in sorted(manifest['profile_functions'].items(),key=lambda pair:pair[1]):
        values=[client.command(PROFILE_READ,struct.pack('<H',identifier*5+i),result_index=identifier*5+i) for i in range(5)]
        rows.append(dict(function=name,calls=values[0],inclusive_cycles=values[1]|values[2]<<32,
                         exclusive_cycles=values[3]|values[4]<<32))
    return dict(valid=words[111]==0, flags=words[111],depth_highwater=words[112],
                scratch_bytes=words[113],ranking_only=True,rows=sorted(rows,key=lambda row:-row['exclusive_cycles']))


def capsule_batch(args):
    """One lens/connection, fresh sealed owner per capsule and clip, no reconnect replay."""
    directory=Path(args.output_dir)
    if getattr(args,'skip_chroma',[1])!=[1]:
        raise ValueError('C capsules only support skip_chroma=1')
    if directory.exists() and any(directory.iterdir()):
        raise FileExistsError('capsule output directory must be empty; preserve prior evidence')
    clips=list(args.clip); references=list(args.reference)
    if len(clips)!=len(references) or len(set(map(str,clips)))!=len(clips):
        raise ValueError('one PC reference per distinct clip is required')
    candidates=[(path,load_capsule(path)[1]) for path in args.capsule]
    if len({m['name'] for _,m in candidates})!=len(candidates):
        raise ValueError('duplicate capsule names')
    directory.mkdir(parents=True,exist_ok=True)
    cases=[]
    for path,manifest in candidates:
        for clip,reference in zip(clips,references):
            profile=check_clip(Path(clip).read_bytes())
            output=directory/f'{args.lens}-{profile["name"]}-{manifest["name"]}-skip.json'
            cases.append(SimpleNamespace(setup='capsule-ram',skip_chroma=1,uncached=0,lens=args.lens,
                capsule=path,clip=clip,reference=reference,output=output))
    summaries=measure(args,cases)
    (directory/'batch-summary.json').write_text(json.dumps(summaries,indent=2)+'\n',encoding='utf-8')
    progress(f'Capsule batch complete: {len(summaries)} valid runs over one connection')

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
    measure.add_argument('--capsule', help='checked capsule.json; C requires capsule-ram/skip 1')
    measure.add_argument('--uncached',type=int,choices=(0,1),default=0)
    measure.add_argument('--mtu',type=int,default=23)
    measure.add_argument('--clip',required=True); measure.add_argument('--reference',required=True); measure.add_argument('--output',required=True)
    matrix=sub.add_parser('batch',help='measure several setup/chroma combinations over one connection; never flash')
    matrix.add_argument('-c','--connection',required=True)
    matrix.add_argument('--lens',choices=('left','right'),required=True)
    matrix.add_argument('--capsule', nargs='+', help='checked manifests; C batch defaults to skip 1')
    matrix.add_argument('--setups',nargs='+',choices=tuple(SETUPS),default=['small-mram','small-ram','mixed-mram','mixed-ram'])
    matrix.add_argument('--skip-chroma',nargs='+',type=int,choices=(0,1),default=[1])
    matrix.add_argument('--mtu',type=int,default=23)
    matrix.add_argument('--clip',nargs='+',required=True,help='one or more verified clips')
    matrix.add_argument('--reference',nargs='+',required=True,help='matching PC references in clip order')
    matrix.add_argument('--output-dir',required=True)
    args=parser.parse_args(argv)
    try:
        if args.action=='prepare': prepare(args.source,args.clip,args.reference)
        elif args.action=='batch': batch(args)
        else: run(args)
    except Exception as error:
        progress(f'Decoder speed test failed: {exception_text(error)}', file=sys.stderr); return 1
    return 0


if __name__=='__main__':
    raise SystemExit(main())
