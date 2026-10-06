# Decoder measurement, firmwares A, B and C

This experiment uses jim's `306e42e` (Even 2.2.9.22, `Faceclaw/16`). It retains
that identity solely for this local measurement and does not implement the video
port. Sub0h264 is unchanged from the G2 decoder commit `59c66b1`. A supplies
`full-mram` and `full-ram` from one `-O2` capsule. B supplies `small-mram`,
`small-ram`, `mixed-mram` and `mixed-ram`: 58,692-byte `-Oz` and 66,600-byte
mixed capsules. Each RAM setup copies its corresponding complete code and
constants into executable heap 27. All setups offer chroma on or skipped.

B partitions pre-optimization LLVM IR from unchanged headers. Luma motion
compensation, luma intra prediction, inverse transforms/dequantization and
luma deblocking compile at `-O2`; other functions compile at `-Oz`, including
its `minsize`/`optsize` attributes. Constants have one owner, cross-module
symbols have hidden, non-preemptible linkage, and no function definition is duplicated.
There is no cross-module inlining or LTO. Unused sections are removed while
retaining all reachable decoder features. The exact membership, compiler flags,
sections and export offsets are generated under ignored `obj/`.

Nothing allocates experiment state or starts a worker before an explicit mode-31
command. Default allocated decoder tables, clip, RBSP, worker stack and frame
planes use cached heap 20. Read-only constants remain with the capsule.
A's `--uncached 1` moves only the frame planes and allocated tables into heap
13, restricted to `full-mram`. B rejects this option and always uses cached
heap 20. No allocator cache is evicted. Live free/largest-block checks retain
32 KiB in heap 20 and 16 KiB in heap 27; allocation failures roll back owned
blocks. The real glasses reported heap-27 free/max 133,924/131,072 bytes with
Faceclaw stopped: A's full-speed RAM capsule is too big; both B capsules fit
those values with the reserve. Each new run still checks live availability.

The original clip is a frozen 32-frame 320x192 Tokyo segment, 23,921 bytes,
SHA-256 `c80a8087c83bed781374b3475987d9e772b517177ba244b250704046b354da0b`.
It contains an IDR followed by 31 P frames, one reference and NALs <=1,466 bytes.
B additionally accepts two matched re-encodes of the same source pictures:

| Clip | Bytes | CRC32 | SHA256 |
|---|---:|---|---|
| tokyo-deblock-on | 22,888 | a7e2ccf1 | 3e31c73ac4208be4b53f9cffc4b7921098cfa17de0f6d4c18730e5bb6bae4215 |
| tokyo-deblock-off | 23,296 | 65ebfb75 | c68f8a3a113b6b0e3cf2aee3a5b9082ea52548a83f8927eddbec3b920e3756e8 |

Each has SPS/PPS, one IDR and 31 P frames, Baseline/CAVLC, ref 1, no I_PCM,
and no 8x8 transform. Every off-clip slice has `disable_deblocking_filter_idc=1`;
the on clip has 0. The decoder already honors this field; its source is unchanged.
The upload ceiling remains 23,921 bytes. Firmware validates each frozen length/
CRC pair; the PC additionally checks its SHA256 and matching Y reference.
A accepts only the original clip. Prepare each private segment/reference before
connecting. Chroma modes must give identical Y within each clip; the two encoded
clips can have different pixels, so each needs its own reference.
Clip bytes, per-frame Y references, generated capsule bytes and measurement results stay untracked.
Neither firmware accepts 640x384. The optional probe used unsupported High
profile CAVLC plus the 8x8 transform (FM-3), also failing the older decoder.
Any future 2x probe must be encoded Baseline, without 8x8dct.

## Commands

From the repository, prepare the private sample/reference without connecting:

```powershell
venv\Scripts\python.exe decoder_speed_test.py prepare --source <Tokyo.h264> --clip <private-clip.h264> --reference <private-reference.json>
```

For A, measure each lens and both setups with `--skip-chroma 0` and `1`:

```powershell
venv\Scripts\python.exe decoder_speed_test.py run -c 'g2://local?left=<left-address>&right=<right-address>&addressType=public' --lens left --setup full-mram --skip-chroma 0 --clip <private-clip.h264> --reference <private-reference.json> --output <private-result.json>
```

Change `--setup` to `full-ram` for the executable copy, or add `--uncached 1`
to `full-mram` to measure uncached frames/tables. The tool authenticates the
connection, uploads and measures; it never flashes. Keep the phone disconnected
during measurement. On Windows, pair both lenses first: an unpaired connection
can be cut after about 35 seconds despite protocol authentication. Supply both
addresses and `addressType=public` in the connection URL; put the URL in single
quotes in PowerShell so its `&` separators are passed literally. Allow a lens
to recover after a failed session before retrying. Each step prints flushed,
timestamped progress, and exceptions include their type even with no message.
Flash only a separately reviewed boot-PASS copy, using
`g2flash.py`; first use its connection-only `--stop-before file_check` gate.
Return afterwards using Faceclaw's Install firmware button, or a reviewed copy
of the unmodified firmware matching the installed app version.

On Windows the measurement tool connects directly to the paired address as a
Bleak `BLEDevice`, without scanning. Windows may already hold a paired lens
connected, so it need not advertise. Default service caching is retained;
forcing uncached discovery failed on the glasses. Login enables both data and
control notifications, settles for 2.5 seconds, drains stale notifications and
retries unanswered login three times. Connection, incomplete-service,
access-denied and cancellation failures rebuild the connection, up to three
connections with a 10-second pause. These retries occur before measurements;
failed or ambiguous timed runs are never replayed automatically. The upstream
flasher is unchanged. On other platforms its scanned-device path is retained.

For B, run all eight cases on one lens over one authenticated connection:

```powershell
venv\Scripts\python.exe decoder_speed_test.py batch -c 'g2://local?left=<left-address>&right=<right-address>&addressType=public' --lens left --setups small-mram small-ram mixed-mram mixed-ram --skip-chroma 0 1 --clip <private-clip.h264> --reference <private-reference.json> --output-dir <private-results-directory>
```

For the deblocking comparison, supply both clips and their references in order
to run 16 cases on that same connection. Adding the original makes 24:

```powershell
venv\Scripts\python.exe decoder_speed_test.py batch -c 'g2://local?left=<left-address>&right=<right-address>&addressType=public' --lens left --setups small-mram small-ram mixed-mram mixed-ram --skip-chroma 0 1 --clip <deblock-on.h264> <deblock-off.h264> --reference <on-reference.json> <off-reference.json> --output-dir <new-private-results-directory>
```

Within each setup/chroma combination the clips run consecutively. Outputs
include clip names. After the complete batch passes, `deblocking-comparison.json`
reports on/off median milliseconds, percentage saving for cycles and ticks,
and excluded-frame counts. An all-flagged run yields no invented gain.

Each case gets a fresh upload/decoder session, raw evidence and validated
summary. CLOSE must release the parked worker before the next case; a 2-second
pause follows. The connection and sequence counter survive across cases.
Any failed case stops the batch; preceding results are retained. A final
`batch-summary.json` is written only after every case passes. Existing case/raw
evidence, a batch summary or a deblocking comparison is never overwritten. Choose a new
output directory for each batch. Run the other lens separately.

## Protocol and timing

The unchanged private SID-f0 message transport carries `31,'D','S',2,op,tokenLE16`
followed by operation arguments. A fresh ordinary transport stream per command
retains jim's compression, sequencing, CRC and ACK behavior. Operations are:

| op | Arguments | Purpose |
|---|---|---|
| HELLO (0) | none | A magic `0x44530203`, B magic `0x4453020f` |
| BEGIN (1) | bytesLE32, CRC32LE32, widthLE16, heightLE16 | reserve upload session |
| WRITE (2) | offsetLE32, 1..2048 clip bytes | sequential/idempotent upload |
| SEAL (3) | none | check exact length and CRC32 |
| RUN (4) | setup, skipChroma, repeats=5, uncached | reserve and start after ACK |
| READ (5) | indexLE16 | read completed result; `0xffff` reads phase |
| ABORT (6) | none | request cooperative stop |
| CLOSE (7) | none | terminate parked worker, then release session |
| HEAPS (8) | indexLE16=0..5 | live free/max for heaps 20/13/27, after CLOSE |

