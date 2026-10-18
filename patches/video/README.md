# Cached decoder ownership

## Queued Playback

SHOWN operation 15 queries an owned stream/token and telemetry cursor in 20
bytes. Its frozen 72-byte reply is version/op/result/lens, followed by LE32
stream, token, telemetry serial, frame ordinal, media PTS, copy tick, trigger
tick, queued tick, flags, first available serial, high serial, overwritten
count, holds, reply tick and uncertainty, then claim and submit ticks. The
reply tick (offset 56, formerly an always-zero wire compensation) is the lens's
tick when the reply was built, so the host can age the copy without relying on
a mapping fixed at START.
Advanced capability byte 7 bit 2 selects extended captures; old 64-byte replies
remain supported by the PC client. Capability bit 3 advertises
this extension. Cursor 0 selects the oldest available row; an overwritten
cursor recovers there with flag 4. Flag 1 denotes a paired trigger and flag 2
uncalibrated wire timing; UINT32_MAX uncertainty is explicitly unknown.
The 32-row ring never waits for host acknowledgements. Timestamps come from
actual software display-copy capture, not packet arrival at the host or panel
light emission. Compare local trigger-to-copy deltas, not absolute lens clocks.

Advanced capability byte 7 bit 2 also advertises TRIGGER_TIMING 22. Its 20-byte
request carries stream and both owner tokens, like PLAYBACK. The frozen 96-byte
reply has version/op/result/lens plus 23 LE32 fields: stream, token, mediaN,
PTS,
roundtrip, LEFT receive-to-DONE elapsed, LEFT receive-to-copy, RIGHT trigger-to-copy,
nominal transport upper bound, signed lower/upper offset, valid, sample/anomaly
counts, min/max transport bound, holds, occupancy, stack used, guards OK,
allocated
bytes, peak and cap. Timing calculation and stack sampling run on presenter;
the command handler only freezes an owner-bound copy.

LEFT DONE carries local elapsed at wire 16 and local copy duration at 20/21;
22 remains generation mode. RIGHT uses its own trigger-to-receive roundtrip,
never unrelated tick origins. Duplicates do not replace first trigger capture.
Retries/inconsistent timing remain raw invalid observations without stopping
decoding. Bounds include four nominal milliseconds for endpoint rounding;
clock drift and optical onset are NOT qualified. Min/max show loaded jitter,
not a physical one-way latency guarantee. No half-roundtrip symmetry claim,
shared clock exchange or assumed nonzero compensation is introduced.

The optional control 13 query returns 16 bytes: version, operation, result,
feature mask (bit 0 owned queue, bit 1 paired triggers, bit 2 compact metadata),
depth 2, supported-rate mask 7 (bits 0/1/2 mean 10/15/20 fps), two reserved bytes,
then LE32 A4 charged allowance 77824 and presenter stack 8192. START option 4
requires a configured TIMELINE. BOTH-target queued START enables paired wire
triggers; option 8 explicitly pairs independently delivered host streams.
Neither legacy capability nor default START implies synchronized playback.

Compact FRAME_TIME operation 14 retains the 24-byte request and returns 6 bytes:
LE32 accepted picture ordinal, remaining metadata credits, result. Full mail
returns BUSY without accepting a new row. The PC queues metadata for reference
identified terminating NALs, never assuming every NAL produces a picture.

RIGHT coordinates at the clip rate, regardless of compressed-payload ingress. Wire-only
kind 5 records carry source/version/operation, stream, picture ordinal, PTS,
sender-local tick, ready depth and terminal flag in 24 bytes. READY gates the
matching heads; a common TRIGGER consumes an already-decoded frame. RIGHT
waits for both actual copy completions before advancing. LEFT never runs its
own cadence. Repeated READY/TRIGGER/DONE records are idempotent; a lost DONE
is recovered by a repeated trigger, not another presentation. Unknown
completion cancels within the display limit and preserves ordinary quarantine.
No local tick values are compared across lenses; wire compensation defaults
to zero pending loaded hardware measurements, not a claimed optical offset.
Bridge callback only posts into a stable bounded mailbox; controller forwards
wakes, and presenter processes records. Legacy input and stock hooks unchanged.

The lazy presenter owns two 30720-byte A4 snapshots and a guarded 8 KiB static
task stack, with its own reserve-aware allocation ledger. Metadata names the
completed picture and terminating NAL independently. The decoder cannot reuse
its borrowed Y until copying is complete; snapshots stay pinned through actual
display-copy completion. Prebuffer two pictures, except the timed terminal
picture. Absolute clip-rate deadlines retain the rational millisecond phase;
starvation
holds/rebuffers rather than presenting overdue pictures in a burst. Controller
completion work posts wakeups; pacing runs on the presenter task. Unknown
completion, task ABI, stack guard or teardown failures retain quarantine.

The internal worker API is inert until a caller explicitly starts it. No boot
path creates a video allocation, task or timer. Start requires a live framebuffer
lease and an absent texture cache. Release jim's previous lease normally before
acquiring the video lease; this owner never frees another client's resources.

Call start, stop and reset outside the image mutex and display gate. Handlers
already holding the image mutex may only request cancellation there, then leave
that scope before reaping. The presenter keeps image-before-display lock
order and pins borrowed output with the generation token. No decoder call, heap
operation, task termination or worker wait occurs while those locks are held.

Heap 20 owns the decoder's actual size/alignment, all requested tables/planes and
scratch, the allocation ledger, static task/event control blocks and 16 KiB stack.
Requests determine sizes, including untagged STL growth; tags are diagnostic.
The decoder-only cap is 206,904 bytes; a transport-controlled start raises it
to 223,288 bytes, including four lazy 4 KiB input slots and their actual ledger
charges. Outstanding slot allowances decrease as their bytes are allocated.
Retain 32 KiB in cached heap 20, 32 KiB in display heap 13 and 16 KiB in heap 27.
Live free/max views do not reserve memory: each allocation and its resulting
reserve is checked. Heap 13 admission includes the missing 153,600-byte shadow
and caller-supplied outstanding ingress/inflater peak. Existing buffers are not
charged twice. Heap 13 supplies only jim's shared shadow; heap 27 has no video
allocation or fallback.

Only the worker constructs and destroys C++. Its complete runtime table is
published before arming and remains bound through cancellation and destruction.
A normal stop drains output/callback pins, destroys the fully initialized
decoder and parks. Fatal callbacks never unwind or free partial objects: they
cancel and park. The controller terminates the parked, different task before
clearing the provider and reclaiming the raw ledger, stack and owner. Failed
termination, timeout, stack guard failure or an unknown RTOS ABI quarantines
ownership and refuses another start. Deferred completion tokens live in the
stable context, so publication-lock contention cannot lose private cleanup.

platform.h uses the authenticated Even 2.2.9.22 CMSIS services. The donor's
osThreadNew accepts a 112-byte caller-owned TCB and supplied stack; its static
flag is 2 at byte 109. osThreadTerminate synchronously reclaims a different task;
prvDeleteTCB preserves both supplied buffers with that flag. Check the flag after
creation and never terminate the current worker from itself. Priority 8 and
event waits leave services, interrupts and the watchdog running normally.
The firmware's exact-base hash guard also authenticates these service bytes.

Two caller-owned 32-byte event groups are created during explicit start, before
the private claim and worker. The private owner and completion handle stay
reachable during preparation, so cancellation does not poll startup. Sticky
wake bits cover arm, input, cancellation, pin drainage and lease renewal; state
and generation are checked after every wake. Idle waits end at the current lease
deadline, and stop waits on completion with a bounded deadline outside locks.
Signal through the locked wake/unpin APIs: releasing a pin without its drain
wake can strand teardown until its deadline.

