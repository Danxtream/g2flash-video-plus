# Inert decoder interface

This GPL-3.0 interface uses the vendored MIT Sub0h264 decoder, upstream revision
15421eeade48774929bc0221ea0418a25462c222 plus the G2 changes in third-party/sub0h264.
Its license, modification notes and Patent Notice are in the repository README.

The firmware build closes the decoder together with the existing C unit.
Boot, transport and display do not call it. No decoder allocation occurs at boot.
The runtime provider reads the existing writable context and remains unbound
until a firmware owner publishes its callbacks, so idle initialization refuses
to start. The standalone decoder retains the weak unbound provider for tests.

Separately closed blobs cannot resolve the decoder's runtime provider across
their boundary without a fixed address. The firmware instead links the C unit,
hot/cold decoder objects and runtime into one closed PIC object, replacing the
weak provider with its context-backed definition and sharing the C memory
helpers. Stock hook targets use the combined function table; byte guards remain.
No writable globals, startup hook or external relocation is introduced.
The combined blob keeps the existing payload-size, checksum and program-memory
ceiling checks. Regenerate the patch list and update the output hash after build
changes:

    bash build_cfw.sh --skip-venv --update-patches

The decoder's IR, objects, exports, sizes, alignment and SHA256 are recorded in
obj/h264/. Inspect them with the standalone command below. The plain firmware
build checks regeneration and replays the committed patch list without needing
the C++ toolchain to apply it.

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
in this folder.

Build hot and cold decoder modules separately for inspection:

    python3 patches/h264/build_decoder.py --output <output-directory>

hot_functions.json freezes the 12 base hot groups and the 229 promotion symbols
selected from the glasses speed tests. Small helpers, CAVLC residual parsing and
P-macroblock decoding are promoted into the -O2 hot module. The list was
recorded from the speed-test build, whose runtime provider was named
imports(); it maps explicitly to g2_h264_runtime_current here. That explicit
mapping leaves 228 promoted C++ definitions. Every decoder function retains
its selection. Missing, duplicate or ambiguous mappings fail the build.

The -O2 frontend emits preopt.ll; hot functions compile at -O2 and cold functions
at -Oz with minsize/optsize attributes. There is no LTO or automatic promotion.
Each function has one owner. Read-only table values are visible to the hot
optimizer through available_externally initializers, so it can fold constants
without emitting another table copy. The cold module retains sole ownership of
table storage; its IR and COMDAT membership remain unchanged. Symbols and
independent links verify that the hot module emits no data and every retained
named table has exactly one copy.

The output contains IR, full membership/mappings, object files, stack usage,
commands, a closed PIC object and decoder.bin/decoder.json. The selection hash
covers the effective parsed configuration, including caller-supplied selections.
The standalone link uses the inert C ABI plus runtime and existing C memory
helpers. The firmware link writes firmware.bin/firmware.json and its combined
function table beside those intermediates. Include-root overrides are the same
as toolchain.py.

Partition tests compare every Y byte from independent whole/partitioned native
builds under ASan/UBSan. Optional G2_H264_TEST_CLIPS adds local clip paths separated
by the platform path separator. A test-only entry admits the long G2 fixture's
large NALs; the production 4086-byte admission limit remains unchanged.
