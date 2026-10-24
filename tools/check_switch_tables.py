"""Audit every jump table (switch) in a ReXGlue translation against the executable image.

For each `switch (ctx.rN.u32)` in the generated code, the instructions just above it (each one is
a comment in the generated code) are replayed: lis/addi/ori build the table and base addresses,
lwzx/lhzx/lbzx load the entry, rlwinm/add combine it. The targets are recomputed from the image
and compared with the `case i: goto loc_X;` lines. Each mismatch is printed with the
[[entrypoint.switch_tables]] override that fixes it.

    python -I tools/check_switch_tables.py <image.bin> port/generated/default [--toml out.toml]

Why: ReXGlue's detector (function_scanner.cpp, detectJumpTable) treats a table address whose
low 16 bits are zero as "high half only" and adds the next addi it meets further back, so a
table on a 64 KB boundary is read from the wrong place (GRAW: the HLSL parser's rule table at
0x82030000 was read from 0x8202FFFC, every case shifted by two). The image comes from
tools/xex_basic_decrypt.py.
"""
import glob
import re
import struct
import sys

BASE = 0x82000000

INSN = re.compile(r'^\s*// (\w+\.?) ?(.*)$')
SWITCH = re.compile(r'^\s*switch \(ctx\.r(\d+)\.u32\) \{')
CASE = re.compile(r'^\s*case (\d+):')
GOTO = re.compile(r'^\s*goto loc_([0-9A-F]+);')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)')
LOOKBACK = 24


def u32(x):
    return x & 0xFFFFFFFF


def regs(text):
    return [t.strip() for t in text.split(',')]


def replay(insns, image):
    """Replay the instructions before a bctr. Returns (kind, table, base, shift, index register,
    table length from the bound check or None)."""
    val = {}
    index_src = None  # the register the table load is indexed by (before scaling)
    bounds = {}       # register -> K from `cmplwi crX,rN,K`          # register -> known constant
    idx = {}          # register -> (index register, scale shift)
    loaded = {}       # register -> (kind, table address) for a value loaded from the table
    shifted = {}      # register -> (kind, table, shift) a loaded offset shifted left
    result = None
    for op, args in insns:
        a = regs(args) if args else []
        num = lambda s: int(s, 0)
        if op == 'lis':
            val[a[0]] = u32(num(a[1]) << 16)
            for d in (idx, loaded, shifted):
                d.pop(a[0], None)
        elif op in ('addi', 'ori') and len(a) == 3:
            src, imm = a[1], num(a[2])
            if op == 'addi' and src == 'r0':
                val[a[0]] = u32(imm)
            elif src in val:
                val[a[0]] = u32(val[src] + imm) if op == 'addi' else val[src] | (imm & 0xFFFF)
            else:
                val.pop(a[0], None)
        elif op == 'li':
            val[a[0]] = u32(num(a[1]))
        elif op == 'cmplwi' and len(a) == 3:
            bounds[a[1]] = num(a[2])
        elif op == 'rlwinm' and len(a) == 5:
            d, s, sh = a[0], a[1], int(a[2])
            mb, me = int(a[3]), int(a[4])
            slwi = mb == 0 and me == 31 - sh
            if s in loaded and slwi:
                kind, table = loaded.pop(s) if s == d else loaded[s]
                shifted[d] = (kind, table, sh)
            elif slwi and s not in val:
                idx[d] = (s, sh)
                loaded.pop(d, None)
                shifted.pop(d, None)
            val.pop(d, None)
        elif op in ('lwzx', 'lhzx', 'lbzx') and len(a) == 3:
            d, ra, rb = a
            table = val.get(ra) if ra in val else val.get(rb)
            kind = {'lwzx': 'word', 'lhzx': 'half', 'lbzx': 'byte'}[op]
            other = rb if ra in val else ra
            index_src = idx[other][0] if other in idx else None
            if table is not None:
                loaded[d] = (kind, table)
            val.pop(d, None)
        elif op == 'add' and len(a) == 3:
            d, ra, rb = a
            for b, o in ((ra, rb), (rb, ra)):
                if b in val and (o in loaded or o in shifted):
                    if o in loaded:
                        kind, table = loaded[o]
                        shift = 0
                    else:
                        kind, table, shift = shifted[o]
                    result = (kind, table, val[b], shift)
            val.pop(d, None)
        elif op == 'mtctr':
            s = a[0]
            if s in loaded and loaded[s][0] == 'word':
                result = ('word', loaded[s][1], 0, 0)
    if result:
        bound = bounds.get(index_src)
        result = result + (index_src, bound + 1 if bound is not None else None)
    return result