Only task-context setters are permitted. ISR event setters defer pointers to
the timer service and could outlive ownership. The donor's static event creation
initializes a local wait list without a global registration; its CMSIS block has
no event-delete entry. After all callers drain and the worker is terminated,
reclaim only groups whose wait lists are empty and static flag is intact.
Completion waiters are serialized by the existing controller claim. Normal
senders hold the image mutex until SET returns; the parked worker is terminated
before either group is reclaimed. Failure retains the owner in quarantine.

After termination, video_worker_get_report exposes requested object bytes,
cached storage peak, heap free/max snapshots, both stack guards and downward
stack high-water from the 0xa5 sentinel. Heap snapshots precede final raw reclaim;
storage_left becomes zero only after reclaim completes. Invalid diagnostics in
quarantine are never sampled from a running task. These fields persist until a
new start. Native tests validate sentinel accounting using a simulated stack;
actual ARM high-water requires a later explicit hardware worker run.

The firmware build emits .su files for the C unit, runtime and decoder modules
alongside its objects. Call-graph estimates must include indirect runtime/heap
callbacks, bounded library recursion, backend helpers and stock RTOS/allocator
stack. The static estimate complements guards and runtime high-water; it does
not replace them. Callback tracing remains compiled with an empty callback:
this C API provides no trace setter.

Run the sanitized worker, storage and lifecycle checks with the existing suite:

    python3 -m unittest discover -s tests/h264 -v

## Shared-shadow packing

### Bounded elapsed profiling

Playback capability byte 7 bit 4 advertises DIAGNOSTICS kind 3. Its 12-byte
request appends kind, page (0 or 1) and two zero bytes to the control header.
Each frozen 128-byte page has kind at byte 3, page/schema/lens at bytes 4-6,
zero at byte 7, generation/stream/token/dropped/flags at offsets 8-24 and
zero at offset 28. Four 24-byte stage rows begin at offset 32: count, bytes,
elapsed ticks, cycle sum low/high, maximum ticks, all LE32.

Stage order is decode calls, queue-space wait, decoded-picture copy, panel
clear/pack, display submit-to-callback, bridge envelope/copy/enqueue, sync
enqueue and custom ingress processing. Display bytes counts successful
callbacks; the other byte fields count input bytes, not radio throughput.
Bridge ingress brackets reconstruction; host ingress also includes bridge
enqueue. Nested brackets are not additive CPU costs and include preemption.
No task-runtime accounting, frequency calibration or optical timing claim
is made. Brackets over 1,000 ticks retain ticks but omit ambiguous cycles.

Eight stages each have four fixed raw mailboxes. Producers only capture
and post, never log, allocate, wait or aggregate. The presenter starts and
folds counters under existing publication protection. Mailboxes survive
generation transitions so old writers cannot corrupt a reset queue; old
generation records are discarded. Startup before presenter activation is
not sampled. Saturated sums, dropped samples and long spans have flags
4, 1 and 2 respectively; dropped count wrap also flags saturation. These
are bounded samples, not guaranteed exhaustive transport accounting.
Snapshots are read-only, owner-origin checked, generation tagged and kept
after STOP until the next presenter starts. The private runner checks
coverage/loss and retains raw pages rather than inventing exclusive CPU
percentages. No new stock hooks or scheduling/transport settings change.

STATUS reports the last completed media count and its matching completion
tick. An in-flight copy or menu refresh cannot clear or advance only half
of that tuple. The display callback captures a pending tick; the existing
publication fold commits both fields together. A genuine zero tick at
counter wrap remains valid. SHOWN keeps the per-copy capture timestamps.

The bounded packer converts top-down Y8 to the panel's A4 layout, with the
left pixel in the high nibble. The default development curve rescales luma
across visible panel levels 1 through 15. The selectable decoded-luma curve
keeps max(1, Y >> 4) without range normalization. Picture margins stay off.
Native 320x192 is centered at (160,144); doubling produces 640x384 at (0,48),
with identical nearest-neighbor pixels and black 48-row top/bottom margins.
Every preparation clears only margins before completely overwriting the
picture rectangle. Mode changes and old menu pixels cannot survive outside
it. Width alignment makes both side margins byte-aligned; odd-height top
and bottom centering are preserved. Input padding is never picture content.

The descriptor carries ownership generation, picture ordinal, geometry,
stride, storage extent and scale. It also accepts owned A4 for a later queued
presenter; borrowed decoder planes must be consumed before the next decode.
No second panel buffer or decoded-frame queue is added. Packing itself is
independent of the asynchronous display services.
Stock side 2 maps to left source 1, and side 1 to right source 2. Both use the
same pixel order; no optical mirror is inferred from those different values.

## Private transport controls

ID 31 uses protocol version 1. The control header is ID, opcode, version,
zero flags and a nonzero LE32 request ID. Named opcodes are capabilities (0),
start (1), stop (2), reset (3), status (4), page (5) and NAL ingress (6).
Capabilities/status have only the eight-byte
header. Stop/reset append the current LE32 stream ID. Reset ends ownership;
a fresh start uses a strictly increasing stream ID after reclamation.

Start is 24 bytes: header, LE32 stream ID, LE16 width/height, reference count,
DPB frame count, presentation flags, zero reserved byte and LE32 interval
in milliseconds. Only 320x192, one reference, two DPB frames and intervals
10..1000 ms are accepted. Flag bit 0 selects native centering; zero selects
pixel doubling. Flag bit 7 requests frame verification; other bits are reserved.
Chroma remains skipped. Release
the previous custom
lease normally, reacquire the framebuffer lease, then start; an allocated
texture cache or conflicting shadow/cache command refuses admission.

Start replies accepted while preparation is pending, rather than claiming
readiness. The existing stock task pool handles allocation and bounded teardown
outside receive/image/display locks. Its copied 20-byte job carries no owner or
transport pointer. Authenticate its 150-item queue identity and use only a
zero-timeout Put; the stock dispatch wrapper instead waits on a mutex and must
not be used. One callback owns controller waits; cancellation invalidates the
start generation before claim and publication. Park notifications use stable
context only. Queue failure or unconfirmed reclamation retains quarantine.
No new timer, permanent task or boot-time video allocation is introduced.

Four exact request/reply entries per ingress lens prevent repeated operations.
Request IDs increase separately per ingress; older uncached IDs or conflicting
duplicates refuse. Each reply freezes its bounded snapshot. A page query
has the original request ID and one page byte, and never executes the control.
One response per query fits the proven notification capacity, including MTU23.
The frozen page width uses the largest proven ingress packet capacity in
its BLE connection epoch, capped at 200 bytes including the reply header.
Ordinary ACK sizes and history remain unchanged. PAGE accepts at least nine
bytes, ignoring trailing padding. Pad PAGE to 195 bytes so its five-byte
record header proves room for a 200-byte reply; compressed senders need fresh
incompressible padding. Record and compression resets retain that proof;
a changed BLE epoch clears it before parsing any new connection's bytes.
A short first command can still freeze a narrow reply. Retrying a control
or proving a larger budget later does not change its original page width.
MTU23 retains its small inferred budget. Replies use two fixed 200-byte buffers
in the CFW context, one per origin, instead of temporary heap or stack storage.
An atomic per-origin claim protects unlocked diagnostic sampling through the
copied send; image_mutex protects replay construction. Same-origin overlap
refuses instead of overwriting the scratch. Request decoding and copied stock
send queues retain their existing allocation and reserve checks.
Replies are ID31, source lens bit, LE32 request ID, page/count and snapshot bytes.
The distinct bridge video-return envelope leaves upstream ACK/NACK validation
unchanged. Send services copy each bounded reply before return. Private bridge
requests carry the ingress connection's LE32 epoch after kind/origin; returns
keep their original header. Both lenses must run this transport version. This
does not change the UART configuration. The DM adapter advances the epoch on
host rebinding/disconnect; receivers discard old reconstruction, inflater and
budget before accepting bytes tagged with a different epoch. No proof is
retained when the adapter cannot establish an epoch.

