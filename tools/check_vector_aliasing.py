#!/usr/bin/env python3
"""Find translated PowerPC instructions whose generated C++ reads a vector register's bytes after
it has already overwritten them (#3: vpkd3d128 v0,v0 lost the sign this way).

Each instruction's translation starts with a `// mnemonic operands` comment. Within one
instruction, the source operands must be read before the destination is written, so a read of
bytes the same instruction already wrote is a hazard (the destination aliases a source).

    python tools/check_vector_aliasing.py port-yeti/generated/yeti_final_xenon
"""

import collections
import glob
import os
import re
import sys

LANES = {'u8': 1, 's8': 1, 'u16': 2, 's16': 2, 'u32': 4, 's32': 4, 'f32': 4, 'u64': 8, 's64': 8,
         'f64': 8}
# ctx.v12.u32[3] / ctx.v12.f32[0] ...
ELEM = re.compile(r'ctx\.(v\d+)\.(u8|s8|u16|s16|u32|s32|f32|u64|s64|f64)\[(\d+)\]')
# a whole-register use: ctx.v12.u8 (as a pointer for simde loads) or `ctx.v12` alone
WHOLE = re.compile(r'ctx\.(v\d+)(?:\.u8)?(?![\w.\[])')
WRITE = re.compile(r'^\s*(ctx\.v\d+\.(?:u8|s8|u16|s16|u32|s32|f32|u64|s64|f64)\[\d+\])\s*(=|\|=|&=|\^=|\+=|-=)(?!=)')
STORE = re.compile(r'simde_mm_store\w*\(\(simde__m128i?\*\)ctx\.(v\d+)\.u8')
WHOLE_ASSIGN = re.compile(r'^\s*ctx\.(v\d+)\s*=(?!=)')


def byte_range(kind, index):
    size = LANES[kind]
    return set(range(index * size, index * size + size))


def check_block(mnemonic, lines):
    written = collections.defaultdict(set)  # register -> bytes written so far
    hits = []
    for line in lines:
        code = line.split('//')[0]
        if not code.strip():
            continue
        target = None
        m = WRITE.match(code)
        rhs = code
        if m:
            target = m.group(1)
            rhs = code[m.end():]
            # A compound assignment (|=) reads its own target: that's the value this
            # instruction just built, not a source, so it isn't checked.
        store = STORE.search(code)
        if store:
            rhs = code[store.end():]
        whole = WHOLE_ASSIGN.match(code)
        if whole:
            rhs = code[whole.end():]
        # reads
        for reg, kind, idx in ELEM.findall(rhs):
            hit = written[reg] & byte_range(kind, int(idx))
            if hit:
                hits.append((reg, f'{kind}[{idx}]', line.strip()))
        for reg in WHOLE.findall(rhs):
            if written[reg]:
                hits.append((reg, 'whole', line.strip()))
        # writes
        if m:
            reg, kind, idx = ELEM.match(target).groups()
            written[reg] |= byte_range(kind, int(idx))
        if store:
            written[store.group(1)] |= set(range(16))
        if whole:
            written[whole.group(1)] |= set(range(16))
    return hits


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'port-yeti/generated/yeti_final_xenon'
    by_mnemonic = collections.Counter()
    checked = collections.Counter()
    examples = {}
    for path in sorted(glob.glob(os.path.join(root, '*_recomp.*.cpp'))):
        block, mnemonic = [], None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                s = line.strip()
                if s.startswith('// ') and re.match(r'// [a-z][\w.]*( |$)', s):
                    if mnemonic and block:
                        if 'ctx.v' in ''.join(block):
                            checked[mnemonic.split()[0]] += 1
                        for hit in check_block(mnemonic, block):
                            key = mnemonic.split()[0]
                            by_mnemonic[key] += 1
                            examples.setdefault(key, (path, mnemonic, hit))
                    mnemonic, block = s[3:], []
                elif mnemonic is not None:
                    block.append(line)
    for key, count in by_mnemonic.most_common():
        path, mnemonic, (reg, lane, line) = examples[key]
        print(f'{count:6} {key:12} e.g. {mnemonic}  ({reg} {lane}): {line[:110]}')
    if not by_mnemonic:
        print('no read-after-write hazards found')
    print(f'checked {sum(checked.values())} vector instructions ({len(checked)} mnemonics)')


if __name__ == '__main__':
    main()
