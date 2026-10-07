"""Expose read-only values to the hot optimizer without emitting table copies.

SPDX-License-Identifier: GPL-3.0-only
The cold module retains its definitions. Only hot declarations become
available_externally initializers, which LLVM may fold but must not emit.
"""
import re

GLOBAL = r'@(?:"[^"\n]+"|[-a-zA-Z$._0-9]+)'
LINKAGE = r'\b(?:private|internal|linkonce_odr|weak_odr|dso_local|hidden)(?:\s+|$)'


def expose(hot, cold, constants):
    """Replace checked hot declarations with optimizer-only cold initializers."""
    expected = set(constants)
    if len(expected) != len(constants):
        raise ValueError("duplicate constant selection")
    definitions = {}
    for line in cold.splitlines(keepends=True):
        if not line.startswith("@"):
            continue
        match = re.fullmatch("(" + GLOBAL + r") = (.*?) constant (.*)\n", line)
        if not match:
            raise ValueError("unsupported cold global")
        name, prefix, value = match.groups()
        if "external" in prefix.split():
            continue
        if name[1:] not in expected or name in definitions:
            raise ValueError("unexpected or duplicate cold constant: " + name)
        attributes = re.sub(LINKAGE, "", prefix).strip()
        if attributes not in ("", "unnamed_addr", "local_unnamed_addr"):
            raise ValueError("unsupported cold constant attributes: " + name)
        definitions[name] = attributes, value
    if {name[1:] for name in definitions} != expected:
        raise ValueError("missing cold constant initializer")
    replaced, seen = [], set()
    for line in hot.splitlines(keepends=True):
        match = re.match("(" + GLOBAL + r") = ", line) if line.startswith("@") else None
        name = match[1] if match else None
        if name not in definitions:
            replaced.append(line)
            continue
        declaration = name + " = external dso_local hidden constant "
        if not line.startswith(declaration) or not line.endswith("\n") or name in seen:
            raise ValueError("expected one hot constant declaration: " + name)
        attributes, value = definitions[name]
        if not value.startswith(line[len(declaration):].rstrip("\n") + " "):
            raise ValueError("constant type mismatch: " + name)
        # Optimizer-only definitions cannot be COMDAT members. Preserve the
        # cold owner's COMDAT and all initializer values and other attributes.
        value = re.sub(r",? comdat(?:\([^)]*\))?", "", value)
        replaced.append(name + " = available_externally dso_local hidden "
                        + attributes + " constant " + value + "\n")
        seen.add(name)
    if seen != definitions.keys():
        raise ValueError("missing hot constant declaration")
    return "".join(replaced)
