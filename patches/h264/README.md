# Inert decoder interface

This GPL-3.0 interface uses the vendored MIT Sub0h264 decoder, upstream revision
15421eeade48774929bc0221ea0418a25462c222 plus the G2 changes in third-party/sub0h264.
Its license, modification notes and Patent Notice are in the repository README.

This step supplies the C interface and runtime only. It is not wired into the
firmware build, boot, transport or display. No decoder allocation occurs at boot.
The default runtime provider is unbound, so initialization refuses to start.
The later firmware owner supplies callbacks from its writable context.

Call size/alignment, allocate aligned object storage, then init. The runtime
allocator, release, allocation-preflight and non-returning failure callbacks must
remain bound to the same single owner through destroy. Unexpected allocation/STL
failure reports and parks that worker; its owner must terminate it before freeing
partially constructed state. Bounded cached allocation and that lifecycle belong
to the worker integration. This interface does not provide an OOM recovery loop.

Chroma is always skipped. Feed raw NALs including the header, without Annex-B
prefixes. The current 4096-byte record leaves 4086 bytes for each NAL; RBSP scratch
is reserved once at 4096 bytes. CONSUMED and FRAME_READY are distinct: input is
not a picture count. A borrowed Y view exists only after FRAME_READY and expires
at the next decode/destroy. Decode/preflight errors require a decoder restart.
The unchanged decoder still supports only single-slice pictures; this API does
not bake that restriction into queue or transport sequencing.

The decoder configuration used for the glasses speed tests keeps one SPS/PPS
(ID zero), no legacy output copy, SUB0H264_TRACE=0 and all structured DecodeTrace
callbacks compiled in. The timing shim and absent profile hooks disable
measurement instrumentation only. CABAC, High support and stream-controlled
deblocking are unchanged.

toolchain.py discovers ARM GCC's installed newlib headers and the softfp multilib.
It prints the C++23 PIC decoder flags used for the glasses speed tests, without
-ffreestanding. Local overrides:

    python3 patches/h264/toolchain.py --cxx-include-root <versioned-C++-folder> --c-include-root <C-folder>

Run interface/ARM closure/LLD/sanitizer and existing linker checks from the root
in Ubuntu (WSL):

    python3 -m unittest discover -s tests/h264 -v

Requires clang, clang++, ld.lld and ARM GCC/newlib development headers. Tests
generate a tiny I/P stream in memory; optional fixture checks read ignored clips
in place and keep all outputs in temporary directories. No binary or clip belongs
in this folder. The final mixed partition/constant tooling is a later build step.
