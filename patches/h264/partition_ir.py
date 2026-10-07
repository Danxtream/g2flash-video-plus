"""Split Clang's pre-optimization IR with one owner per definition.

SPDX-License-Identifier: GPL-3.0-only
Read-only tables belong to the cold module. The hot module receives declarations
only. Functions referenced across the split use hidden external linkage; no LTO
is used, so the compiler cannot inline across module boundaries.
"""
import re

SYMBOL = r'@(?:"[^"\n]+"|[-a-zA-Z$._0-9]+)'
LINKAGE = r'\b(?:private|internal|linkonce_odr|weak_odr|dso_local|hidden)(?:\s+|$)'


def symbol(line):
    """Extract a plain or quoted LLVM symbol, retaining its quoting."""
    match = re.search(SYMBOL, line)
    if not match:
        raise ValueError("missing IR symbol")
    return match.group()[1:]


def constant_type(text):
    """Read a scalar/named/aggregate type without consuming its initializer."""
    if not text:
        raise ValueError("missing global type")
    if text[0] in "[{<":
        pairs = {"[": "]", "{": "}", "<": ">"}
        stack, quoted = [], False
        for index, char in enumerate(text):
            if char == '"':
                quoted = not quoted
            if quoted:
                continue
            if char in pairs:
                stack.append(pairs[char])
            elif char in "]}>":
                if not stack or stack.pop() != char:
                    raise ValueError("unbalanced global type")
                if not stack:
                    return text[:index + 1]
        raise ValueError("unterminated global type")
    match = re.match(r'(?:%"[^"]+"|%[-\w.$]+|i\d+|ptr|float|double)\s', text)
    if not match:
        raise ValueError("unsupported global type")
    return match.group().rstrip()


def definitions(ir):
    """Collect checked single-line function headers and complete bodies."""
    if not ir.endswith("\n"):
        raise ValueError("IR must end with a newline")
    result, body, name = {}, [], None
    for line in ir.splitlines(keepends=True):
        if line.startswith("define "):
            if name is not None:
                raise ValueError("unterminated function")
            name = symbol(line)
            if name in result or not line.endswith(" {\n") or "!dbg" in line:
                raise ValueError("duplicate, multiline or debug function header")
            body = [line]
        elif name is not None:
            body.append(line)
            if line == "}\n":
                result[name] = "".join(body)
                name = None
    if name is not None:
        raise ValueError("unterminated function")
    return result


def partition(ir, groups, promotions):
    """Return hot/cold IR and complete membership; reject unsupported input."""
    forbidden = ("@llvm.global_ctors", "@llvm.global_dtors", " blockaddress(",
                 " alias ", " ifunc ", "thread_local", "!dbg", "!llvm.dbg")
    if any(token in ir for token in forbidden):
        raise ValueError("unsupported initializer, alias, thread-local or debug IR")
    if len(set(promotions)) != len(promotions):
        raise ValueError("duplicate promotion")
    bodies = definitions(ir)
    missing = set(promotions) - bodies.keys()
    if missing:
        raise ValueError("promotion matched no definition: " + ", ".join(sorted(missing)))
    for group in groups:
        if not any("sub0h264" in name and group in name for name in bodies):
            raise ValueError("hot group matched no definition: " + group)
    owners = {name: "hot" if name in promotions or
              ("sub0h264" in name and any(group in name for group in groups))
              else "cold" for name in bodies}
    if "hot" not in owners.values():
        raise ValueError("hot selection matched no definitions")
    references = {"hot": set(), "cold": set()}
    for name, body in bodies.items():
        references[owners[name]].update(m.group()[1:] for m in re.finditer(SYMBOL, body))
    hot, cold, constants = [], [], []
    lines, index = ir.splitlines(keepends=True), 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("define "):
            name = symbol(line)
            is_hot = owners[name] == "hot"
            header = "define dso_local hidden " + re.sub(LINKAGE, "", line[len("define "):])
            header = re.sub(r" comdat(?:\([^)]*\))?", "", header)
            declaration = "declare " + header[len("define "):].removesuffix(" {\n") + "\n"
            # Parameter alignment belongs to the signature; function alignment
            # belongs only to a definition, not its opposite-module declaration.
            declaration = re.sub(r"(\)\s*(?:unnamed_addr\s*)?#\d+) align \d+", r"\1", declaration)
            opposite = "cold" if is_hot else "hot"
            body = [header if name in references[opposite] else line]
            index += 1
            while lines[index] != "}\n":
                body.append(lines[index])
                index += 1
            body.append(lines[index])
            hot.extend(body if is_hot else [declaration])
            cold.extend([declaration] if is_hot else body)
        elif line.startswith("@"):
            if " global " in line:
                raise ValueError("writable global is forbidden")
            if " external " in line:
                if " constant " not in line:
                    raise ValueError("unsupported external global")
                hot.append(line)
                cold.append(line)
            else:
                if " constant " not in line:
                    raise ValueError("unsupported global")
                name = symbol(line)
                if name in constants:
                    raise ValueError("duplicate global")
                constants.append(name)
                prefix, value = line.split(" constant ", 1)
                attributes = re.sub(LINKAGE, "", prefix.split("=", 1)[1]).strip()
                if attributes not in ("", "unnamed_addr", "local_unnamed_addr"):
                    raise ValueError("unsupported constant attributes")
                definition = "@" + name + " = dso_local hidden " + attributes + " constant " + value
                definition = re.sub(r",? comdat(?:\([^)]*\))?", "", definition)
                cold.append(definition if name in references["hot"] else line)
                hot.append("@" + name + " = external dso_local hidden constant " + constant_type(value) + "\n")
        else:
            hot.append(line)
            cold.append(line)
        index += 1
    # An IR-input -Oz command alone does not attach the frontend size attributes.
    # Apply them to cold attribute groups exactly as Clang's -Oz frontend does.
    cold_ir = re.sub(r"^(attributes #\d+ = \{ )", r"\1minsize optsize ",
                     "".join(cold), flags=re.MULTILINE)
    return "".join(hot), cold_ir, {
        "functions": owners, "constants": constants,
        "groups": list(groups), "promotions": list(promotions),
        "constant_policy": "cold owner; hot external declarations only",
    }
