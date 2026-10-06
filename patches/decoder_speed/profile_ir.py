"""Add bounded function hooks after optimization, without changing decoder sources."""
import re
from .partition_ir import symbol


def function_names(ir):
    return [symbol(line) for line in ir.splitlines() if line.startswith('define ')]


def instrument(ir, identifiers):
    """Instrument emitted functions only; reject exceptional exits or musttail."""
    lines = ir.splitlines(keepends=True)
    out = []
    current = None
    entered = False
    for line in lines:
        if line.startswith('attributes '):
            # Added hooks access controller state through callbacks. Keeping
            # the old pure/readonly contracts could let later passes erase them.
            line=re.sub(r'\b(?:readnone|readonly|writeonly|argmemonly|inaccessiblememonly|inaccessiblemem_or_argmemonly|nosync|nocallback|speculatable)\b', '', line)
            line=re.sub(r'\bmemory\([^)]*\)', '', line)
        if line.startswith('define '):
            current = identifiers.get(symbol(line))
            entered = False
        elif current is not None:
            if any(word in line for word in ('musttail ', 'invoke ', 'resume ', 'catchswitch ', 'cleanupret ')):
                raise ValueError('unsupported profiling control flow')
            # Entry labels/comments precede the first instruction. Void calls
            # do not renumber Clang's numeric SSA values or block labels.
            if not entered and line.strip() and not line.lstrip().startswith(';') and not re.match(r'^[\w.$-]+:', line):
                out.append(f'  call void @ds_profile_enter(i32 {current})\n')
                entered = True
            if line.lstrip().startswith('ret '):
                out.append(f'  call void @ds_profile_exit(i32 {current})\n')
            line = line.replace('tail call ', 'call ')
            if line == '}\n':
                current = None
        out.append(line)
    out += ['\ndeclare hidden void @ds_profile_enter(i32)\n', 'declare hidden void @ds_profile_exit(i32)\n']
    return ''.join(out)
