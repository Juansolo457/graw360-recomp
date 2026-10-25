#!/usr/bin/env python3
"""Map the guest Xbox 360 D3D library in ReXGlue generated C++ (renderer discovery).

The XDK's D3D is linked statically into the game. Its draw, clear and state functions build PM4
type-3 packets (header 0xC0000000 | (dwords - 1) << 16 | opcode << 8) and write them into the
ring buffer that ReXGlue's command processor replays. This lists the functions that build
packet headers, by opcode, plus who calls them from outside the library, so the API entry
points (DrawIndexedVertices, Clear, Resolve, Swap, SetBinMask for predicated tiling) can be
named and hooked, as shaunsingh/pgr4-native does for PGR4 (scripts/analysis/scan_d3d_surface.py
there).

  python -I tools/scan_d3d.py port-yeti/generated/yeti_final_xenon [--lo 0x824C0000 --hi 0x824F0000]

Headers are found from the generated code's instruction comments: `lis rX,<0xC0nn>` followed by
`ori rY,rX,<opcode << 8 | predicate>` (or addi) in the same function. Packets built some other way
(computed counts, tables) are missed.
"""
import argparse
import collections
import glob
import os
import re

# Type-3 opcodes, from the SDK's include/rex/graphics/xenos.h.
PM4 = {
    0x48: 'ME_INIT', 0x10: 'NOP', 0x3F: 'INDIRECT_BUFFER', 0x37: 'INDIRECT_BUFFER_PFD',
    0x26: 'WAIT_FOR_IDLE', 0x3C: 'WAIT_REG_MEM', 0x52: 'WAIT_REG_EQ', 0x53: 'WAIT_REG_GTE',
    0x5C: 'WAIT_UNTIL_READ', 0x5D: 'WAIT_IB_PFD_COMPLETE', 0x21: 'REG_RMW', 0x3E: 'REG_TO_MEM',
    0x3D: 'MEM_WRITE', 0x4F: 'MEM_WRITE_CNTR', 0x44: 'COND_EXEC', 0x45: 'COND_WRITE',
    0x46: 'EVENT_WRITE', 0x58: 'EVENT_WRITE_SHD', 0x59: 'EVENT_WRITE_CFL', 0x5A: 'EVENT_WRITE_EXT',
    0x5B: 'EVENT_WRITE_ZPD', 0x22: 'DRAW_INDX', 0x36: 'DRAW_INDX_2', 0x34: 'DRAW_INDX_BIN',
    0x35: 'DRAW_INDX_2_BIN', 0x23: 'VIZ_QUERY', 0x25: 'SET_STATE', 0x2D: 'SET_CONSTANT',
    0x55: 'SET_CONSTANT2', 0x56: 'SET_SHADER_CONSTANTS', 0x2F: 'LOAD_ALU_CONSTANT', 0x27: 'IM_LOAD',
    0x2B: 'IM_LOAD_IMMEDIATE', 0x2E: 'LOAD_CONSTANT_CONTEXT', 0x3B: 'INVALIDATE_STATE',
    0x04: 'SET_SHADER_BASES', 0x50: 'SET_BIN_MASK', 0x51: 'SET_BIN_SELECT', 0x5E: 'CONTEXT_UPDATE',
    0x54: 'INTERRUPT', 0x64: 'XE_SWAP', 0x2C: 'IM_STORE', 0x60: 'SET_BIN_MASK_LO',
    0x61: 'SET_BIN_MASK_HI', 0x62: 'SET_BIN_SELECT_LO', 0x63: 'SET_BIN_SELECT_HI',
}

FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
LIS = re.compile(r'//\s*lis\s+(r\d+),(-?\d+)')
ORI = re.compile(r'//\s*(?:ori|addi)\s+(r\d+),(r\d+),(-?\d+)')
CALL = re.compile(r'\bsub_([0-9A-F]{8})\(ctx, base\)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('generated')
    ap.add_argument('--lo', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--hi', type=lambda s: int(s, 0), default=0xFFFFFFFF)
    args = ap.parse_args()

    builds = collections.defaultdict(collections.Counter)  # func -> opcode -> count
    callers = collections.defaultdict(set)                   # callee -> callers
    sizes = {}
    for path in sorted(glob.glob(os.path.join(args.generated, '*.cpp'))):
        cur, lis, n = None, {}, 0
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                if cur is not None:
                    sizes[cur] = n
                cur, lis, n = int(m.group(1), 16), {}, 0
                continue
            if cur is None:
                continue
            if line.lstrip().startswith('//'):
                n += 1
            m = LIS.search(line)
            if m:
                lis[m.group(1)] = int(m.group(2)) & 0xFFFF
                continue
            m = ORI.search(line)
            if m and m.group(2) in lis:
                hi = lis[m.group(2)]
                lo = int(m.group(3)) & 0xFFFF
                value = (hi << 16) + (lo if 'ori' in line else int(m.group(3)))
                value &= 0xFFFFFFFF
                if value >> 30 == 3 and ((value >> 8) & 0x7F) in PM4 and (value & 0xFE) == 0:
                    # Bit 0 is the predicate: the packet runs only in the EDRAM tiles
                    # (bins) that SET_BIN_MASK/SELECT enable, i.e. predicated tiling.
                    builds[cur][((value >> 8) & 0x7F, value & 1)] += 1
                continue
            for c in CALL.findall(line):
                callers[int(c, 16)].add(cur)
        if cur is not None:
            sizes[cur] = n

    def inside(a):
        return args.lo <= a < args.hi

    rows = sorted(f for f in builds if inside(f))
    print(f'{len(rows)} functions build PM4 type-3 headers'
          + (f' in {args.lo:08X}..{args.hi:08X}' if args.hi != 0xFFFFFFFF else ''))
    for f in rows:
        ext = sorted(c for c in callers[f] if not inside(c)) if args.hi != 0xFFFFFFFF else []
        ops = ', '.join(f"{PM4[o]}{'(pred)' if p else ''}x{k}" for (o, p), k in sorted(builds[f].items()))
        print(f'  sub_{f:08X}  n={sizes.get(f, 0):5d}  callers={len(callers[f]):3d} ext={len(ext):3d}  {ops}')
    by_op = collections.defaultdict(list)
    for f in rows:
        for o in builds[f]:
            by_op[o].append(f)
    print('by opcode:')
    for o, p in sorted(by_op):
        name = PM4[o] + ('(pred)' if p else '')
        print(f'  {name:<26} ' + ' '.join(f'{f:08X}' for f in by_op[(o, p)]))


if __name__ == '__main__':
    main()