def entry(image, kind, table, i):
    off = table - BASE
    if kind == 'word':
        return struct.unpack_from('>I', image, off + 4 * i)[0]
    if kind == 'half':
        return struct.unpack_from('>H', image, off + 2 * i)[0]
    return image[off + i]


def main():
    image = open(sys.argv[1], 'rb').read()
    gen = sys.argv[2]
    toml_out = sys.argv[sys.argv.index('--toml') + 1] if '--toml' in sys.argv else None
    checked = bad = unknown = 0
    blocks = []
    for path in sorted(glob.glob(gen + '/*_recomp.*.cpp')):
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        insns = []      # (op, args, guest address unknown) since the function start
        func = None
        i = 0
        while i < len(lines):
            line = lines[i]
            m = FUNC.match(line)
            if m:
                func, insns = m.group(1), []
            m = INSN.match(line)
            if m and not line.strip().startswith('// ---'):
                insns.append((m.group(1).rstrip('.'), m.group(2).strip()))
            m = SWITCH.match(line)
            if m:
                index_reg = int(m.group(1))
                cases = {}
                j = i + 1
                while j < len(lines) and not lines[j].strip().startswith('default:') \
                        and lines[j].strip() != '}':
                    c = CASE.match(lines[j])
                    if c:
                        g = GOTO.match(lines[j + 1])
                        if g:
                            cases[int(c.group(1))] = int(g.group(1), 16)
                    j += 1
                # The bctr is the last instruction comment before the switch.
                window = [x for x in insns[-LOOKBACK:] if x[0] != 'nop']
                got = replay(window, image)
                checked += 1
                if not got:
                    unknown += 1
                else:
                    kind, table, base, shift, src, length = got
                    n = max(cases) + 1 if cases else 0
                    problems = []
                    if src and src != f'r{index_reg}':
                        problems.append(f'switches on r{index_reg}, the PowerPC indexes with {src}')
                        index_reg = int(src[1:])
                    if length and length > n:
                        problems.append(f'{n} cases, the bound check allows {length}')
                        n = length
                    want = []
                    for k in range(n):
                        e = entry(image, kind, table, k)
                        want.append(e if kind == 'word' else u32(base + (e << shift)))
                    diff = [k for k in range(n) if cases.get(k) != want[k]]
                    if problems:
                        print(f'sub_{func} ({path.split("/")[-1].split(chr(92))[-1]}:{i + 1}): '
                              f'{kind} table {table:08X}: ' + '; '.join(problems))
                    if diff or problems:
                        bad += 1
                        if diff:
                            print(f'sub_{func} ({path.split("/")[-1].split(chr(92))[-1]}:{i + 1}): '
                                  f'{kind} table {table:08X} base {base:08X} shift {shift} index r{index_reg}: '
                                  f'{len(diff)} of {n} cases wrong (case {diff[0]}: generated '
                                  f'{cases.get(diff[0], 0):08X}, image {want[diff[0]]:08X})')
                        blocks.append((func, index_reg, want, table))
                i = j
            i += 1
    print(f'{checked} switches, {bad} wrong, {unknown} not replayed')
    if toml_out and blocks:
        # The override is keyed by the bctr address, which the comments don't carry; the caller
        # resolves it (see find_bctr below) from the image: the bcctr nearest after the table load.
        with open(toml_out, 'w', newline='\n') as f:
            for func, reg, want, table in blocks:
                bctr = find_bctr(image, int(func, 16), table)
                f.write(f'\n[[entrypoint.switch_tables]]   # in sub_{func}, table {table:08X}\n')
                f.write(f'address = 0x{bctr:08X}\nregister = {reg}\nlabels = [\n')
                for k in range(0, len(want), 6):
                    f.write('  ' + ', '.join(f'0x{t:08X}' for t in want[k:k + 6]) + ',\n')
                f.write(']\n')
        print(f'wrote {toml_out}')


def find_bctr(image, func, table):
    """The bctr whose setup loads `table` with lis/addi, searching the function forward."""
    hi = (table + 0x8000) >> 16 & 0xFFFF
    a = func
    seen_lis = False
    while a < func + 0x40000:
        w = struct.unpack_from('>I', image, a - BASE)[0]
        if w >> 26 == 15 and (w & 0xFFFF) == hi and (w >> 16 & 0x1F) == 0:  # lis rX,hi
            seen_lis = True
        if seen_lis and w == 0x4E800420:  # bctr
            return a
        a += 4
    raise SystemExit(f'no bctr found for table {table:08X} in sub_{func:08X}')


if __name__ == '__main__':
    main()
