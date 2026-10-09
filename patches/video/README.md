# Cached decoder ownership

The internal worker API is inert until a caller explicitly starts it. No boot
path creates a video allocation, task or timer. Start requires a live framebuffer
lease and an absent texture cache. Release jim's previous lease normally before
acquiring the video lease; this owner never frees another client's resources.

Call start, stop and reset outside the image mutex and display gate. Handlers
already holding the image mutex may only request cancellation there, then leave
that scope before reaping. The future presenter keeps image-before-display lock
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
charged twice. There is no video allocation or fallback in heaps 13 or 27.

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

## Private transport controls

ID 31 uses protocol version 1. The control header is ID, opcode, version,
zero flags and a nonzero LE32 request ID. Named opcodes are capabilities (0),
start (1), stop (2), reset (3), status (4), page (5) and NAL ingress (6).
Capabilities/status have only the eight-byte
header. Stop/reset append the current LE32 stream ID. Reset ends ownership;
a fresh start uses a strictly increasing stream ID after reclamation.

Start is 24 bytes: header, LE32 stream ID, LE16 width/height, reference count,
DPB frame count, zero presentation flags, zero reserved byte and LE32 interval
in milliseconds. Only 320x192, one reference, two DPB frames and intervals
10..1000 ms are accepted. Chroma remains skipped. Release the previous custom
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
duplicates refuse. Each reply freezes an 80-byte status snapshot. A page query
has the original request ID and one page byte, and never executes the control.
One response per query fits the proven notification capacity, including MTU23.
Replies are ID31, source lens bit, LE32 request ID, page/count and snapshot bytes.
The distinct bridge video-return envelope leaves upstream ACK/NACK validation
unchanged. Send services copy each bounded stack reply before return.

Snapshot bytes 0..3 are version, supported stage, control result and lifecycle
state; LE32 values at 4/8/12/16/20/24/28 are stream, generation, left/right request
high-water, stream high-water, last error and interval. Record/NAL maxima are
LE32 at 32/36; geometry at 40/42, references/DPB at 44/45, forced chroma skip and
single-slice limit at 46/47, four/six-slot bounds at 48/49. Capacity/free slots are bytes 50/51; LE32
accepted/consumed NAL counts are at 56/60. Expected sequence is LE32 at 52. Completed pictures are LE32 at 64;
bytes 68..79 are reserved for recovery details. Stage 3 announces ordered ingress with
no NAL consumer or completed-picture/presentation support. Transport ACK confirms only
accepted command validation; refusals preserve upstream NACK/context-reset rules.

Transport allocation checks preserve the heap-13 reserve and missing-shadow
allowance before and after stock-coordinated allocation. Cached decoder sizes
come from actual requests, including future compile-time feature changes.

NAL ingress has a separate ten-byte header: ID31, opcode6, LE32 stream and
LE32 NAL sequence, followed by exactly one raw NAL (no Annex-B start code).
The record limit is 4,096 bytes and raw NAL limit 4,086 bytes. START selects
the version; data does not carry a second version byte. The owner rejects
wrong streams/origins, invalid headers, oversize records and cancelled input.
Its four cached snapshots include any consumer-active slot. Borrowed transport
bytes never escape dispatch. A future consumer claims/releases a generation-
checked owned view; this receive-only firmware retains every accepted slot and
refuses at capacity. A NAL, transport record and completed picture are distinct.

Sequence zero starts a new stream. At capacity four, at most three future NALs
fit ahead of the expected one; sequence distance is strictly below capacity.
Only the expected sequence can be claimed. Matching queued/recently released
bytes are idempotent; conflicting duplicates refuse. Released slot bytes are
the bounded exact replay history until reuse, after which older retries refuse.
The highest sequence is reserved to prevent wrap; start a new stream instead.

A worker-only completed-picture notification may attempt one 8 KiB extension
when the decoder reports its two-frame DPB fully allocated. Allocation happens
outside image/display locks, with fresh live heap/reserve checks. The two slots
publish together; failure restores the prior cap and keeps four slots. This
receive-only firmware never emits that notification. Native fake consumers
exercise the extension and separate NAL/picture counters.

### Gap recovery

Future NALs start a five-second missing-input deadline. Later inputs, retries
and status queries cannot extend it; input is checked before copying so a late
missing NAL cannot repair an expired stream. Thirty seconds without accepted
NAL input or a fresh owner-origin status/capability request retires a session;
cached control replay does not renew it. Event waits use the nearest activity,
gap or existing framebuffer-lease deadline, including millisecond tick wrap.
No timer, polling loop or display wait is added.

Conflicting duplicates, sequence exhaustion, invalid header progression and
expired gaps stop acceptance and require fresh START with a larger stream ID.
The controller reclaims only after cancellation, pin drainage and worker park;
allocation, joins and frees stay outside image/display locks. A failed reaper
keeps quarantine. Header receipt requires SPS, PPS and IDR before non-IDR
slices; it does not validate their contents or claim decoded pictures.

Status stage four remains receive-only. Byte 68 is recovery state (0 clear,
1 waiting, 2 needs a fresh stream), byte 69 records SPS/PPS/IDR header receipt
bits, offset 72 is the gap deadline and offset 76 the first missing receipt
sequence. Expected consumption at offset 52 and completed pictures at 64 remain
separate. All pending queue bytes belong to the retired owner, never the next
stream, and the worker still does not consume or decode them.

### Upstream session cleanup

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
sequences NALs independently and asserts no consumption/presentation. Live
device validation is separate from these mocks and boot/update-start checks.
