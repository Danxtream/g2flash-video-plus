"""Build hot and cold decoder modules independently of the firmware image.

SPDX-License-Identifier: GPL-3.0-only
Run in Ubuntu from the repository root with --output <output-directory>.
The decoder flags and configuration are those used for the glasses speed tests.
The firmware build does not invoke this tool yet.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXPORTS = ("g2_h264_size", "g2_h264_alignment", "g2_h264_init",
           "g2_h264_destroy", "g2_h264_decode", "g2_h264_frame")


def load_module(name, path):
    """Import a sibling tool without altering the caller's module search path."""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


toolchain = load_module("h264_toolchain", HERE / "toolchain.py")
partition_ir = load_module("h264_partition", HERE / "partition_ir.py")
linker = load_module("h264_linker", ROOT / "patches/build.py")


def load_selection(path=HERE / "hot_functions.json"):
    """Validate the frozen promotion list and explicit adapter-only mapping."""
    config = json.loads(Path(path).read_text(encoding="utf-8"))
    if config.get("schema") != 1:
        raise ValueError("unsupported decoder selection schema")
    for key in ("hot_groups", "promotions"):
        items = config.get(key)
        if not isinstance(items, list) or not items or not all(isinstance(s, str) and s for s in items):
            raise ValueError("invalid " + key)
        if len(set(items)) != len(items):
            raise ValueError("duplicate " + key)
    if len(config["promotions"]) != 229:
        raise ValueError("decoder selection requires the original 229 promotion symbols")
    mapping = config.get("external_promotions")
    if not isinstance(mapping, dict) or not set(mapping) <= set(config["promotions"]):
        raise ValueError("invalid external promotion mapping")
    if not all(isinstance(v, str) and v for v in mapping.values()):
        raise ValueError("invalid external promotion target")
    return config


def canonical_native_type(name):
    """Normalize LP64 size/array spelling for unique ARM/native test matches."""
    replacements = {"unsigned long": "unsigned int", "long": "int"}
    name = re.sub(r"\b(?:unsigned long long|long long|unsigned long|long)\b",
                  lambda m: replacements.get(m.group(), m.group()), name)
    return re.sub(r"(\d+)ul\b", r"\1u", name)


def resolve_promotions(ir, config, native=False):
    """Resolve every frozen symbol once; never guess an ambiguous native match."""
    names = set(partition_ir.definitions(ir))
    declarations = {partition_ir.symbol(line) for line in ir.splitlines()
                    if line.startswith("declare ")}
    lookup = {}
    if native:
        all_names = sorted(names | set(config["promotions"]))
        output = subprocess.check_output(["c++filt", "-n"], input="\n".join(all_names) + "\n", text=True)
        demangled = output.splitlines()
        if len(demangled) != len(all_names):
            raise ValueError("demangler changed the symbol count")
        texts = dict(zip(all_names, demangled))
        for name in names:
            lookup.setdefault(canonical_native_type(texts[name]), []).append(name)
    mapping, external = {}, {}
    for name in config["promotions"]:
        if name in config["external_promotions"]:
            target = config["external_promotions"][name]
            if name in names or target not in declarations or target in names:
                raise ValueError("external promotion must resolve to a declared provider: " + name)
            external[name] = target
            continue
        if name in names:
            matches = [name]
        elif native:
            matches = lookup.get(canonical_native_type(texts[name]), [])
        else:
            matches = []
        if len(matches) != 1:
            raise ValueError(f"promotion must map uniquely: {name}: {matches}")
        mapping[name] = matches[0]
    if len(set(mapping.values())) != len(mapping):
        raise ValueError("two promotions map to the same definition")
    return mapping, external


