"""List likely function entries the ReXGlue analyser missed: words in an executable image's data
that point into its code at a function boundary (the instruction before is a blr, a bctr, an
unconditional branch or padding) and sit in a table next to other code pointers, but are not a
known function start, plus addresses built in code (lis/addi) at a function boundary: callbacks.
Method tables (.rdata vtables) and callbacks are where these come from; each one called at run time ends the game with "Call
to invalid or unregistered function". The output is [entrypoint.functions] lines.

    python -I tools/find_missed_functions.py <image.bin> <generated/default folder>

The image is the game's loaded executable (tools/xex_basic_decrypt.py (or a runtime dump)); the generated folder supplies the
code range (<name>_pch.h) and the known function starts (<name>_init.cpp). It's a heuristic: a
candidate that is really a label inside a function splits it, and codegen then warns "Unresolved
conditional branch" across the split; drop that candidate.
"""
import glob
import re
import struct
import sys

image = open(sys.argv[1], 'rb').read()
gen = sys.argv[2]
pch = open(glob.glob(gen + '/*_pch.h')[0], encoding='utf-8').read()
base = int(re.search(r'REX_IMAGE_BASE (0x[0-9A-Fa-f]+)', pch).group(1), 16)
code_base = int(re.search(r'REX_CODE_BASE (0x[0-9A-Fa-f]+)', pch).group(1), 16)
code_end = code_base + int(re.search(r'REX_CODE_SIZE (0x[0-9A-Fa-f]+)', pch).group(1), 16)
known = {int(a, 16) for a in re.findall(r'\{ 0x([0-9A-F]+), ', open(glob.glob(gen + '/*_init.cpp')[0], encoding='utf-8').read())}


def word(addr):
    return struct.unpack_from('>I', image, addr - base)[0]


BLR, BCTR, NOP = 0x4E800020, 0x4E800420, 0x60000000
found = {}
for offset in range(0, len(image) - 3, 4):
    addr = base + offset
    if code_base <= addr < code_end:
        continue  # pointers inside code are branches and jump tables, not entries
    target = struct.unpack_from('>I', image, offset)[0]
    if not (code_base <= target < code_end) or target & 3 or target in known:
        continue
    prev = word(target - 4)
    boundary = prev in (BLR, BCTR, 0, NOP) or (prev >> 26) == 18 and not prev & 1  # b (not bl)
    # In a table: a neighbouring word points into code too (a method or callback table).
    in_table = any(code_base <= word(n) < code_end for n in (addr - 4, addr + 4)
                   if base <= n < base + len(image) - 3)
    if boundary and in_table:
        found.setdefault(target, addr)
# Callbacks built in code: lis rX,hi then addi rY,rX,lo (each instruction is a comment in the
# generated code) forming a function-boundary address that isn't a known function. These are
# noisier (labels, jump tables, exception-handler funclets entered with the caller's r31), so a
# target must lie in a gap no known function covers and start with a plausible first instruction:
# not padding, not an address (a jump table), not reading r31 before setting it.
import bisect
import glob
ranges = []
for path in glob.glob(gen + '/*_recomp.*.cpp'):
    start = count = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', line)
        if m:
            if start is not None:
                ranges.append((start, start + 4 * count))
            start, count = int(m.group(1), 16), 0
        elif start is not None and re.match(r'\s*// [a-z]', line):
            count += 1
    if start is not None:
        ranges.append((start, start + 4 * count))
ranges.sort()
starts = [r[0] for r in ranges]


def covered(target):
    i = bisect.bisect_right(starts, target) - 1
    return i >= 0 and ranges[i][0] < target < ranges[i][1]


def plausible_entry(target):
    first = word(target)
    if first == 0 or base <= first < base + len(image):
        return False
    ra = (first >> 16) & 31 if (first >> 26) in (32, 34, 36, 38, 48, 50, 52, 54) else None
    return ra != 31  # a load or store off r31 before anything sets it


after_bl = set()
for path in glob.glob(gen + '/*_recomp.*.cpp'):
    hi = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('DEFINE_REX_FUNC'):
            hi = {}
            continue
        m = re.search(r'// lis (r\d+),(-?\d+)$', line)
        if m:
            hi[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        m = re.search(r'// addi (r\d+),(r\d+),(-?\d+)$', line)
        if not m or m.group(2) not in hi:
            continue
        target = (hi[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
        if not (code_base <= target < code_end) or target & 3 or target in known:
            continue
        prev = word(target - 4)
        if prev in (BLR, BCTR, 0, NOP) or (prev >> 26) == 18 and not prev & 1:
            if not covered(target) and plausible_entry(target):
                found.setdefault(target, 0)
        # After a bl that never returns (an assert or fatal-error call ending the previous
        # function), the analyser runs on into the next function and swallows it, so `covered`
        # can't be used. An address built in code that lands right after a bl is a callback
        # (GRAW: the qsort comparator 0x825C6000). Codegen warns if one splits a function.
        elif (prev >> 26) == 18 and prev & 1 and plausible_entry(target):
            after_bl.add(target)

for target in sorted(after_bl - set(found)):
    found[target] = -1

for target in sorted(found):
    origin = ('built in code, after a bl' if found[target] == -1 else
              f'from 0x{found[target]:08X}' if found[target] else 'built in code')
    print(f'0x{target:08X} = {{}}  # {origin}')
print(f'# {len(found)} candidates', file=sys.stderr)