Snapshot bytes 0..3 are version, supported stage, control result and lifecycle
state; LE32 values at 4/8/12/16/20/24/28 are stream, generation, left/right request
high-water, stream high-water, last error and interval. Record/NAL maxima are
LE32 at 32/36; geometry at 40/42, references/DPB at 44/45, forced chroma skip and
legacy single-slice byte at 46/47 (always 1; playback capability byte 7 bit 3
advertises multi-slice pictures), four/six-slot bounds at 48/49. Capacity/free slots are bytes 50/51; LE32
accepted/consumed NAL counts are at 56/60. Expected sequence is LE32 at 52. Completed pictures are LE32 at 64;
bytes 68..79 carry recovery and diagnostic details. Stage 7 announces ordered
decoding, copy-completed presentation and optional verification; byte 71 advertises allowed START
presentation flags. LE32 at 80/84/88/92 holds copied picture count, failed-copy
count, last copy tick and selected flags. Copy completion means physical
framebuffer copy plus cache flush, not proof of optical scanout.
Transport ACK confirms only
accepted command validation; refusals preserve upstream NACK/context-reset rules.

### Compact receive credits

Capability byte 70 bit 3 advertises CREDITS (opcode 10). Its 20-byte request
contains the usual header followed by LE32 stream, left generation token and
right generation token. Each lens validates its own token and the ingress
owner; single-lens requests zero the other token. Exact retries freeze the
original snapshot and never renew activity again. A fresh valid query renews
the existing owner deadline without allocating storage.

The six-byte snapshot contains LE32 next-to-consume NAL sequence, one packed
byte and the control result/error byte. Packed bits 0..2 are free credits,
bit 3 selects capacity six rather than four, bits 4..6 are lifecycle state
and bit 7 is a pending gap. Idle/starting/refused replies grant no credits.
Together with the eight-byte reply header it fits one 14-byte page on a large
MTU. MTU23 uses the existing bounded page fallback. The layout is specific to
CREDITS, rather than the version/stage prefix of the full STATUS snapshot.

Both-target packets are sent once and forwarded through the existing bridge.
Each lens returns its own source-tagged ACK and credit snapshot; a local ACK
does not establish peer acceptance. Senders reserve one slot on each target,
fence both sets of record ACKs before a fresh query, and use the smaller free
count plus both next-to-consume sequence windows. Consumption only frees slots
while that single producer is paused. One lens's optional slot extension does
not enlarge the other's capacity. No NAL count is treated as a picture count.

Transport allocation checks preserve the heap-13 reserve and missing-shadow
allowance before and after stock-coordinated allocation. Cached decoder sizes
come from actual requests, including future compile-time feature changes.

NAL ingress has a separate ten-byte header: ID31, opcode6, LE32 stream and
LE32 NAL sequence, followed by exactly one raw NAL (no Annex-B start code).
The record limit is 4,096 bytes and raw NAL limit 4,086 bytes. START selects
the version; data does not carry a second version byte. The owner rejects
wrong streams/origins, invalid headers, oversize records and cancelled input.
Its four cached snapshots include any consumer-active slot. Borrowed transport
bytes never escape dispatch. The worker claims/releases a generation-checked
owned view, returning one credit after successful CONSUMED or FRAME_READY.
ERROR retires the stream without successful consumption. C++ runs outside
publication locks; cancellation drains its pin before park and reclamation.
A NAL, transport record and completed picture are distinct.

Sequence zero starts a new stream. At capacity four, at most three future NALs
fit ahead of the expected one; sequence distance is strictly below capacity.
Only the expected sequence can be claimed. Matching queued/recently released
bytes are idempotent; conflicting duplicates refuse. Released slot bytes are
the bounded exact replay history until reuse, after which older retries refuse.
The highest sequence is reserved to prevent wrap; start a new stream instead.

A worker-only completed-picture notification may attempt one 8 KiB extension
when the decoder reports its two-frame DPB fully allocated. Allocation happens
outside image/display locks, with fresh live heap/reserve checks. The two slots
publish together; failure restores the prior cap and keeps four slots. The
trigger is actual allocated DPB planes after FRAME_READY, never NAL count.
Native tests exercise the real consumer, Y output, extension and refusal.

### Gap recovery

Future NALs start a five-second missing-input deadline. Later inputs, retries
and status queries cannot extend it; input is checked before copying so a late
missing NAL cannot repair an expired stream. Thirty seconds without accepted
NAL input or a fresh owner-origin status/capability request retires a session;
cached control replay does not renew it. Event waits use the nearest activity,
gap or existing framebuffer-lease deadline, including millisecond tick wrap.
No timer or polling loop is added.

Conflicting duplicates, sequence exhaustion, invalid header progression and
expired gaps stop acceptance and require fresh START with a larger stream ID.
The controller reclaims only after cancellation, pin drainage and worker park;
allocation, joins and frees stay outside image/display locks. A failed reaper
keeps quarantine. Header receipt requires SPS, PPS and IDR before non-IDR
slices; it does not validate their contents or claim decoded pictures.

Byte 68 is recovery state (0 clear,
1 waiting, 2 needs a fresh stream), byte 69 records SPS/PPS/IDR header receipt
bits, offset 72 is the gap deadline and offset 76 the first missing receipt
sequence. Expected consumption at offset 52 and completed pictures at 64 remain
separate. All pending queue bytes belong to the retired owner, never the next
stream. The C interface limits the parsed progressive I/P format before its
first input: the selected coded size, one reference and two DPB frames, no crop, FMO,
weighted prediction; only CAVLC pictures without 8x8 transforms may span several
slices, in raster order. Baseline CAVLC, Main CABAC and High
CABAC with 8x8 transforms are supported; High CAVLC with 8x8 is rejected.
I_PCM remains rejected by the decoder. Queries return actual lazy DPB storage,
independent of which reconstruction features are compiled into the decoder.

START retains its legacy 24-byte full-320x192 form. Capability operation 13
byte 4 bit 7 advertises the 32-byte form; bits 0..5 retain decoded queue depth.
Bytes 12..15 select exactly 320x192, 320x176 or 144x240. Extended bytes 24..31
contain little-endian 16-bit picture x, y, width and height inside that coded
frame. All four are replay-frozen and bounds-checked before admission. Odd
picture edges are allowed: 16:9 uses 313x176 at x=3 in 320x176, leaving three
and four unlit coded columns. Neither rotation nor pixel-aspect correction
is performed by the glasses. The phone provides the final upright rectangle.

The coded frame is centered at Native or 2x. Packing leaves every pixel outside
the picture rectangle at level zero, including partial packed-byte edges;
picture content keeps the existing level-one black floor. Maximum decoder and
two-slot A4/Y8 buffer sizes and heap reserves do not grow. The exact selected
size is enforced by the decoder before input; it is not a crop or rescale.
New clients require the capability for nonlegacy geometry. Old phone clients
reject the unfamiliar capability depth byte instead of misplacing a picture.

Capability byte 4 bit 6 additionally advertises a 36-byte picture START. It
includes the same geometry plus byte 32 selecting dither floor 1/0/0.25/0.5
as values 0/1/2/3; bytes 33..35 must be zero. Nonzero style requires queued
dithering, and legacy START forms reset to default style zero. The entire
command, including the floor, is replay-frozen. It changes only at START/seek.