A setup IDs 0/1 mean full MRAM/RAM; B IDs 0/1/2/3 mean small MRAM/RAM and
mixed MRAM/RAM. B requires the uncached byte to be zero. The tool checks the
selected setup's firmware magic before uploading; result headers repeat it.

Replies fit ATT's minimum MTU: a ten-byte body `5,lens,tokenLE16,indexLE16,valueLE32`
uses the existing BLE envelope. A low-priority static worker uses a 16 KiB stack;
normal interrupts, scheduler and watchdog continue. Two warmups plus five fresh
decoder passes produce 224 hashes and 160 measured samples. The worker has a
60-second deadline; an abandoned session expires after 120 seconds.

DWT CYCCNT brackets only NAL parsing/decoding, with no display, hashing, cache
copy or BLE send in that bracket. Calibration against the donor's millisecond
tick before/after each slice reports cycles/ms and effective MHz. Timing anomalies
do not stop the decoder. Record cycles, Y hash, ticks, clock-before and clock-after
as five words per frame, including warmups: 128 header words plus 224 rows gives
1,248 words. Header word 15 specifies five words per row. Protocol version 2 and
the new HELLO magic reject an old tool/firmware pairing before upload.

The PC flags unavailable/out-of-range calibration, a before/after change greater
than 4% of the before rate, out-of-range deltas, or cycles/ticks disagreeing beyond
the 2 ms tick tolerance. Every sample retains both calibrated cycle milliseconds
(using the mean before/after rate) and integer tick milliseconds, plus its flags.
Only unflagged measured frames enter either summary; report the excluded count
and flagged warmups separately. If all samples are flagged, Y correctness can
still pass, with null timing summaries and no invented speed result. Interrupt and
preemption time remains included. First IDR/P samples include lazy DPB/table
initialization from already reserved pools; show them separately from steady P
frames. Report per-frame and pooled median/min/max of the five measured passes.

After each frame, CRC32/IEEE covers the entire contiguous Y plane before reuse.
Any PC hash mismatch, including warmups, voids the complete run. Raw results are
saved before validation. Results include heap snapshots, exact pool addresses,
stack/pool high-water marks and MPU/cache state; released heap stats follow CLOSE.

RAM copy checks privilege, MPU and cached/default-map placement first. It never
changes protection: copy and checksum, D-cache clean by CTR line size, DSB,
I-cache invalidate by line, DSB/ISB, then call exported Thumb offsets. The
controller remains in MRAM; executable RAM is freed only after the parked worker
has been terminated. Clock registers and the import slot are restored on exit.

## Offline validation

Run the repository's normal Sub0h264 tests on generic and default x86 platforms.
The PC reference also exercises the real capsule with its bounded allocator.
New tests need no emulator, Bluetooth or glasses:

```powershell
venv\Scripts\python.exe -m unittest discover -s tests/decoder_speed -p test_tool.py -v
wsl -d Ubuntu -- python3 -m unittest discover -s tests/decoder_speed -p test_link.py -v
wsl -d Ubuntu -- python3 -m unittest discover -s tests/decoder_speed -p test_partition.py -v
wsl -d Ubuntu -- clang -O1 -g '-fsanitize=address,undefined' -Itests/decoder_speed tests/decoder_speed/controller_test.c -o obj/ds_controller_test
wsl -d Ubuntu -- obj/ds_controller_test
wsl -d Ubuntu -- clang -O1 -g '-fsanitize=address,undefined' tests/decoder_speed/pool_test.c -o obj/ds_pool_test
wsl -d Ubuntu -- obj/ds_pool_test
wsl -d Ubuntu -- clang -O2 tests/decoder_speed/runtime_test.c patches/decoder_speed/runtime.c -o obj/ds_runtime_test
wsl -d Ubuntu -- obj/ds_runtime_test
```

