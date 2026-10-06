# Decoder measurement, firmware A

This experiment uses jim's `306e42e` (Even 2.2.9.22, `Faceclaw/16`). It retains
that identity solely for this local measurement and does not implement the video
port. Sub0h264 is unchanged from the G2 decoder commit `59c66b1`. One complete
`-O2` C++23 capsule supplies `full-mram` and `full-ram`; the latter copies the
same code and constants into executable heap 27. Small and mixed setups are
future work.

Nothing allocates experiment state or starts a worker before an explicit mode-31
command. Default allocated decoder tables, clip, RBSP, worker stack and frame
planes use cached heap 20. Read-only constants remain with the capsule.
`--uncached 1` moves only the frame planes and allocated tables into heap 13;
this option is restricted to `full-mram`. No allocator cache is evicted.
Before and after reservations, live free/largest-block checks retain 32 KiB on
heaps 13/20 and 16 KiB on heap 27. Allocation failures roll back owned blocks.

The accepted clip is a frozen 32-frame 320x192 Tokyo segment, 23,921 bytes,
SHA-256 `c80a8087c83bed781374b3475987d9e772b517177ba244b250704046b354da0b`.
It contains an IDR followed by 31 P frames, one reference and NALs <=1,466 bytes.
Clips, hashes, generated capsule bytes and measurement results stay untracked.
No 640x384 clip is accepted by firmware A: its optional PC probe failed the
color/skip Y-equivalence gate. Decoder fixes belong in a separate change.

## Commands

From the repository, prepare the private sample/reference without connecting:

```powershell
venv\Scripts\python.exe decoder_speed_test.py prepare --source <Tokyo.h264> --clip <private-clip.h264> --reference <private-reference.json>
```

For each lens, measure both setups with `--skip-chroma 0` and `1`:

```powershell
venv\Scripts\python.exe decoder_speed_test.py run -c 'g2://local?left=<left-address>&right=<right-address>&addressType=public' --lens left --setup full-mram --skip-chroma 0 --clip <private-clip.h264> --reference <private-reference.json> --output <private-result.json>
```

Change `--setup` to `full-ram` for the executable copy, or add `--uncached 1`
to `full-mram` to measure uncached frames/tables. The tool authenticates the
connection, uploads and measures; it never flashes. Keep the phone disconnected
during measurement. On Windows, pair both lenses first: an unpaired connection
can be cut after about 35 seconds despite protocol authentication. Supply both
addresses and `addressType=public` in the connection URL; put the URL in single
quotes in PowerShell so its `&` separators are passed literally. The measurement
tool scans for 60 seconds. A lens can take 1–2 minutes to advertise again after
a session; allow it to recover before retrying. Each step prints flushed,
timestamped progress, and exceptions include their type even with no message.
Flash only a separately reviewed boot-PASS copy, using
`g2flash.py`; first use its connection-only `--stop-before file_check` gate.
Return afterwards using Faceclaw's Install firmware button, or a reviewed copy
of the unmodified firmware matching the installed app version.

## Protocol and timing

The unchanged private SID-f0 message transport carries `31,'D','S',2,op,tokenLE16`
followed by operation arguments. A fresh ordinary transport stream per command
retains jim's compression, sequencing, CRC and ACK behavior. Operations are:

| op | Arguments | Purpose |
|---|---|---|
| HELLO (0) | none | experiment magic `0x44530203`, setup bits 0/1 |
| BEGIN (1) | bytesLE32, CRC32LE32, widthLE16, heightLE16 | reserve upload session |
| WRITE (2) | offsetLE32, 1..2048 clip bytes | sequential/idempotent upload |
| SEAL (3) | none | check exact length and CRC32 |
| RUN (4) | setup, skipChroma, repeats=5, uncached | reserve and start after ACK |
| READ (5) | indexLE16 | read completed result; `0xffff` reads phase |
| ABORT (6) | none | request cooperative stop |
| CLOSE (7) | none | terminate parked worker, then release session |
| HEAPS (8) | indexLE16=0..5 | live free/max for heaps 20/13/27, after CLOSE |

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
wsl -d Ubuntu -- clang -O1 -g -fsanitize=address,undefined -Itests/decoder_speed tests/decoder_speed/controller_test.c -o obj/ds_controller_test
wsl -d Ubuntu -- obj/ds_controller_test
wsl -d Ubuntu -- clang -O1 -g -fsanitize=address,undefined tests/decoder_speed/pool_test.c -o obj/ds_pool_test
wsl -d Ubuntu -- obj/ds_pool_test
```

`test_link.py` compares final bytes with independent LLD at MRAM/RAM addresses
and rejects writable state, undefined targets and absolute pointers. Build with
`bash build_cfw.sh --skip-venv --update-patches`, review the generated manifest,
pin the resulting hash and reproduce it before the independent boot gate.
All real execution timings and cache/RAM behavior remain to be measured on the
glasses; offline tests and a healthy boot do not establish decoder throughput.
