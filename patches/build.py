#!/usr/bin/env python3
"""
Compile a C source to position-independent Thumb-2 machine code for the G2
mainapp core (ARMv7E-M, Cortex-M-class), resolve internal relocations without
external calls, and emit the executable/read-only bytes.

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

Self-containedness is enforced in both modes. The mini-linker lays out .text,
other executable sections and read-only data with their ELF alignment, then
resolves internal Thumb BL/B.W, R_ARM_REL32 and MOVW/MOVT_PREL relocations.
REL32 preserves the Thumb bit in function addresses; branch addends are signed.
These fixups depend only on offsets within the blob, not its load address.

The blob must be loaded at an address satisfying its sections' alignment.
Function offsets are blob-relative; text_len covers code, constants and padding.
rodata_len covers everything after the executable sections. Callers extracting
one function must require rodata_len == 0 rather than discard required data.

Undefined symbols, writable/GOT/startup sections, absolute pointers and other
relocations are errors. Firmware entry points remain absolute-constant function
pointers. Only CANTUNWIND ARM exception metadata may be omitted: injected code
has no unwinder or startup machinery.
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

R_ARM_REL32            = 3    # S + A - P, including Thumb function addresses
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
    """Read ELF32-LE section headers; reject truncated or malformed inputs."""
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 52 or d[:6] != b"\x7fELF\x01\x01":
        raise BuildError(f"{path}: not ELF32-LE")
    (e_shoff,) = struct.unpack_from("<I", d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x2e)
    if (e_shentsize != 40 or not e_shnum or e_shstrndx >= e_shnum
            or e_shoff + e_shnum * e_shentsize > len(d)):
        raise BuildError(f"{path}: invalid section table")
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsz = \
            struct.unpack_from("<IIIIIIIIII", d, off)
        if typ != 8 and offset + size > len(d):  # NOBITS has no file payload.
            raise BuildError(f"{path}: section outside the file")
        secs.append(dict(name=name, type=typ, flags=flags, offset=offset,
                         size=size, link=link, info=info, align=align, entsize=entsz))
    shstr = secs[e_shstrndx]
    strings = d[shstr["offset"]:shstr["offset"] + shstr["size"]]
    for s in secs:
        start = s["name"]
        end = strings.find(b"\0", start)
        if start >= len(strings) or end < 0:
            raise BuildError(f"{path}: invalid section name")
        s["sname"] = strings[start:end].decode()
    return d, secs

def section(secs, name):
    for s in secs:
        if s["sname"] == name:
            return s
    return None

class BuildError(Exception):
    pass

SHT_PROGBITS = 1
SHT_RELA     = 4
SHT_REL      = 9
SHT_ARM_EXIDX = 0x70000001
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
    """Compile C with the original flags and return (blob, functions, rodata_len).

    All offsets are relative to the blob. rodata_len includes data alignment
    after its last executable section; it is zero for code-only objects.
    """
    obj = obj_path(src, ".o")
    subprocess.run([CLANG, *CFLAGS, *extra, "-c", src, "-o", obj], check=True)
    blob, funcs, layout = link_pic_object(obj)
    _, secs = parse_elf(obj)
    executable = {s["sname"] for s in secs if s["flags"] & SHF_EXECINSTR}
    executable_end = max((off + size for name, off, size in layout
                          if name in executable), default=0)
    return blob, funcs, len(blob) - executable_end


def link_pic_object(obj):
    """Resolve a closed ARM relocatable object into a movable code/data blob.

    Return bytes, (name, offset, size) functions and (section, offset, size)
    layout. No writable state, external calls or runtime loader is allowed.
    Load the blob on an address aligned for its most-aligned emitted section.
    """
    d, secs = parse_elf(obj)
    if struct.unpack_from("<HH", d, 16) != (1, 40):  # ET_REL, EM_ARM.
        raise BuildError(f"{obj}: expected an ARM relocatable object")
    emitted = [(i, s) for i, s in enumerate(secs)
               if s["type"] == SHT_PROGBITS and s["flags"] & SHF_ALLOC
               and s["flags"] & SHF_EXECINSTR]
    emitted.sort(key=lambda item: (item[1]["sname"] != ".text", item[0]))
    emitted += [(i, s) for i, s in enumerate(secs) if _is_rodata(s)]
    emitted_indices = {i for i, _ in emitted}
    for i, s in enumerate(secs):
        if not s["size"] or not s["flags"] & SHF_ALLOC:
            continue
        if (s["flags"] & SHF_WRITE or s["sname"].startswith(
                (".got", ".init", ".fini", ".ctors", ".dtors", ".ARM.extab"))):
            raise BuildError(f"{obj}: forbidden allocated section {s['sname']}")
        if s["sname"].startswith(".ARM.exidx"):
            # Clang emits these even with -fno-unwind-tables. Dropping an
            # actual unwind recipe would silently produce incomplete code.
            if s["type"] != SHT_ARM_EXIDX or s["size"] % 8:
                raise BuildError(f"{obj}: invalid ARM exception index")
            for p in range(s["offset"] + 4, s["offset"] + s["size"], 8):
                if struct.unpack_from("<I", d, p)[0] != 1:  # EXIDX_CANTUNWIND.
                    raise BuildError(f"{obj}: unwind tables are unsupported")
        elif i not in emitted_indices:
            raise BuildError(f"{obj}: unknown allocated section {s['sname']}")
    blob, base = bytearray(), {}
    for i, s in emitted:
        align = max(1, s["align"])
        if align & (align - 1):
            raise BuildError(f"{obj}: invalid section alignment")
        blob.extend(b"\0" * ((-len(blob)) % align))
        base[i] = len(blob)
        blob.extend(d[s["offset"]:s["offset"] + s["size"]])

    tab = section(secs, ".symtab")
    if (tab is None or tab["entsize"] != 16 or tab["size"] % 16
            or tab["link"] >= len(secs)):
        raise BuildError(f"{obj}: invalid symbol table")
    strings = secs[tab["link"]]
    names = d[strings["offset"]:strings["offset"] + strings["size"]]
    syms = []
    for p in range(tab["offset"], tab["offset"] + tab["size"], 16):
        name, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", d, p)
        end = names.find(b"\0", name)
        if name >= len(names) or end < 0:
            raise BuildError(f"{obj}: invalid symbol name")
        nm = names[name:end].decode()
        if nm and shndx == 0:
            raise BuildError(f"{obj}: undefined symbol {nm!r}")
        if shndx in base:
            offset = value & ~1 if info & 15 == 2 else value
            if offset + size > secs[shndx]["size"]:
                raise BuildError(f"{obj}: symbol outside its section: {nm!r}")
        syms.append(dict(name=nm, value=value, size=size,
                         typ=info & 15, shndx=shndx))

    for rs in secs:
        if rs["type"] not in (SHT_REL, SHT_RELA) or not rs["size"]:
            continue
        if rs["info"] >= len(secs):
            raise BuildError(f"{obj}: invalid relocation section")
        if rs["info"] not in base:  # Nonloaded debug/CANTUNWIND metadata.
            continue
        if (rs["type"] != SHT_REL or rs["entsize"] != 8
                or rs["size"] % 8 or rs["link"] != secs.index(tab)):
            raise BuildError(f"{obj}: expected ARM ELF REL relocations")
        for p in range(rs["offset"], rs["offset"] + rs["size"], 8):
            site, info = struct.unpack_from("<II", d, p)
            if info >> 8 >= len(syms):
                raise BuildError(f"{obj}: invalid relocation symbol")
            kind, sym = info & 255, syms[info >> 8]
            if sym["shndx"] not in base:
                raise BuildError(f"{obj}: relocation to nonloaded {sym['name']!r}")
            if site > secs[rs["info"]]["size"] - 4:
                raise BuildError(f"{obj}: relocation outside its section")
            place = base[rs["info"]] + site
            target = base[sym["shndx"]] + sym["value"]
            if kind in (R_ARM_THM_CALL, R_ARM_THM_JUMP24):
                if not secs[sym["shndx"]]["flags"] & SHF_EXECINSTR:
                    raise BuildError(f"{obj}: branch target is not executable")
                hw1, hw2 = struct.unpack_from("<HH", blob, place)
                opcode = 0xD000 if kind == R_ARM_THM_CALL else 0x9000
                if hw1 & 0xF800 != 0xF000 or hw2 & 0xD000 != opcode:
                    raise BuildError(f"{obj}: unsupported Thumb branch opcode")
                sign = (hw1 >> 10) & 1
                i1 = 1 ^ ((hw2 >> 13) & 1) ^ sign
                i2 = 1 ^ ((hw2 >> 11) & 1) ^ sign
                addend = ((sign << 24) | (i1 << 23) | (i2 << 22)
                          | ((hw1 & 1023) << 12) | ((hw2 & 2047) << 1))
                if sign:
                    addend -= 1 << 25
                # ELF stores S + A - P; the encoder takes a target for PC=P+4.
                resolve_thumb_branch(blob, place, (target & ~1) + addend + 4)
            elif kind == R_ARM_REL32:
                addend, = struct.unpack_from("<I", blob, place)
                struct.pack_into("<I", blob, place,
                                 (target + addend - place) & 0xFFFFFFFF)
            elif kind in (R_ARM_THM_MOVW_PREL_NC, R_ARM_THM_MOVT_PREL):
                resolve_movwt(blob, place, target, high=(kind == R_ARM_THM_MOVT_PREL))
            else:
                raise BuildError(f"{obj}: unsupported relocation {kind} to {sym['name']!r}")

    raw_funcs = sorted((s["name"], base[s["shndx"]] + (s["value"] & ~1),
                        s["size"], s["shndx"])
                       for s in syms if s["typ"] == 2 and s["shndx"] in base)
    raw_funcs.sort(key=lambda f: f[1])
    funcs = []
    for i, (name, offset, size, shndx) in enumerate(raw_funcs):
        if not secs[shndx]["flags"] & SHF_EXECINSTR:
            raise BuildError(f"{obj}: function outside executable sections")
        if not size:
            end = base[shndx] + secs[shndx]["size"]
            following = [f[1] for f in raw_funcs[i + 1:]
                         if f[3] == shndx and f[1] > offset]
            size = min(following, default=end) - offset
        funcs.append((name, offset, size))
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