`test_link.py` compares final bytes with independent LLD at MRAM/RAM addresses
and rejects writable state, undefined targets and absolute pointers.
For controller upload/CRC checks against real frozen clips, pass the original,
on and off file paths as arguments to `obj/ds_controller_test`; the test uploads
each through all four setup IDs and both chroma modes. Paths stay local.
Build with `bash build_cfw.sh --skip-venv --update-patches`, review the generated manifest,
pin the resulting hash and reproduce it before the independent boot gate.
All real execution timings and cache/RAM behavior remain to be measured on the
glasses; offline tests and a healthy boot do not establish decoder throughput.

## Firmware C: uploaded candidates and a function profile

C removes both embedded decoder capsules. It keeps the controller, bounded
allocator, clock/results code and a SHA256 upload check in MRAM. No decoder
worker or experiment buffer exists before a PC command. Decoder sources still
equal `59c66b1`; every C run requires chroma skipped and cached heap-20 data.
Code and read-only constants are uploaded into one 64-byte-aligned heap-27
allocation, with 128 bytes of slack and the existing 16 KiB reserve. C accepts
at most 156,108 capsule bytes, subject to live free/largest-block checks. A
phone session can leave too little heap 27 for the full endpoint; C refuses it
without evicting Even's allocations. No code is persisted or written to MRAM.

The existing public stock login is retained. It contains no secret; the nonce
and token identify a single owner and do not authenticate native code. C adds
no bonding/login hooks. Mode 31 must originate locally and target that one
lens; inter-lens bridge and both-lens commands are rejected while other private
messages keep their routing. The C tool also rejects DroidBridge connections.
Use trusted, PC-checked capsules only: a digest checks bytes, not code safety.
Uploaded native code can fault, hang or write memory despite these bounds.
This is a temporary measurement build, never part of Faceclaw or the port.

C's command header is `31,'D','S',3,op,tokenLE16,nonceLE32`; HELLO returns
`0x44530301`. The 88-byte BEGIN manifest contains clip length/CRC/dimensions,
capsule length, text bounds, ABI/kind, profile count, six even/distinct export
offsets and the declared SHA256. ABI 1 exports, in order, are `ds_size`,
`ds_selftest`, `ds_init`, `ds_destroy`, `ds_decode` and `ds_frame`. Offsets must
lie inside the declared executable bounds. Kind 0 is ordinary, 1 profiles,
and 2 is the matching profile control with accounting disabled.

CAP_WRITE (9) uses an offset and at most 2,048 code bytes. Writes must be
contiguous; only an identical already-written range can be retried. CAP_SEAL
(10) computes SHA256 over the complete capsule. Clip WRITE/SEAL remain separate.
Both seals are required before RUN, and sealed bytes cannot change. RUN accepts
only setup 4, skip 1, repeats 5, uncached 0. Ordinary capsules use two warmups
and five measured passes; profile/control use one warmup and one measured pass
to bound instrumentation time. No ambiguous RUN is replayed or resumed after
connection loss. The lease reclaims abandoned state; CLOSE waits for the worker
to park and terminate before freeing executable memory. The original MPU/cache
policy checks, clean/invalidate barriers, normal watchdog/interrupts and deadline
remain. C does not modify MPU mappings.

Result words 110-114 give profile count, validity flags, depth high-water,
scratch bytes and kind; 115 is the nonce; 116-123 repeat the capsule hash.
PROFILE_READ (11) pages five words per function: call count, inclusive cycles
(low/high), exclusive cycles (low/high). Only decoding is profiled: initialization,
destruction, calibration, hashes and BLE remain outside the accounting bracket.
The bounded scratch is 13,328 bytes in heap 20, with at most 512 rows and 64
active calls; depth, call-count, unbalanced-return and ambiguous-wrap failures
make a profile unusable. Inlined work belongs to its emitted caller. Profile
timings rank promotions and never become points on the final speed curve.

