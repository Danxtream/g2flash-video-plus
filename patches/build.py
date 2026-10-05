#!/usr/bin/env python3
"""
Compile a C source to position-independent Thumb-2 machine code for the G2
mainapp core (ARMv7E-M, Cortex-M-class), close its internal relocations and
emit a movable code/constants blob with no external symbol dependencies.

Usage:
  python3 build.py <src.c> [-Dname=val ...]           # human report + obj/<stem>.text.bin
  python3 build.py <src.c> [-Dname=val ...] --json     # machine-readable JSON to stdout

Build intermediates (<stem>.o, <stem>.text.bin) are written to g2flash/obj/ (created
on demand), not next to the sources.

Human mode prints, per exported function, its offset/size and the raw bytes (hex)
and writes obj/<stem>.text.bin. JSON mode prints a single object:

  {
    "src": "zlib_glue.c",
    "text_len": 1394,
    "text": "<hex of the full .text section>",
    "functions": [{"name": "...", "offset": 0, "size": 14, "bytes": "<hex>"}, ...]
  }

so patch_compress.py can pull the exact bytes it injects straight from the build
instead of carrying pasted hex. --json has no side effects beyond the obj/<stem>.o the
compiler emits (it does NOT write obj/<stem>.text.bin).

Self-containedness is enforced in both modes. build.py acts as a mini-linker over
the emitted blob and resolves these relocation families in place:

  * Internal branches (R_ARM_THM_CALL / R_ARM_THM_JUMP24 across executable
    sections): the BL/B.W displacement is rewritten, so injected functions can call
    each other by name (incl. from inline asm) without an "everything must be
    static" restriction.

  * PC-relative read-only-data references (R_ARM_THM_MOVW_PREL_NC / MOVT_PREL,
    emitted under -fropi): string literals and other read-only constants. The
    referenced .rodata* sections are appended to the blob right after .text and
    the movw/movt immediate pair is fixed up so the runtime `add rX, pc` lands on
    the datum. Because these are PC-relative, the fixup depends only on the datum's
    offset WITHIN the blob, so the result stays position-independent regardless of
    where the blob is later loaded -- exactly like the branch case. This is why we
    compile with -fropi: absolute (MOVW_ABS/MOVT_ABS) rodata refs would need the
    final load address, which only patch_compress.py knows.

  * C++ R_ARM_REL32 references: signed relative data and Thumb function
    references are resolved against the final section layout.

The emitted bytes are .text, other executable sections, then read-only data; the
returned/reported `text`/`text_len` cover the whole blob and function offsets stay
blob-relative. `rodata_len` reports how
many trailing bytes are data -- callers that extract a SINGLE function's bytes
(rather than appending the whole blob) must assert rodata_len == 0, since a lone
function carries no rodata with it.

Any OTHER relocation -- an external/undefined branch target, an absolute rodata
ref, or a relocation inside the rodata itself (e.g. an array of pointers to string
literals, which needs data-to-data fixups) -- is still a hard error, because PIC
injection has no linker to fix absolute addresses up (firmware entry points must be
called via absolute-constant function pointers instead).
"""
import sys, os, struct, subprocess, json

# Build intermediates (.o, .text.bin) go in g2flash/obj (a sibling of this patches/
# dir), created on demand, rather than cluttering the source tree next to the .c files.
OBJ_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "obj")

def obj_path(src, suffix):
    """Path for a build intermediate of `src` (by basename) inside OBJ_DIR."""
    os.makedirs(OBJ_DIR, exist_ok=True)
    stem = os.path.basename(src).rsplit(".", 1)[0]
    return os.path.join(OBJ_DIR, stem + suffix)

R_ARM_THM_CALL         = 10   # BL / BLX  (Thumb-2, 32-bit)
R_ARM_THM_JUMP24       = 30   # B.W       (Thumb-2, 32-bit)
R_ARM_THM_MOVW_PREL_NC = 49   # movw rX, #:lower16:(sym - .)   (PC-relative, -fropi)
R_ARM_THM_MOVT_PREL    = 50   # movt rX, #:upper16:(sym - .)   (PC-relative, -fropi)