Dithering remains an On/Off choice with quarter-step spatial Bayer cells.
Floor one retains exactly the previous curve. Other floors blend into that
curve below normalized Y7; every brighter target is unchanged. Half/quarter
floors average two/one lit level-one pixels per black 2x2 cell; floor zero
leaves black unlit. The fast doubled path computes one cell per source pixel,
with no additional large lookup table. Unlit margins never enter this curve.
Plain conversion is unchanged when dithering is off; fractional floor settings
are retained by the phone but apply only when dithering is enabled.

### Shared display lifetime

Prepare the single 153,600-byte shadow outside publication/display locks with
fresh heap-13 free/max and reserve checks. Revalidate the lease and generation
before publishing it, and free an unused private candidate outside locks.
Replace the missing-shadow allowance with that actual allocation; subsequent
frames reuse it. The worker packs borrowed Y before any further decoder call.

Acquire image_mutex before the stock display semaphore. The donor's void gate
wrapper discards its timeout, so the presenter checks the authenticated
underlying take result. Packing and tagging precede the existing refresh enqueue.
The existing display hook claims the stable tag, copies and flushes the shared
shadow, then posts completion by atomics and zero-timeout controller enqueue.
It never waits, allocates, frees or borrows a worker/event pointer. Only video
jobs suppress the diagnostics overlay; other image behavior stays the same.

The controller folds actual completion and retires the lifetime pin before
waking the worker. STOP defers reaping while a submitted copy needs that same
controller, so it cannot block the callback needed to finish. Queue refusal
releases an unclaimed job; if copying already started it waits for completion
and still reports failure. Missing completion retains the pointer, semaphore
and owner in quarantine instead of guessing that a free or gate release is safe.
The display wait is bounded and leaves normal interrupts/watchdog running.

### Optional frame verification

START flag bit 7 requests verification; it defaults off. Bit 0 independently
selects native presentation. Other bits remain reserved. Off playback allocates
no result window, performs no CRC or clock calibration and never waits for
result ACKs.
Diagnostics remain available. Verification adds at most 1,536 bytes to the
cached ledger cap, with a single lazy allocation of sixteen 64-byte rows and
bookkeeping. It does not change the decoder, panel packing or display path.

Before claiming another NAL, a full result window waits on existing events and
lease/activity deadlines. No result is overwritten. Explicit READ (opcode 8)
and cumulative ACK (9) use the usual header followed by LE32 stream, owner
token and picture ordinal, twenty bytes total. READ returns a frozen 80-byte
response; ACK returns sixteen bytes. Both begin with version, opcode, result,
zero flags, then stream, token and ordinal. Source/owner/token checks reject
stale sessions. An ACK past the published cursor refuses; older ACKs cannot
retire newer results or renew activity. Exact request retries and PAGE queries
use the existing frozen replay cache. STOP with unread rows marks the run
incomplete before safe reclamation, including a final copy racing with STOP.

The READ payload at offset 16 has sixteen LE32 values: ordinal, first and last
contributing NAL sequence, width/height packed as low/high halves, Y CRC32,
decode cycles, millisecond ticks, calibrated clock before/after in Hz, timing
flags, decode-return tick, copy/flush tick, call count, finishing-call cycles
and ticks, and no-output-call cycles. CRC covers active Y bytes without stride
padding, using reflected polynomial 0xedb88320 and initial/final xor 0xffffffff.
A row is published only after actual physical copy and cache flush.

DWT CYCCNT and the donor millisecond tick bracket only C ABI calls. Unsigned
deltas support counter wrap. Calibration samples at least ten ticks outside
those brackets, with at most twenty one-millisecond waits; failed calibration
or overflowing timing accumulation marks the row invalid without aborting.
CRC, calibration, packing, display and transport are outside decode timing.
The PC additionally flags clock changes over four percent and cycle/tick
disagreement, reports both timing scales, excludes anomalous timing rows and
counts exclusions while still checking every hash. Real decode, format,
memory, deadline, cancellation, count and hash failures invalidate a run.

STATUS LE32 offsets 96/100/104/108 hold verification capacity, published cursor,
acknowledged cursor and full-window stall episodes. Off mode reports zero.
Offsets 112/116/120 report cumulative no-output-call cycles/ticks/count,
including parameter sets and any tail after the last picture. Offset 124 marks
incomplete verification. Presentation count at 80 remains independent of NAL
consumption and the result acknowledgement cursor.

### Ordinary decode timing

Capability byte 70 bit 4 advertises decode totals. Bits 5..7 count 64-byte
STATUS extension units beyond the original 128 bytes: one unit, 192 bytes
total. Capabilities, START/STOP/RESET and diagnostics retain their 128-byte
lengths. CREDITS stays six bytes. Clients reject unsupported lengths/schema.

Offset 128 starts a 64-byte extension: version byte 1, diagnostic flags byte,
LE16 non-VCL call count, LE32 completed-picture count, LE64 cycle/tick sums,
LE32 maximum picture cycles/ticks, six LE32 bucket counts, and LE32 non-VCL
cycle/tick sums. Bucket upper bounds are 30, 50, 62, 80, 100 and unbounded
milliseconds. Parameter-set/SEI work is separate; VCL calls accumulate until
FRAME_READY. NAL consumption, completed pictures and copies remain independent.

The worker enables DWT once per session and reads cycles/ticks immediately
around each C ABI decode call. Optional verification shares those samples.
Normal mode adds no calibration, CRC, allocation, result window or ACK wait.
Unsigned deltas cover a single counter wrap. Brackets of at least one second
set flag bit 0 because their cycle delta is not qualified. Saturating counters
set bit 1; flags never abort decoding. Statistics with flags remain diagnostic.

Totals publish coherently under the existing short publication lock and freeze
with each STATUS request. Reads/retries do not reset them. The last totals
survive STOP; an accepted fresh START resets them. Stable replay storage grows
by 512 bytes and retained totals by 64 bytes, plus the worker's lazy accumulator.
Tick totals are wall time with millisecond granularity and include preemption.
Cycles also include preempting tasks/interrupts. Bins do not establish a median,
P90 or an exclusive decoder CPU share; hardware scheduling needs separate
evidence.

### Paired display power

START first acquires a finite display lease and requests the stock dashboard
startup. Decoder arming requires the local UI, local driver and peer's matching
acknowledgement to be ready. A LEFT-only session also wakes the RIGHT coordinator;
a peer-only lease allocates no decoder, frame or framebuffer ownership.

Power messages use bridge kind 4/version 1, independently from NAL transport
credits. Their fixed 26 bytes contain sender, operation, result, generation,
stream, request serial, remaining lease milliseconds and a session nonce.
One copied mailbox bounds receive work. Exact duplicates do not extend a lease;
released or expired owners cannot be revived by renewal. Separate local/peer
leases keep simultaneous independent sessions from releasing each other.

The lease lasts at most 15 seconds and renews every two seconds while the
session's input and framebuffer leases remain valid. START waits at most two
seconds, with bounded retries. Missing peer readiness refuses START; lost bridge,
manual sleep or lease expiry cancels playback. Power errors report code 16.

The local gate reads UI state at `0x20077298`, driver initialization at
`0x20076784` and active dashboard app 1 at `0x20076770`. The pending-app word
at `0x20076778` can be zero during normal dashboard refresh and is not a
readiness signal.