The profile and control have identical instrumented bytes. Compare both with
ordinary mixed on the glasses to report frame slowdown and estimated additional
cycles per hook using the recorded call count. This includes accounting overhead
and excludes untimed initialization; interrupt/preemption noise still applies.
If overhead changes rankings substantially, verify savings with uninstrumented
one-function/group promotion capsules before selecting the curve.

C adds a second matched Baseline/CAVLC/ref-1 pair, from a different source,
with no I_PCM or 8x8 transform. SPS/PPS + IDR + 31 P frames are retained; each
off slice has filter-idc 1. The cap remains 23,921 bytes and 2,048 bytes per NAL:
both new segments fit without modifying encoder settings.

| Clip | Bytes | CRC32 | SHA256 |
|---|---:|---|---|
| tokyo15-deblock-on | 19,038 | 5026f28e | 88af67490140ac9355371f9dcb008339eafc958943feab35f97ca1ff655b6aa0 |
| tokyo15-deblock-off | 19,672 | 91283249 | 52d8a8f474d9138480f37b58bb0c4b8b86a56fb5b0f010851aa2e11fac9315fb |

Build capsules without rebuilding firmware, in Ubuntu:

```sh
python3 patches/decoder_speed/build_capsule.py --kind mixed --output obj/decoder-speed/mixed
python3 patches/decoder_speed/build_capsule.py --kind full --output obj/decoder-speed/full
python3 patches/decoder_speed/build_capsule.py --kind profile --output obj/decoder-speed/profile
python3 patches/decoder_speed/build_capsule.py --kind profile-control --output obj/decoder-speed/profile-control
python3 -m unittest discover -s tests/decoder_speed -p test_link.py -v
python3 -m unittest discover -s tests/decoder_speed -p test_partition.py -v
python3 -m unittest discover -s tests/decoder_speed -p test_profile.py -v
```

`--promotions <symbols.json>` adds exact pre-optimization symbols to mixed's
existing hot set. Unknown symbols fail; constants/definitions retain one owner,
with no LTO or duplicated decode path. Promoted capsules are named
`mixedplus-<actual decimal KB to three places>`. Manifests record bytes, exports,
SHA256, flags, compiler, source identity and membership. Generated files remain
ignored. `DS_CAPSULE_ROOT` optionally locates these directories for link tests.

Before upload, each capsule must have independent LLD byte comparisons and
sanitized PC equivalents matching every frame of all accepted clips. Store a
matching `checks.json` beside `capsule.json` and `capsule.bin`: `result="PASS"`,
the exact `sha256`, `lld=true` and `pc_y=true`. The tool requires this evidence
before connecting; it is a local provenance record, not a signature. Every
physical run must still match its PC Y hashes.

```powershell
venv\Scripts\python.exe -m unittest discover -s tests/decoder_speed -p 'test*tool.py' -v
venv\Scripts\python.exe decoder_speed_test.py batch -c 'g2://local?left=<left-address>&right=<right-address>&addressType=public' --lens left --capsule <mixed/capsule.json> <profile-control/capsule.json> <profile/capsule.json> --clip <Tokyo-off.h264> <second-off.h264> --reference <Tokyo-off-reference.json> <second-off-reference.json> --output-dir <new-private-profile-directory>
```

A C batch forces skip mode, keeps one paired Windows connection, and closes
each sealed owner before the next capsule/clip. Color requests and existing
evidence directories are rejected. Raw results precede validation; profile
rows and validity flags accompany the summary. Capsule bytes/hash/nonce/kind
are checked against the manifest. Use ordinary mixed and full plus at least
five promoted intermediates for the complete left-lens, deblocking-off curve
on both sources. Then measure the 2-3 knee sizes on both lenses, OFF/ON, both
sources. Measure full RAM on original Tokyo once per lens to compare with A.
The hardware profile chooses promotions; no candidate speed is inferred from
PC timings, instrumentation or the boot check. The user chooses the final size.