def resolve_thumb_branch(tbytes, off, target):
    """Rewrite the Thumb-2 BL/B.W at tbytes[off:off+4] to branch to .text offset
    `target`, preserving the BL-vs-B.W opcode bits. Both are PC-relative to off+4."""
    hw2_old = tbytes[off + 2] | (tbytes[off + 3] << 8)
    disp = target - (off + 4)
    if disp % 2 or not (-(1 << 24) <= disp < (1 << 24)):
        raise BuildError(f"branch at {off:#x} -> {target:#x} out of Thumb range (disp {disp})")
    imm = (disp >> 1) & 0xFFFFFF
    S     = (imm >> 23) & 1
    i1    = (imm >> 22) & 1
    i2    = (imm >> 21) & 1
    imm10 = (imm >> 11) & 0x3FF
    imm11 = imm & 0x7FF
    j1 = (~(i1 ^ S)) & 1
    j2 = (~(i2 ^ S)) & 1
    hw1 = 0xF000 | (S << 10) | imm10
    hw2 = (hw2_old & 0xD000) | (j1 << 13) | (j2 << 11) | imm11   # keep bits 15/14(type)/12
    tbytes[off:off + 4] = bytes([hw1 & 0xFF, hw1 >> 8, hw2 & 0xFF, hw2 >> 8])