STATUS byte 69 keeps header progress in bits 0..2 and the last acquisition
barrier in bits 3..7. Codes 0..10 name: none, local startup refused, local UI not
awake, local driver not initialized, local active app is not dashboard, bridge
send refused, peer acknowledgement missing, peer not ready, peer refused,
owner cancelled or expired, and peer acknowledgement expired. The controller
retains the last barrier after failed-start rollback; a new acquisition clears
it and success leaves zero. A peer-not-ready result does not identify which
stock gate blocked that peer. The sender reports each targeted lens's named
barrier when READY fails. Snapshot length, pagination and power bridge records
are unchanged; use the updated STATUS parser to read a nonzero barrier.

The authenticated stock pointer queues retain the original paired application
lifecycle. A task-context adapter allocates bounded cached packets once, checks
the live reserve, and submits with timeout zero. Refusal rolls back without
CommonMalloc's retry loop or the public startup wrapper's asserting failure.
After enqueue only the stock consumer owns the packet. No queue wait, allocation
or free runs in the receive or UI maintenance callback.

Only the dashboard's automatic idle decrement is held by an unexpired lease.
Explicit shutdown, wear/OTA/fault paths and other applications remain unchanged.
Foreground/background refresh dispatches still run once, with their original
arguments and return value; the wrapper only posts bounded controller work.
There is no new timer, boot allocation or permanent worker. Worker waits include
the power deadline. STOP retains ownership until pending copies and the worker
park, then releases that generation; an unknown copy remains quarantined.

### Upstream session cleanup

STOP, RESET and session cancellation park the decoder before restoring the
current stock UI through a copied type-3 display refresh. Its generation tag
cannot be claimed by an unrelated or periodic refresh. Clear the full physical
frame first, then let the stock compositor fill its viewport; this also removes
the video's margins without a second saved framebuffer. The refresh does not
wake a sleeping driver or restart an application.

Every full-panel direct copy invalidates older video ownership. Hand-back
preserves newer custom output, framebuffer leases and an independent peer owner.
New video and custom shadow writes remain blocked through restoration; STATUS
reports STOPPING until the bounded controller operation completes. Known queue
refusal releases the gate and returns ordinary repaint policy with error 17
(an earlier session fault is retained).
Unknown completion retains only the gate and stable tags, refuses a new START,
and permits safe late completion. Decoder storage has already been reclaimed;
no borrowed plane or freed shadow can reach this job.

Mode 11 cancels acceptance and pending START while holding the existing image
mutex, then requests the controller without joining/freeing there. All existing
shadow/cache, timer, buzzer, microphone, ALS, compass and dashboard cleanup
effects remain. The controller leaves image/display locks before any video heap
wrapper, worker wait, termination or reclamation.

Existing framebuffer release/expiry and renewal paths post only atomic
generation-tagged signals and copied zero-timeout task-pool jobs. They never
take a mutex, touch an owned event/stack pointer or call the stock heap wrappers.
Older notifications cannot cancel a newer stream. The controller folds release
under the publication lock and wakes renewed lease waits. A dispatch/termination
failure retains quarantine. No new timer, device-login or bonding hook is added.
Unobservable connection loss falls back to the thirty-second inactivity bound;
packet RESET/context reset and NACK rebuild transport, not the video owner.

The native controller tests and mocked PC receive client cover cleanup, partial
startup, generation tags, repeated sessions and paged MTU-23 replies. The client
sequences NALs independently and intentionally rejects the active decoder
stage: its header-only receive probes are not valid H.264 content. Live
device validation is separate from these mocks and boot/update-start checks.

### Bounded diagnostics

Status byte 70 bit 0 advertises diagnostics (opcode 7, eight-byte request).
The 128-byte response is frozen in the same bounded replay cache, including
MTU-23 pagination. Duplicate reads do not resample or renew a video session.
Diagnostics do not require a decoder owner or verification/playback mode.

Bytes 0..3 are protocol version, opcode, control result and validity flags:
bits 0..2 validate live cached/display/other heaps, bit 3 marks quarantine,
bit 4 validates the pool-task stack, and bit 5 marks a lifecycle change during
capture. Bit 6 means the local display/UI state was known and unchanged across
heap sampling; bit 7 means awake (otherwise asleep). Bit 7 requires bit 6.
A transition or other UI state leaves both clear, as do older firmware images.
Heap comparisons require matching known display states: the awake dashboard
retains memory that is released on sleep. Unknown state is an evidence gap,
not a leak or proof of recovery. LE32 tick/generation occupy 4/8. Three 12-byte heap records at 12 hold
tick, free bytes and largest allocation; an invalid heap uses UINT32_MAX.
These bounded, lock-free TLSF views are approximate, not one atomic snapshot.
Repeat quiet post-cleanup reads before interpreting a free-byte loss as a leak.

The completed-worker report at 48 contains seventeen LE32 fields: validity,
token, current state, fault, storage peak/remaining, decoder object bytes and
alignment, worker stack used/size, guards, three captured free-byte values and
three largest allocations. No active/quarantined worker stack is scanned.
Captured heaps precede owner reclamation; compare post-IDLE live heaps too.
At 116 are LE32 pool task ID, lifetime stack bytes used and allocation size.
Stock work shares those tasks, so this is not controller-only usage.

The authenticated donor fills task stacks with 0xa5; its own high-water scan
has no bound. The sampler instead authenticates the current task against the
eight static pool TCBs, stack bases, word counts and static flags, then reads
at most the allocated 4096 bytes. It never waits, allocates, resets a shared
stack or guesses a CMSIS stack-space entry. Sampling occurs at controller
entry/exit outside image/display locks. An unavailable sample stays invalid.

### Offline references and credit sender

`tests/h264/reference_clip.py` compiles the current configured C interface and
runtime with fatal ASan/UBSan, then decodes every Annex-B NAL in order. It saves
source and decoder identity, NAL sizes, actual picture counts, contributing NAL
ranges and active-plane CRC32. The stream-format guard rejects unsupported
geometry or syntax before this reference can be used for playback. Run it in
the same environment as the integration suite:

    python3 tests/h264/reference_clip.py <clip.h264> --output <reference.json>

`tests/h264/video_client.py` works over a supplied authenticated transport. It
creates no connection and has no firmware-install operation. It reuses stock
framing, durable control request IDs, frozen pagination and exact retries.
Only observed free credits admit NALs. Up to four records share a transport
window, bounded by each recipient's free slots and next-to-consume distance.
The sender writes that window without per-record status polling and fences
the exact source-tagged ACKs before its next credit query. Explicit ACK history
can recover lost notifications. Missing replies get two exact retries; a NACK
or unrecoverable peer loss retires both owners instead of granting new credit.
Connection loss invalidates the run and requires a new cleaned-up START.

Select both targets with an explicit ingress lens to send once through the
existing bridge. Independent replies retain each generation token, sequence,
capacity and verification cursor. Full STATUS is read at startup, at most once
per five seconds during input, and at EOF/cleanup. Compact CREDITS drives input.
The explicit direct-to-both fallback uses two supplied links, the same slower
recipient bounds and separate ACKs; never switch routes inside a live session.
Neither mode synchronizes optical presentation. Later pacing must identify
completed pictures separately from transport records and NAL sequences.

Verification drains every published picture, checks owner/count/format/NAL
range/Y CRC, then acknowledges its cumulative cursor. EOF waits for all input
consumed, all expected pictures copied and all requested evidence acknowledged
before STOP. Timing anomalies exclude samples while hashes remain mandatory.
Summaries report cycle and tick milliseconds, exclusions, median/min/max/P90
and the slowest P frame, with IDR pictures separate. Optional PC pacing uses
actual reference picture boundaries, never one NAL per picture.

