"""Registers a translated function reads before it writes them, along some path from its entry.

    python -I tools/regflow.py port-yeti/generated/yeti_final_xenon [--all]

From fear360-recomp (tools/analysis/regflow.py, docs/HANDOFF-REGOPTS.md), unchanged below.
The translator's register options (grawyeti_manifest.toml) turn some registers into each generated function's C++
locals: r0, r2, r11, r12 and f0 (non_argument_as_local), r14-r31, f14-f31 and the non-volatile
VMX registers (non_volatile_as_local) and the CR fields (cr_as_local). A local starts at zero, so
a function that reads one of them before writing it, on some path, expects a value its caller (or
the function it was split from) left in the register: it reads zero instead. The usual causes are
an SEH funclet (frame in r12, owner's non-volatiles), setjmp (saves the non-volatiles) and a
function the analyser split in two. Reads that only save a register into the frame (the prologue's
std r31,-8(r1), mfcr) are not counted.

Works on the generated C++: each instruction's disassembly comment, its label (loc_X:) and the
gotos and returns emitted for it. A call (bl) is taken to preserve the non-volatiles and to leave
the volatiles undefined, as the ABI says.
"""
import collections
import glob
import os
import re
import sys

FUNC_RE = re.compile(r'DEFINE_REX_FUNC\((\w+)\) \{\n(.*?)\n\}', re.S)
CALL_RE = re.compile(r'\b(sub_[0-9A-F]+)\(ctx, base\);')
LABEL_RE = re.compile(r'^loc_([0-9A-F]+):$')
GOTO_RE = re.compile(r'goto loc_([0-9A-F]+);')

NONARG = {'r0', 'r2', 'r11', 'r12', 'f0'} | {f'v{i}' for i in range(32, 64)}
NONVOL = ({f'r{i}' for i in range(14, 32)} | {f'f{i}' for i in range(14, 32)} |
          {f'v{i}' for i in range(14, 32)} | {f'v{i}' for i in range(64, 128)})
CRS = {f'cr{i}' for i in range(8)}
SPECIAL = {'ctr', 'xer', 'reserved'}  # ctr_as_local, xer_as_local (the carry), reserved_as_local
TRACKED = NONARG | NONVOL | CRS | SPECIAL
VOLATILE_AFTER_CALL = ({f'r{i}' for i in (0, 2, 11, 12)} | {'f0'} |
                       {f'v{i}' for i in range(32, 64)} | {'cr0', 'cr1', 'cr5', 'cr6', 'cr7'} |
                       SPECIAL)
REG_RE = re.compile(r'^(r\d+|f\d+|v\d+|cr\d)$')
# Stores and other ops whose first operand is read, not written.
NO_DEST = re.compile(r'^(st|dcb|icbi|mt(ctr|lr|xer|msr|fsf|spr)|sync|eieio|isync|lwsync|nop)')
CMP = re.compile(r'^(cmp|fcmp)')
MEMORY = re.compile(r'^(l|st|dcb|icbi)')


def operands(text):
    parts = text.replace('(', ',').replace(')', '').split(None, 1)
    op = parts[0]
    args = [a.strip() for a in parts[1].split(',')] if len(parts) > 1 else []
    return op, [a for a in args if a]


CARRY_SET = ('addic', 'addc', 'subfc', 'subfic', 'srawi', 'sraw', 'sradi', 'srad')
CARRY_USE = ('adde', 'addze', 'addme', 'subfe', 'subfze', 'subfme')


def special_defs_uses(op):
    """CTR, the XER carry and the lwarx reservation."""
    base = op.rstrip('.').rstrip('o')
    defs, uses = set(), set()
    if op == 'mtctr':
        defs.add('ctr')
    elif op == 'mfctr' or (op.startswith('b') and 'ctr' in op):
        uses.add('ctr')
    elif op.startswith(('bdnz', 'bdz')):
        defs.add('ctr')
        uses.add('ctr')
    if base in CARRY_SET or op == 'mtxer':
        defs.add('xer')
    elif base in CARRY_USE:
        defs.add('xer')
        uses.add('xer')
    elif op == 'mfxer':
        uses.add('xer')
    if op in ('lwarx', 'ldarx'):
        defs.add('reserved')
    elif op in ('stwcx.', 'stdcx.'):
        uses.add('reserved')
    return defs, uses