def _thumb_movwt_get_imm(hw1, hw2):
    """Extract the 16-bit immediate encoded in a Thumb-2 movw/movt (T3) pair."""
    imm4 = hw1 & 0xF
    i    = (hw1 >> 10) & 1
    imm3 = (hw2 >> 12) & 7
    imm8 = hw2 & 0xFF
    return (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8

def _thumb_movwt_set_imm(hw1, hw2, val):
    """Return (hw1, hw2) with the 16-bit immediate field replaced by `val`."""
    val &= 0xFFFF
    imm4 = (val >> 12) & 0xF
    i    = (val >> 11) & 1
    imm3 = (val >> 8) & 7
    imm8 = val & 0xFF
    hw1 = (hw1 & ~((1 << 10) | 0xF)) | (i << 10) | imm4
    hw2 = (hw2 & ~((7 << 12) | 0xFF)) | (imm3 << 12) | imm8
    return hw1, hw2

def resolve_movwt(blob, off, sym_addr, high):
    """Fix up the PC-relative Thumb movw (high=False) / movt (high=True) at
    blob[off:off+4] so the movw/movt pair materializes `sym_addr - P`, where the
    symbol and the instruction are both blob-relative (P == off). The addend is
    read in place (ELF REL form, sign-extended 16-bit). movw takes bits [15:0] of
    the result; movt takes bits [31:16]. Together with the `add rX, pc` the
    compiler emits, this reconstructs `sym_addr` at runtime, independent of load
    address (the relative distance is what's baked in)."""
    hw1 = blob[off] | (blob[off + 1] << 8)
    hw2 = blob[off + 2] | (blob[off + 3] << 8)
    addend = _thumb_movwt_get_imm(hw1, hw2)
    if addend & 0x8000:
        addend -= 0x10000
    result = (sym_addr + addend - off) & 0xFFFFFFFF
    val = (result >> 16) & 0xFFFF if high else result & 0xFFFF
    hw1, hw2 = _thumb_movwt_set_imm(hw1, hw2, val)
    blob[off:off + 4] = bytes([hw1 & 0xFF, hw1 >> 8, hw2 & 0xFF, hw2 >> 8])

CLANG = "clang"
CFLAGS = [
    "--target=thumbv7em-none-eabi", "-mthumb",
    "-O2", "-ffreestanding", "-fno-jump-tables", "-fomit-frame-pointer",
    "-fno-builtin", "-mno-unaligned-access",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-fropi",   # PC-relative rodata refs so string literals resolve in-blob (see module docstring)
    "-Wall", "-Wextra",
]

# ---- minimal ELF32 LE parser (section headers + symtab) ----
def parse_elf(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 1 and d[5] == 1, "not ELF32-LE"
    (e_shoff,) = struct.unpack_from("<I", d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x2e)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsz = \
            struct.unpack_from("<IIIIIIIIII", d, off)
        secs.append(dict(name=name, type=typ, flags=flags, offset=offset,
                         size=size, link=link, info=info, align=align, entsize=entsz))
    shstr = secs[e_shstrndx]
    def sname(n):
        s = d[shstr["offset"] + n:]
        return s[:s.index(b"\0")].decode()
    for s in secs:
        s["sname"] = sname(s["name"])
    return d, secs

def section(secs, name):
    for s in secs:
        if s["sname"] == name:
            return s
    return None

class BuildError(Exception):
    pass

SHT_PROGBITS = 1
SHT_REL      = 9
SHF_WRITE    = 0x1
SHF_ALLOC    = 0x2
SHF_EXECINSTR = 0x4

def _is_rodata(sec):
    """True for a read-only allocated data section (rodata, string/constant pools):
    PROGBITS, ALLOC, neither writable nor executable. Name-agnostic so .rodata,
    .rodata.str1.1, .rodata.cst16, .rodata.* are all captured."""
    return (sec["type"] == SHT_PROGBITS
            and (sec["flags"] & SHF_ALLOC)
            and not (sec["flags"] & (SHF_WRITE | SHF_EXECINSTR)))

def compile_text(src, extra=()):
    """Compile `src` to Thumb-2 and return (blob, funcs, rodata_len). `blob` is the
    executable sections followed by read-only data (supported relocations
    resolved in place; see module docstring); `funcs` is a list of (name, offset,
    size) with offsets blob-relative and
    sizes resolved; `rodata_len` is the count of trailing data bytes. Raises
    BuildError on any relocation that can't be resolved position-independently or
    any reference to an external/undefined symbol. Sizes are resolved from st_size,
    falling back to the gap to the next function (or end of .text) when 0."""
    if os.path.basename(src) == "patches_main.c":
        # The experiment capsule is separately closed and movable; the C unit
        # merely embeds its bytes and calls exported offsets indirectly.
        from decoder_speed.build_capsule import build_capsule
        build_capsule()
    obj = obj_path(src, ".o")
    subprocess.run([CLANG, *CFLAGS, *extra, "-I", OBJ_DIR, "-c", src, "-o", obj], check=True)

    blob, funcs, layout = link_pic_object(obj)
    _, sections = parse_elf(obj)
    by_name = {s["sname"]: s for s in sections}
    executable_end = max((off + size for name, off, size in layout
                          if by_name[name]["flags"] & SHF_EXECINSTR), default=0)
    return blob, funcs, len(blob) - executable_end


def link_pic_object(obj):
    """Close a relocatable C++ object into a movable text/constants capsule.

    No writable state, GOT, startup code or external relocations are permitted.
    ARM unwind metadata is omitted because the capsule disables exceptions.
    Return bytes, exported function offsets, and the exact section layout.
    """
    d, secs = parse_elf(obj)
    emitted = [(i, s) for i, s in enumerate(secs)
               if s["type"] == SHT_PROGBITS and s["flags"] & SHF_ALLOC
               and s["flags"] & SHF_EXECINSTR]
    emitted.sort(key=lambda item: (item[1]["sname"] != ".text", item[0]))
    emitted += [(i, s) for i, s in enumerate(secs) if _is_rodata(s)]
    for s in secs:
        if not s["size"] or not s["flags"] & SHF_ALLOC:
            continue
        if (s["flags"] & SHF_WRITE or s["sname"].startswith(
                (".got", ".init", ".fini", ".ctors", ".dtors"))):
            raise BuildError(f"{obj}: forbidden allocated section {s['sname']}")
        if not any(s is sec for _, sec in emitted) and not s["sname"].startswith(
                (".ARM.exidx", ".ARM.extab")):
            raise BuildError(f"{obj}: unknown allocated section {s['sname']}")
    blob, base = bytearray(), {}
    for i, s in emitted:
        align = max(1, s["align"])
        if align & (align - 1):
            raise BuildError("invalid section alignment")
        blob.extend(b"\0" * ((-len(blob)) % align))
        base[i] = len(blob)
        blob.extend(d[s["offset"]:s["offset"] + s["size"]])
    tab = section(secs, ".symtab")
    strings = secs[tab["link"]]
    syms = []
    for offset in range(tab["offset"], tab["offset"] + tab["size"], 16):
        name, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", d, offset)
        nm = d[strings["offset"] + name:].split(b"\0", 1)[0].decode()
        syms.append(dict(name=nm, value=value, size=size, typ=info & 15, shndx=shndx))
    for rs in secs:
        if rs["info"] not in base or rs["type"] not in (4, SHT_REL):
            continue
        if rs["type"] != SHT_REL or rs["entsize"] != 8:
            raise BuildError("capsules require ARM ELF REL relocations")
        for offset in range(rs["offset"], rs["offset"] + rs["size"], 8):
            site, info = struct.unpack_from("<II", d, offset)
            kind, sym = info & 255, syms[info >> 8]
            if sym["shndx"] not in base:
                raise BuildError(f"{obj}: unresolved {sym['name']} at {site:#x}")
            if site > secs[rs["info"]]["size"] - 4:
                raise BuildError("relocation outside its section")
            p = base[rs["info"]] + site
            s = base[sym["shndx"]] + sym["value"]
            if kind in (R_ARM_THM_CALL, R_ARM_THM_JUMP24):
                hw1, hw2 = struct.unpack_from("<HH", blob, p)
                sign = (hw1 >> 10) & 1
                i1, i2 = 1 ^ ((hw2 >> 13) & 1) ^ sign, 1 ^ ((hw2 >> 11) & 1) ^ sign
                addend = ((sign << 24) | (i1 << 23) | (i2 << 22)
                          | ((hw1 & 1023) << 12) | ((hw2 & 2047) << 1))
                if sign:
                    addend -= 1 << 25
                resolve_thumb_branch(blob, p, (s & ~1) + addend + 4)
            elif kind == 3:  # R_ARM_REL32, including Thumb function addresses.
                addend, = struct.unpack_from("<I", blob, p)
                struct.pack_into("<I", blob, p, (s + addend - p) & 0xffffffff)
            elif kind in (R_ARM_THM_MOVW_PREL_NC, R_ARM_THM_MOVT_PREL):
                resolve_movwt(blob, p, s, kind == R_ARM_THM_MOVT_PREL)
            else:
                raise BuildError(f"{obj}: unsupported capsule relocation {kind}")
    funcs = [(s["name"], base[s["shndx"]] + (s["value"] & ~1), s["size"])
             for s in syms if s["typ"] == 2 and s["shndx"] in base]
    layout = [(s["sname"], base[i], s["size"]) for i, s in emitted]
    return bytes(blob), funcs, layout

def build_dict(src, extra=()):
    blob, funcs, rodata_len = compile_text(src, extra)
    return {
        "src": src,
        "text_len": len(blob),
        "rodata_len": rodata_len,
        "text": blob.hex(),
        "functions": [
            {"name": nm, "offset": val, "size": sz, "bytes": blob[val:val + sz].hex()}
            for nm, val, sz in funcs
        ],
    }

def main():
    args = sys.argv[1:]
    as_json = "--json" in args
    args = [a for a in args if a != "--json"]
    extra = [a for a in args if a.startswith("-")]   # e.g. -DFOO=0x1234
    src = next(a for a in args if not a.startswith("-"))

    try:
        if as_json:
            print(json.dumps(build_dict(src, extra)))
            return
        blob, funcs, rodata_len = compile_text(src, extra)
    except BuildError as e:
        print("FAIL:", e)
        sys.exit(1)

    text_len = len(blob) - rodata_len
    rod = f", +{rodata_len} B rodata" if rodata_len else ""
    print(f"OK: {os.path.basename(src)} blob = {len(blob)} bytes (.text {text_len}{rod}), "
          f"relocs resolved, no external refs\n")
    for nm, val, sz in sorted(funcs, key=lambda x: x[1]):
        b = blob[val:val + sz]
        print(f"== {nm}  (text+{val:#x}, {sz} bytes) ==")
        print("bytes:", b.hex())
        print()

    text_bin = obj_path(src, ".text.bin")
    open(text_bin, "wb").write(blob)
    print(f"wrote {text_bin} ({len(blob)} bytes)")

if __name__ == "__main__":
    main()