The caller wraps each session in ordinary lease cleanup and diagnostic reads
before START, after READY and after fully reclaimed STOP. Compare repeated
quiet heap samples and completed stack/guard reports before continuing a batch.
Ordinary playback uses no frame reads or result ACKs. Presentation-rate reports
use count differences and device copy ticks, with each lens's observed interval
count shown. Verification covers its first-to-last frame; ordinary STATUS may
cover only the tail. One endpoint yields no estimate. Host completion time is
reported separately and includes paged reply delay; neither metric establishes
optical synchronization. One early EOF snapshot can anchor the tail interval;
upload still uses compact credits with no per-NAL STATUS polling.
Diagnostic and transport waits do not belong in decode-only timing. Native mocks do not qualify optical
orientation, hardware stack use or real Bluetooth throughput.
## Standard host-link PHY adapter

The radio adapter binds a connection only from the stock EUS write callback
and the donor DM peripheral control block. Connection epochs survive handle
reuse. The original DM and application handlers still process their events
once; observing them creates no context, task or boot-time radio request.

Task-context READ/SET requests use one copied eight-byte, serial-tagged WSF
mailbox. Its immutable command state retains the epoch and permission until
completion; the full serial rejects stale mail before reading that state.
The mailbox and standard Read PHY buffer fit the 16-byte pool class; Set PHY
needs the 32-byte class. Empty classes can still fall through to larger pools,
so compact mail does not guarantee that either command has room. The
Cordio consumer revalidates the connection before submission. SET additionally
requires a finite owner's permission. It refuses submission unless the stock
HCI command queue is empty, a command credit is available, and the controller
transport FIFO has room. Fixed WSF pools retain three spare buffers. Neither
the observer nor any timer allocates, waits or issues a command.

The donor retains Set PHY and Read PHY's completion parser, but omits the
public Read PHY wrapper. The adapter constructs only that standard read
command using the same allocator and queue. Completion events, rather than
submission, establish negotiated TX/RX values. The existing boot feature edit
and UART bridge behavior are unchanged. No generic HCI interface is exposed.

The eight-byte DIAGNOSTICS request retains its heap/stack format. Adding a
little-endian uint32 selection of 1 requests a separate 128-byte PHY report;
selection 2 requests retained refusal evidence below; other selections are
refused. Header bytes 0..3 identify protocol, command,
result and selection; bytes 4..7 hold the capture tick. Bytes 8..11 identify
record version 1, record size 32, three connection slots and the source lens.
Each record begins at 12 + 32 * index:

| Offset | Field |
| --- | --- |
| 0..3 | Connection ID, validity/ownership flags, phase, last stock status |
| 4..19 | Connection epoch; current, before and after sample ticks |
| 20..25 | Before, current and after TX/RX PHY pairs |
| 26..27 | Owner count and pending operation |
| 28..31 | Finite changing permission token |

Flags identify connected, EUS-bound, current/before/after valid, owned,
pending and uncertain restoration in bits 0..7. Unavailable samples are
explicit; a pre-video or restored value is never inferred from a preference.
The tail reports command serial/completion at 108/112, pending/result/link at
116..118, and its epoch at 120. Remaining bytes are zero. All pages and exact
replays freeze one capture; only a new selection may post one bounded READ.
This report never changes the PHY or enlarges normal STATUS traffic.

Selection 2 is a distinct opt-in 128-byte report, not an extension of the
selection-1 reserved bytes. It never requests a READ or SET. Header bytes
0..3 are protocol 1, DIAGNOSTICS, result and selection 2; 4..7 are the
capture tick. Bytes 8..11 are record version 1, trace size 32, two traces
and source lens; 12 is context-present, 13 the three-buffer reserve, 14
empty-only fallback support and 15 zero. LE32 fields at 16/20/24/28 hold
the saturating attempt counter, accepted serial, completion and command
epoch. Bytes 32..35 hold pending/result/connection/operation; 36..39 are zero.
Trace records at 40 and 72 hold the last gate observation and last refusal;
104..127 are zero. No context means zero counters/traces, not an allocation.

Each trace has LE32 attempt/tick/epoch at 0/4/8, connection/operation/site/
reason at 12..15, required allocator bytes LE16 at 16, skipped empty count
at 18 and zero at 19. First eligible and donor-selected nonempty pool
samples at 20 and 24 each hold index/free-count/LE16 size. Counts saturate
at four safe nodes (four or more); index/count 255 means unavailable or
invalid, never empty. Bytes 28..31 are zero. Sites are request 1 and consumer
2; reasons 0..14 are none, argument, busy, serial exhaustion, link, epoch,
permission, pool metadata, no eligible pool, all eligible pools empty,
reserve, mailbox allocation NULL, consumer link, command credit and command
allocation NULL. Counts are captured at the guard decision, not query time.
The last refusal persists after success; consumer records retain the queued
attempt identity, so the last observation may have an older ID than a busy
refusal. Attempt IDs saturate rather than wrapping; use tick/site/epoch too.

The pool guard follows the authenticated donor's empty-class fallback only:
skip NULL free-list heads, then require four distinct safe nodes in the first
eligible NONEMPTY class, leaving three after an allocation. Never bypass a
nonempty class with one to three buffers. Invalid descriptors, pointers or
cycles fail closed. This is a fixed WSF pool view, not a TLSF heap measurement;
it never exposes pointers or changes pool ownership. Explicit selection-2
version/format validation establishes support without altering STATUS bits.

PHY ownership uses two fixed generation records; it does not allocate a task,
timer or heap buffer. A changing owner first obtains a fresh standard Read PHY
completion for its exact EUS-bound peripheral epoch. Compatible owners share
that original TX/RX pair, and the last one restores it. Already-2M links need
no Set PHY, including on exit. Off grants no changing permission and issues
no PHY request. A UART-only recipient does not invent a host connection.

Acquisition is bounded to two seconds outside image/display locks. Release is
asynchronous: stock completion and existing controller maintenance drive up to
three restoration submissions within 6.5 seconds. An accepted Set PHY is never
assumed drained because its timeout expired. Lost or unconfirmed restoration
is explicit in diagnostics and blocks new changing owners until disconnect.
An Off owner may still play without changing it. Callback paths only copy
fixed state and post a zero-timeout controller reason. Disconnect retires the
epoch and cancels its video generation in task context, never in the callback.
START option bit 1 selects Off; zero is Video-only. The advertised mask is
0x83 (native presentation, radio Off, verification). Reserved bits still fail,
and replay includes the radio option. Acquire precedes power/storage startup;
every failed/partial START and every normal, fault, sleep, lease, disconnect or
reset retirement releases its generation. Quarantined worker storage remains
allocated while independent fixed-state radio restoration is attempted.
Unconfirmed acquisition reports control error 18. STOP may reach IDLE before
asynchronous restoration finishes; inspect the PHY diagnostics after it.
Boot and non-video traffic do not request a PHY.

The supplied-link PC client selects `radio="off"` or `radio="video"` on START;
Video-only is the default. `stream_clip` checks the Off capability on every
target before sending it. `phy()` reads the source-tagged PHY selection for
one lens, forwarded paired delivery or two independent host links. No PC
function exposes a raw controller command or changes the UART bridge.

Capture PHY before START, at READY, after input drains and after STOP.
The optional `on_finished` callback runs after the playback interval is
recorded and before STOP, keeping observational paging out of timed input.
Compare matching connection epochs, actual TX/RX and confirmed restoration
of the saved pair. Classify symmetric 1M and 2M separately; Off can already
be 2M. Exclude unknown, asymmetric/coded, fallback, changed-epoch or uncertain
restoration evidence. Sparse snapshots do not prove uninterrupted RF state.

