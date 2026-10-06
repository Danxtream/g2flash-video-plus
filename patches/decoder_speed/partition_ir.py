"""Partition Clang's pre-optimization IR, with one owner per definition.

This deliberately accepts a narrow, checked subset of Clang IR. Constants
belong to the cold module; hidden external linkage lets hot code reference
them without a GOT. Cross-module functions cannot be inlined (no LTO).
"""
import re

HOT_GROUPS = ('lumaMotionComp', 'intraPred4x4', 'intraPred16x16', 'intraPred8x8Luma',
              'inverseDct', 'inverseHadamard', 'inverseQuantize', 'dequantize',
              'dequantLumaDcValues', 'deblockMb', 'filterLuma', 'computeBs')
SYMBOL = r'@(?:"[^"\n]+"|[-a-zA-Z$._0-9]+)'
LINKAGE = r'\b(?:private|internal|linkonce_odr|weak_odr|dso_local|hidden)(?:\s+|$)'


def symbol(line):
    match = re.search(SYMBOL, line)
    if not match:
        raise ValueError('missing IR symbol')
    return match.group()[1:]


def constant_type(text):
    """Read one scalar/named/aggregate type, without reading its initializer."""
    if text[0] in '[{<':
        pairs = {'[': ']', '{': '}', '<': '>'}
        stack, quoted = [], False
        for i, char in enumerate(text):
            if char == '"':
                quoted = not quoted
            if quoted:
                continue
            if char in pairs:
                stack.append(pairs[char])
            elif char in ']}>':
                if not stack or stack.pop() != char:
                    raise ValueError('unbalanced global type')
                if not stack:
                    return text[:i+1]
        raise ValueError('unterminated global type')
    match = re.match(r'(?:%"[^"]+"|%[-\w.$]+|i\d+|ptr|float|double)\s', text)
    if not match:
        raise ValueError('unsupported global type')
    return match.group().rstrip()


def partition(ir, groups=HOT_GROUPS, promotions=()):
    """Return hot/cold IR plus an auditable membership manifest; fail closed."""
    if any(token in ir for token in ('@llvm.global_ctors', '@llvm.global_dtors',
                                    ' blockaddress(', ' alias ', ' ifunc ', 'thread_local')):
        raise ValueError('unsupported initializer, alias or thread-local IR')
    def selected(name):
        return name in promotions or ('sub0h264' in name and any(group in name for group in groups))
    hot, cold, names, constants = [], [], {}, []
    lines = ir.splitlines(keepends=True)
    references = {'hot': set(), 'cold': set()}
    owner = None
    for line in lines:
        if line.startswith('define '):
            name = symbol(line)
            owner = 'hot' if selected(name) else 'cold'
        elif line == '}\n':
            owner = None
        elif owner:
            references[owner].update(m.group()[1:] for m in re.finditer(SYMBOL, line))
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('define '):
            name = symbol(line)
            if name in names or not line.endswith(' {\n') or '!dbg' in line:
                raise ValueError('duplicate, multiline or debug function header')
            is_hot = selected(name)
            names[name] = 'hot' if is_hot else 'cold'
            header = 'define dso_local hidden ' + re.sub(LINKAGE, '', line[len('define '):])
            header = re.sub(r' comdat(?:\([^)]*\))?', '', header)
            # Parameter align attributes belong to the signature. Only the
            # trailing function alignment is forbidden on declarations.
            declaration = 'declare ' + header[len('define '):].removesuffix(' {\n') + '\n'
            declaration = re.sub(r'(\)\s*(?:unnamed_addr\s*)?#\d+) align \d+', r'\1', declaration)
            # Preserve the original discardable/local linkage when no other
            # module needs this symbol. Promoting every inline helper prevents
            # Clang from eliminating otherwise unused out-of-line copies.
            opposite = 'cold' if is_hot else 'hot'
            body = [header if name in references[opposite] else line]
            i += 1
            while i < len(lines) and lines[i] != '}\n':
                if lines[i].startswith('define '):
                    raise ValueError('unterminated function')
                body.append(lines[i]); i += 1
            if i == len(lines):
                raise ValueError('unterminated function')
            body.append(lines[i])
            hot.extend(body if is_hot else [declaration])
            cold.extend([declaration] if is_hot else body)
        elif line.startswith('@'):
            if ' external ' in line:
                hot.append(line); cold.append(line)
            else:
                if ' constant ' not in line or ' global ' in line:
                    raise ValueError('writable or unsupported global')
                name = symbol(line)
                if name in constants:
                    raise ValueError('duplicate global')
                constants.append(name)
                prefix, value = line.split(' constant ', 1)
                attributes = re.sub(LINKAGE, '', prefix.split('=', 1)[1]).strip()
                if attributes not in ('', 'unnamed_addr', 'local_unnamed_addr'):
                    raise ValueError('unsupported constant attributes')
                definition = '@' + name + ' = dso_local hidden ' + attributes + ' constant ' + value
                definition = re.sub(r',? comdat(?:\([^)]*\))?', '', definition)
                cold.append(definition if name in references['hot'] else line)
                hot.append('@' + name + ' = external dso_local hidden constant ' + constant_type(value) + '\n')
        else:
            if '!dbg' in line or line.startswith('!llvm.dbg'):
                raise ValueError('debug metadata not supported; emit IR without stack usage/debug')
            hot.append(line); cold.append(line)
        i += 1
    if set(promotions)-set(names):
        raise ValueError('promotion matched no definition')
    if not any(owner == 'hot' for owner in names.values()):
        raise ValueError('hot selection matched no definitions')
    cold_ir = ''.join(cold)
    # Clang's -Oz frontend attaches both attributes. IR-input compilation
    # keeps the input attributes; the driver flag alone cannot add them.
    cold_ir = re.sub(r'^(attributes #\d+ = \{ )', r'\1minsize optsize ', cold_ir, flags=re.MULTILINE)
    return ''.join(hot), cold_ir, dict(functions=names, constants=constants, groups=list(groups), promotions=list(promotions))