def compile_modules(output, flags, source=HERE / "g2_h264.cpp", native=False,
                    object_flags=(), selection=None):
    """Emit preopt IR, single-owner modules, objects and full membership."""
    directory = Path(output).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if [flag for flag in flags if re.fullmatch(r"-O[0-3szg]", flag)] != ["-O2"]:
        raise ValueError("partition input must use exactly one -O2 frontend flag")
    if any(flag.startswith("-flto") for flag in flags):
        raise ValueError("cross-module LTO is forbidden")
    config = load_selection() if selection is None else selection
    commands = []
    def run(command):
        commands.append([str(s) for s in command])
        subprocess.run(command, check=True)
    ir_flags = [flag for flag in flags if flag != "-fstack-usage"]
    preopt = directory / "preopt.ll"
    run(["clang++", *ir_flags, "-Xclang", "-disable-llvm-passes", "-S", "-emit-llvm",
         str(source), "-o", str(preopt)])
    ir = preopt.read_text(encoding="utf-8")
    mapping, external = resolve_promotions(ir, config, native)
    hot, cold, membership = partition_ir.partition(ir, config["hot_groups"], sorted(mapping.values()))
    objects = []
    for name, text, optimization in (("hot", hot, "-O2"), ("cold", cold, "-Oz")):
        path = directory / (name + ".ll")
        path.write_text(text, encoding="utf-8")
        compile_flags = [optimization if flag == "-O2" else flag for flag in ir_flags]
        obj = directory / (name + ".o")
        run(["clang++", *compile_flags, *object_flags, "-ffunction-sections",
             "-fdata-sections", "-fstack-usage", "-c", str(path), "-o", str(obj)])
        objects.append(obj)
    selection_bytes = json.dumps(config, sort_keys=True, separators=(",", ":")).encode("utf-8")
    membership.update({"promotion_mapping": mapping, "external_promotions": external,
                       "selection_sha256": hashlib.sha256(selection_bytes).hexdigest(),
                       "preopt_sha256": hashlib.sha256(preopt.read_bytes()).hexdigest()})
    (directory / "membership.json").write_text(json.dumps(membership, indent=2) + "\n", encoding="utf-8")
    (directory / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    return objects, membership


def build_decoder(output, headers=None):
    """Close the inert C ABI with existing memory helpers; do not edit firmware."""
    directory = Path(output).resolve()
    headers = toolchain.discover_headers() if headers is None else headers
    flags = toolchain.compiler_flags(headers)
    objects, membership = compile_modules(directory, flags)
    for name, source in (("runtime", HERE / "runtime.c"), ("memory", ROOT / "patches/memory.c")):
        obj = directory / (name + ".o")
        subprocess.run(["clang", *linker.CFLAGS, "-ffunction-sections", "-fdata-sections",
                        "-c", str(source), "-o", str(obj)], check=True)
        objects.append(obj)
    closed = directory / "closed.o"
    roots = [arg for name in EXPORTS for arg in ("-u", name)]
    subprocess.run(["ld.lld", "-r", "--gc-sections", *roots, *map(str, objects),
                    "-o", str(closed)], check=True)
    blob, functions, layout = linker.link_pic_object(closed)
    exports = {name: offset for name, offset, size in functions if name in EXPORTS}
    if set(exports) != set(EXPORTS):
        raise ValueError("missing C decoder export")
    code = sum(size for name, offset, size in layout if name.startswith(".text"))
    constants = sum(size for name, offset, size in layout if not name.startswith(".text"))
    manifest = {"schema": 1, "name": "h264-decoder", "bytes": len(blob),
                "sha256": hashlib.sha256(blob).hexdigest(), "exports": exports,
                "code_bytes": code, "constant_bytes": constants,
                "padding_bytes": len(blob) - code - constants, "layout": layout,
                "flags": flags, "c_flags": linker.CFLAGS,
                "selection_sha256": membership["selection_sha256"],
                "compiler": subprocess.check_output(["clang++", "--version"], text=True).splitlines()[0]}
    (directory / "decoder.bin").write_bytes(blob)
    (directory / "decoder.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--arm-gxx", default="arm-none-eabi-g++")
    parser.add_argument("--cxx-include-root", type=Path)
    parser.add_argument("--c-include-root", type=Path)
    args = parser.parse_args()
    headers = toolchain.discover_headers(args.arm_gxx, args.cxx_include_root, args.c_include_root)
    result = build_decoder(args.output, headers)
    print(f"H.264 decoder: {result['bytes']} B, SHA256 {result['sha256']}")


if __name__ == "__main__":
    main()