For hardware comparisons, alternate Off and Video-only with the same clip,
MTU, pacing, distance and radio environment: one warm-up and at least three
repetitions on each lens, both forwarded ingress choices and both direct.
First verify a short clip's Y CRCs, then use evidence-off playback and a
parser-checked filler/SEI workload to separate transport from decode time.
Count consumed NAL payload bytes and the fenced input interval independently
of completed pictures. Retain credits/refusals, command/page round trips,
device-copy FPS and per-lens aggregate decode timing. The peer path is UART;
only indirect input/scheduling effects of host PHY can change its throughput.
Check stock hand-back, same-state heaps, stacks and restoration after every
exit. RF range, battery behavior and optical presentation sync need their
own device evidence; a successful boot does not establish them.
## Picture Timeline Records

The decoded-picture FIFO owns admitted source-size A4 or original Y8 storage
provided by its session owner. Depth 1 or 2 is explicit; full queues apply
backpressure rather than overwrite. Producer Y is copied before the next
decode. A published head can only be claimed by its exact picture ordinal,
and it stays unavailable until actual display completion releases it.
Cancellation stops admission but does not invalidate an outstanding presenter
pin. Serial/generation exhaustion refuses instead of reusing a stale token.
Advanced capability operation 13 advertises queued playback; START bit 4
enables it only with a configured timeline and admitted task/queue ownership.

Optional ID31 operations 11 and 12 retain protocol version 1 and the existing
24-byte bounded replay format. TIMELINE 11, while stopped, carries little-endian
u32 stream/duration_ms/start_pts_ms and u16 fps numerator/denominator after
the eight-byte control header. This contract accepts exactly 10/1, 15/1 or 20/1.
The capability rate mask replaces the former fixed-rate byte: older apps
reject it, and apps requiring this mask reject the old fixed-rate firmware.
FRAME_TIME 12 carries u32
stream/picture_ordinal/terminating_nal_sequence/pts_ms.
Pictures are one-based; parameter sets and filler do not increment ordinals.
Six metadata records have credits independent of compressed input slots.
Exact duplicates are idempotent; conflicting, stale and overflowing records
refuse without overwriting pending media identity. RESET retires the generation.
Metadata alone does not enable queued/synchronized presentation. Playback
without the explicit queued START option retains legacy decode-ready behavior.

## Playback Controls

Advanced capability operation 13 advertises bit 16 for owner-bound playback
controls. PAUSE 16, RESUME 17 and PLAYBACK 18 use a 20-byte request: the control
header, stream, LEFT token and RIGHT token. The 36-byte frozen reply contains
version/operation/result/lens followed by stream, local token, actual paused
state, requested pause, shown picture count, media position in milliseconds,
duration and pending paired transition (all u32 little-endian).
Position advances only after display copy. Commands publish intent; clock,
boundary and wire processing remain on the independent presenter task.

RIGHT waits for both copies before a paired pause. Ordered, stream-qualified
PAUSE/RESUME wire records carry a finite control serial and the completed
picture boundary. LEFT acknowledges exact repeats; RIGHT blocks new triggers
until that acknowledgement, retrying at 100 ms with a 2 s limit. Resume
reanchors
the cadence after prebuffering. Pausing retains owned queues and the radio
lifecycle; STOP/RESET still drain and restore. A supplied-link PC client can
read and control playback without opening any connection itself.

PACE 25 puts the picture schedule on the phone's audio clock: a 32-byte
request with the control header, stream, LEFT token, RIGHT token, picture
ordinal, then the due tick in LEFT's and RIGHT's own millisecond clocks. The
reply is the 36-byte playback state above. Only the timing coordinator (RIGHT,
or a lone lens) applies it: the commanded anchor is moved to its next picture
with the same rational spacing, and one landing more than 10 s from now is
ignored. Once paced, a late picture or the end of a starvation keeps the
commanded schedule, so later pictures catch up instead of re-anchoring to now.
A pause or resume drops pacing and any pending PACE until the next one. Firmware
without PACE refuses it with the generic status reply (operation 7, result
FORMAT), so the phone can probe for it without a capability bit.

## Seek Generation

Advanced capability bit 32 advertises seek version 1 with maximum 255 decoded
preroll pictures (capability bytes 6/7). START byte 19 is that explicit prefix;
option 32 requests a paused target preview. Both require queued presentation.
TIMELINE retains the full media duration and target PTS. Prefix metadata must
precede the target; visible metadata must not. Each decoded prefix picture
updates references but never enters the display queue or shown reports.
Display/evidence ordinals count actual copies separately from media ordinals.

The PC seek helper drains RESET, selects the preceding IDR and its applicable
SPS/PPS, preserves every tail NAL and starts a fresh generation. It requires
the target's shown record from every lens before confirming arrival. A paused
seek shows the target once and pauses at the common completion boundary;
explicit PC resume continues it. The serialized upload control callback can
send controls while pumping, without concurrent transport writers. Seeking
to duration selects the last available picture; unbounded GOP prefixes refuse.

## PC Menu and Scale

Advanced capability bit 64 advertises MENU 19 and SCALE 20. Both use 24-byte
owner-bound requests: header, stream, LEFT token, RIGHT token, value. MENU
values 0..5 mean open, close, next, previous, apply and query. SCALE 1 selects
native, 2 selects double. A pending single-slot menu intent returns BUSY;
retry with a fresh request, never overwrite it. RIGHT applies paired actions
on the presenter and sends the common pause/menu view with finite serial/ACK.

Replies extend the 36-byte playback state to 48 bytes: view, seek serial and
seek target milliseconds (u32 each). View low bits encode open plus selection
shifted 1; bit 4 is native selected and bit 5 is native active. Seek selection
cycles PLAY, -10, +10, -30, +30 seconds; targets clamp to media duration.
Applying
PLAY resumes. Other selections publish a PC source seek request; the supplied
PlaybackController performs RESET/restoration/preroll on its single writer.
Paused EOF retains the preview until explicit resume rather than silently STOP.

The existing raw 20 px bitmap font draws position/duration and selection into
the bottom 48 px letterbox, clipped to that band. Menu-only tagged copies retain
the frozen video and do not change media counts, copy time or SHOWN records.
Scale is latched on the common frame trigger; retry keeps that frame's scale.
There is no radio entry, physical-input hook or independent gesture route.

## Selected Radio Policy

Advanced capability bit 128 advertises RADIO_POLICY 21. A 12-byte request
contains
the control header and u32 value: 0 Off, 1 Video-only, UINT32_MAX query.
Video-only
is the unconfigured default. Its 24-byte frozen reply contains
version/op/result/
lens, then selected policy, active policy (UINT32_MAX while stopped), pending,
wire-fed peer and reserved 0. These describe policy, not negotiated PHY speed.

Changing the field while active only selects the next START/seek generation;
it does not pause, drain, restore or recreate the current owner. Resume is not
a boundary. Legacy explicit START Off still forces that generation Off. The
normal retirement path restores every actual owner's original pair unchanged.
A validated wire-fed peer makes no PHY READ/SET request, even with a bound
lease-only host; non-changing epoch ownership still tracks host loss and
grants no 2M permission. Locally delivered ingress/direct owners keep read-before-
set, finite WSF handoff, refusal reporting and all existing exit restoration.

## Luma Range

Each completed picture retains the active SPS range, including explicit versus
inferred TV signalling. Owned FIFO slots keep those flags beside their copied
pixels. A4 is quantized once before publication; Y8 stays original until packing.
Changing an SPS does not retag queued pictures. SHOWN flags 8 and 16 indicate TV
and unsignalled TV respectively; decoded Y and evidence CRCs remain unchanged.