def defs_uses(op, args):
    """The tracked registers an instruction writes and reads (disassembly operand order)."""
    d, u = gpr_defs_uses(op, args)
    d2, u2 = special_defs_uses(op)
    return d | d2, u | u2


def gpr_defs_uses(op, args):
    defs, uses = set(), set()
    if op.startswith(('tw', 'td')):  # traps
        return defs, uses
    if MEMORY.match(op) and 'r0' in args:
        # r0 as the base register is a literal 0: lvx v0,r0,r3; lwz r3,8(r0); dcbt r0,r3.
        base = 0 if op.startswith(('dcb', 'icbi')) else 2 if len(args) > 2 and             args[1].lstrip('-').isdigit() else 1
        if base < len(args) and args[base] == 'r0':
            args = args[:base] + ['0'] + args[base + 1:]
    regs = [a for a in args if REG_RE.match(a)]
    if op.startswith('b'):
        # Conditional branches read their CR field (cr0 when none is named); bl/bctrl/blrl calls.
        if op not in ('b', 'bl', 'blr', 'bctr', 'bctrl', 'blrl') and not op.startswith('bdnz') \
                and not op.startswith('bdz'):
            uses.add(next((a for a in args if a in CRS), 'cr0'))
        return defs, uses
    if op == 'mfcr':  # copies all of CR, the prologue's save of cr2-cr4
        return {regs[0]} if regs else set(), set()
    if op == 'mtcrf':
        return set(CRS), set(regs)
    if op.startswith('cr'):  # crxor 6,6,6: bit numbers; the field of the target bit is written
        try:
            defs.add(f'cr{int(args[0]) // 4}')
        except (ValueError, IndexError):
            pass
        return defs, uses
    if CMP.match(op):
        if regs and regs[0] in CRS:
            defs.add(regs[0])
            uses.update(regs[1:])
        else:
            defs.add('cr0')
            uses.update(regs)
        return defs, uses
    if op.endswith('.'):
        defs.add('cr1' if op.startswith('f') else 'cr6' if op.startswith('v') else 'cr0')
    if op.startswith('vcmp'):
        return defs | set(regs[:1]), set(regs[1:])
    if NO_DEST.match(op):
        uses.update(regs)
        return defs, uses
    if not regs:
        return defs, uses
    if op in ('stmw', 'lmw'):
        return defs, uses
    dest, srcs = regs[0], regs[1:]
    if op in ('xor', 'subf', 'sub') and len(regs) == 3 and regs[1] == regs[2]:
        return {dest}, set()  # xor r30,r30,r30: the zeroing idiom reads nothing
    # Read-modify-write: inserts, multiply-adds into the target, update forms read the base.
    if op.startswith(('rlwimi', 'rldimi', 'vmaddfp128', 'vnmsubfp128')) or \
            op.startswith('vrlimi'):
        uses.add(dest)
    if op.startswith('l') and op.endswith(('u', 'ux')) and len(regs) >= 2:
        defs.add(regs[1])
    defs.add(dest)
    uses.update(srcs)
    return defs, uses


def is_frame_save(op, args):
    """std r31,-8(r1), stfd f31,-16(r1), stvx v127,r11,r1, stvlx128 v64,r1,r6: a non-volatile
    saved into the frame."""
    if not op.startswith('st') or not args:
        return False
    return args[0] in NONVOL and op != 'stwu' and         (args[-1] in ('r1', 'r12', 'r11') or (len(args) == 3 and args[1] == 'r1'))


