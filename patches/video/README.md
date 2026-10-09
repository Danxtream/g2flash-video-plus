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
The owner byte cap is 206,904 bytes, excluding the future four 4 KiB input slots.
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
start (1), stop (2), reset (3), status (4) and page (5); NAL ingress (6) is
reserved until implemented. Capabilities/status have only the eight-byte
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
duplicates refuse. Each reply freezes a 64-byte status snapshot. A page query
has the original request ID and one page byte, and never executes the control.
One response per query fits the proven notification capacity, including MTU23.
Replies are ID31, source lens bit, LE32 request ID, page/count and snapshot bytes.
The distinct bridge video-return envelope leaves upstream ACK/NACK validation
unchanged. Send services copy each bounded stack reply before return.

Snapshot bytes 0..3 are version, supported stage, control result and lifecycle
state; LE32 values at 4/8/12/16/20/24/28 are stream, generation, left/right request
high-water, stream high-water, last error and interval. Record/NAL maxima are
LE32 at 32/36; geometry at 40/42, references/DPB at 44/45, forced chroma skip and
single-slice limit at 46/47, planned four/six-slot bounds at 48/49. Current queue,
NAL consumption and completed-picture fields remain zero: this is control-only
support, without a receive/decode/display route. Transport ACK confirms only
accepted command validation; refusals preserve upstream NACK/context-reset rules.

Transport allocation checks preserve the heap-13 reserve and missing-shadow
allowance before and after stock-coordinated allocation. Cached decoder sizes
come from actual requests, including future compile-time feature changes.