The rescale curve uses f=TV?normalize(Y):Y, then
level=(16+(f*239+127)/255)>>4. The immutable normalization table clamps
Y<=16 to 0 and Y>=235 to 255. A level table avoids hot-path division.
Rescale is the only mapping. Decoder bytes, evidence CRCs and off margins
remain unchanged. Retired development command 23 is refused; advanced
capability byte 7 bit 3 now advertises multi-slice pictures; paired picture
wire byte 23 is a reserved zero.

Plain doubled A4 expands each packed pair through a 512-byte immutable
table, writing both physical rows directly: 30720 lookups for a 320x192
picture. Borrowed Y8 similarly quantizes each source pixel once for both
rows. Native packing, already-quantized A4 levels, margins and source/
destination bounds remain unchanged; neither path needs extra frame RAM.

## Optional Dithering

Advanced capability byte 7 bit 1 advertises owned Y8 dithering; bit 0 remains
the
seek contract version 1. START bit 16 selects it for the whole generation and
requires queued presentation. Default off keeps two 30720-byte A4 snapshots;
on admits two 61440-byte original Y8 snapshots under a separate 139264-byte
presenter cap (A4 cap 77824). Heap reserves, decoder cap and cache ownership
are unchanged. Refusal is explicit; no automatic depth or representation change.

The immutable quarter-level table uses t=clamp((f*239+510)/1020+2,4,60),
with integer division, centered half a level below the continuous rescale.
This follows the truncated plain mapping within half a level per cell
average, retaining exact endpoints 1 and 15. TV luma is normalized first.
Final panel coordinates choose the 2x2 thresholds [0,2;3,1], base=t>>2
and exactly t&3 one-level promotions. Cell sums remain t and each output
is monotonic in Y. Picture margins remain off. Both physical rows
of doubled video are evaluated separately; native uses the same panel phase.
No low bits are reconstructed from A4. SHOWN flag 32 records on-mode. Picture
wire records use byte 22 for the generation's mode; mixed paired modes refuse.
The supplied PC seek/controller retains the choice in a fresh generation.

At 2x scale an immutable 2048-byte cell table performs one lookup per source
sample and writes its two packed panel rows. TV normalization and quarter
evaluation are already in each cell; the decoded-luma curve selects the raw
table. Width alignment guarantees an even centered x, while both centered
row phases are retained. Native scale keeps the scalar path. No new frame
RAM, extra Y8 copy, decoder change or phase approximation is required.

## Retained Worker Failure Evidence

Diagnostic kind 7 uses opcode 7 with a twelve-byte request: kind at byte 8,
selector 0 or 1 at byte 9, and zero bytes 10..11. Each frozen reply is 128
bytes. Bytes 0..7 are protocol, opcode, result, kind, selector, schema 1,
lens and valid. Before any START the query is stale. The record survives STOP
and owner reclamation. A new START that passes validation and creates a
control generation clears it, even if startup subsequently fails. No periodic
reporting, decoder recovery or power-policy change is implied.

Selector 0 contains little-endian words at these offsets:

| Offset | Field |
|---|---|
| 8, 12, 16 | Stream, owner token, exact worker fault |
| 20, 24, 28 | Input sequence, length, NAL type |
| 32, 36 | FNV-1a at decoder entry and at failure |
| 40 | Reject stage: wrapper 1, NAL parse 2, input contract 3, core 4, completed-frame contract 5; 0 means no decoder rejection |
| 44, 48, 52, 56 | Core stage, MB entered/completed, bit offset |
| 60, 64 | Parsed first_mb_in_slice and frame_num; UINT32_MAX if unavailable |
| 68, 72, 76 | Picture-open flag, next MB and open frame_num before this NAL |
| 80, 84 | Core allocation failure tag and size |
| 88, 92 | Failure tick and history count |
| 96, 100 | Runtime allocation failure tag and size |
| 104, 108, 112, 116 | Power acquire step, peer generation, stream and closed flag |
| 120, 124 | Input still pinned, decoder constructed |

Selector 1 repeats stream and current sequence at offsets 8 and 12, then the
history count at 16. At offset 32 are up to eight twelve-byte records, oldest
first: sequence u32, checksum u32, length u16, NAL type u8, reserved zero u8.
The current NAL is separate from the previous successfully consumed inputs.
For a fault outside decode, selector 0 describes the last attempted input,
or sequence UINT32_MAX/length zero if there was none; the pinned flag is zero
and no second checksum is taken. A released slot is never sampled.

Checksums are FNV-1a over raw NAL bytes including the header, without Annex B
prefix: initial 2166136261, then (hash XOR byte) * 16777619 modulo 2^32. This
cheap diagnostic checksum does not replace transport CRC or prove equality
against every possible collision. The phone compares against its bounded
attempted-payload ring; a matching attempt does not prove receipt or ACK.

STATUS error 8 is a broad worker-fault category, including initialization,
ownership and runtime faults. A returned decoder error maps to INPUT(11).
The exact fault and reject stage distinguish these cases. START POWER(16)
retains its acquisition barrier in STATUS byte 69 >> 3; neither error alone
establishes a physical readiness or decoder root cause.

## Receive And Clock Evidence

Playback capability byte 7 bit 5 advertises diagnostic kinds 4 and 5. Both use
opcode 7 and a twelve-byte request: kind at 8, selector at 9, zero bytes 10..11.
They return frozen 128-byte snapshots through the normal PAGE contract.

Kind 4 selects origin index 0 or 1. Header bytes 3..7 are kind, schema 1,
origin bit, responding lens and row count. LE32 serial/dropped are at 8/12;
byte 16 is row size 24. Four newest-first records start at 32. Each contains
LE32 serial/tick/request at 0/4/8, opcode/page/reason/proven budget at 12..15,
frozen capacity/page count at 16/17, and LE32 detail at 20. For NALs the request
field is the NAL sequence. Reasons are 1 lock timeout, 2 missing entry, 3 page
range, 4 capacity, 5 pending/scratch overlap, 6 reserved reply-allocation,
7 copied-send failure, 8 format, 9 replay and 10 command result. Reply allocation
cannot fail now because the scratch is context-owned. A bounded atomic claim
protects each ring even after a control-lock timeout; contention increments
dropped rather than waiting. This evidence covers dispatched video requests,
not failures decoding a compressed record before its opcode is known.

Kind 5 selects START/steady/STOP indices 0/1/2. The presenter alone captures
samples, including stop before normal park, under publication protection. No
clock service is called and no register is written. Bytes 3..7 are kind, phase,
schema 1, lens and flags: valid 1, 250..1000 ms bracket 2, CYCCNT enabled 4,
performance request changed 8, tick-pair skew 16. Missing samples stay invalid.
LE32 stream/generation are at 8/12. First tick/CYCCNT/tick-after are at 16/20/24;
last are at 28/32/36. Raw words at 40..92 are MCUPERFREQ, CLKGEN CLOCKENSTAT,
MISC, CLKCTRL, MCUCTRL PLLCTL0, PLLDIV0, PLLDIV1, manager word, DWT CTRL,
SDK cached mode, ending MCUPERFREQ, starting MCUPERFREQ, PLLSTAT and PLLMUXCTL.
Source/divider offsets follow Apollo510 CMSIS; donor mode/cache/manager addresses
are fixed to the guarded image. Clock transitions, stopped counters, long spans
or tick skew prevent a trustworthy cycle ratio; raw evidence is retained.
The rolling steady bracket is at least 250 ms without sleeping or busy-waiting.
These snapshots do not establish a physical core frequency by themselves.