def analyse(name, body):
    lines = body.split('\n')
    insns = []  # (address index, op, args, gotos, falls through)
    labels = {}
    cur = None
    for line in lines:
        s = line.strip()
        m = LABEL_RE.match(s)
        if m:
            labels[m.group(1)] = len(insns)
            continue
        if line.startswith('\t// ') and not line.startswith('\t// ERROR'):
            text = line[4:].strip()
            if not text or text.startswith(('PATCHED', 'hook', 'mid-asm')):
                continue
            op, args = operands(text)
            cur = {'op': op, 'args': args, 'gotos': [], 'stop': False, 'code': []}
            insns.append(cur)
        elif cur is not None:
            cur['code'].append(line)
    for ins in insns:
        for c in ins['code']:
            ins['gotos'] += GOTO_RE.findall(c)
            # An unconditional goto or return at statement level ends the path.
            if re.match(r'^\t(goto loc_[0-9A-F]+|return);', c):
                ins['stop'] = True
    n = len(insns)
    # Forward dataflow over (register, origin) pairs: origin 'entry' while the register is
    # unwritten since the function's entry, or the callee a call left it to.
    state_in = [None] * n
    entry = frozenset((r, 'entry') for r in TRACKED)
    work = collections.deque([(0, entry)]) if n else collections.deque()
    first_use = {}
    while work:
        i, state = work.popleft()
        if i >= n:
            continue
        old = state_in[i]
        new = state if old is None else old | state
        if old is not None and new == old:
            continue
        state_in[i] = new
        ins = insns[i]
        op, args = ins['op'], ins['args']
        d, u = defs_uses(op, args)
        if not is_frame_save(op, args):
            for r, origin in new:
                if r in u:
                    first_use.setdefault((r, origin), op + ' ' + ','.join(args))
        out = frozenset(p for p in new if p[0] not in d)
        if op.startswith('b') and op.endswith('l'):
            # A call leaves the volatiles to the callee and preserves the non-volatiles.
            callee = next((m.group(1) for c in ins['code']
                           for m in [re.search(r'(\w+)\(ctx, base\);', c)] if m), 'indirect')
            out = frozenset(p for p in out if p[0] not in VOLATILE_AFTER_CALL) | \
                frozenset((r, callee) for r in VOLATILE_AFTER_CALL)
        for g in ins['gotos']:
            if g in labels:
                work.append((labels[g], out))
        if not ins['stop']:
            work.append((i + 1, out))
    return first_use


def kind(r):
    return 'cr' if r in CRS else r if r in SPECIAL else 'nonvol' if r in NONVOL else 'nonarg'


def main():
    gen = sys.argv[1]
    callers = collections.Counter()
    funcs = {}
    for path in sorted(glob.glob(os.path.join(gen, '*.cpp'))):
        text = open(path, encoding='utf-8', errors='replace').read()
        callers.update(CALL_RE.findall(text))
        for m in FUNC_RE.finditer(text):
            funcs[m.group(1)] = m.group(2)
    inputs = collections.defaultdict(list)   # kinds -> [(function, {reg: insn})]
    after_call = collections.defaultdict(list)  # callee -> [(function, reg, insn)]
    for name, body in funcs.items():
        if name.startswith('__'):  # save/restore helpers: elided under non_volatile_as_local
            continue
        used = analyse(name, body)
        regs = {r: insn for (r, origin), insn in used.items() if origin == 'entry'}
        if regs:
            # A function that sets up its own frame was compiled as one: the compiler never
            # depends on a caller's non-volatiles, so its reads are on paths where the value is
            # dead. Without a frame it may be a piece of another function, or a funclet.
            head = [l.strip()[3:] for l in body.split('\n') if l.startswith('\t// ')][:12]
            framed = any(h.startswith(('stwu r1,', 'stdu r1,', 'stwux r1,')) for h in head)
            inputs[(frozenset(kind(r) for r in regs), framed)].append((name, regs))
        for (r, origin), insn in used.items():
            if origin != 'entry':
                after_call[origin].append((name, r, insn))
    limit = None if '--all' in sys.argv else 15
    print(f'{len(funcs)} functions, {sum(len(v) for v in inputs.values())} read a register '
          'their caller set (unwritten since entry on some path):')
    for (kinds, framed), items in sorted(inputs.items(), key=lambda kv: (kv[0][1], -len(kv[1]))):
        print(f'\n== {"+".join(sorted(kinds))}, {"own frame" if framed else "no frame"}: '
              f'{len(items)} functions')
        items.sort(key=lambda it: (-callers[it[0]], it[0]))
        for name, regs in items[:limit]:
            print(f'  {name} calls={callers[name]}  ' +
                  ' '.join(f'{r}@{regs[r]}' for r in sorted(regs)))
    print(f'\n{sum(len(v) for v in after_call.values())} reads of a volatile a call left '
          'undefined, by callee (a problem only if the callee returns a value there):')
    for callee, items in sorted(after_call.items(), key=lambda kv: -len(kv[1]))[:limit]:
        regs = collections.Counter(r for _, r, _ in items)
        print(f'  {callee}: {len(items)} reads in {len({f for f, _, _ in items})} functions '
              f'({", ".join(f"{r} {c}" for r, c in regs.most_common())}), e.g. {items[0][0]} '
              f'{items[0][2]}')


if __name__ == '__main__':
    main()
