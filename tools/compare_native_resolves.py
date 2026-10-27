#!/usr/bin/env python3
"""Compares two resolve dumps of one frame (D3D12 --graw_native_resolve_dump, Vulkan
--graw_native_vulkan_resolve_dump; docs/VULKAN_RENDERER.md, M4b): the mean RGBA of every
resolve, decoded from its destination format, side by side.

    python tools/compare_native_resolves.py <d3d12 dir> <vulkan dir> <frame>
"""
import json, sys, os
import numpy as np

def load(d, frame):
    meta = [json.loads(l) for l in open(os.path.join(d, f'f{frame}-resolves.jsonl'))]
    return meta

def f20e4(v):
    m = v & 0xFFFFF; e = v >> 20
    out = np.where(e == 0, m.astype(np.float64) / (1 << 20) * 2.0**-14,
                   (1 + m.astype(np.float64) / (1 << 20)) * np.power(2.0, e.astype(np.float64) - 15))
    return out

def decode(d, frame, m):
    raw = np.fromfile(os.path.join(d, f'f{frame}-r{m["index"]}-native.bin'), dtype=np.uint8)
    w, h = m['native_width'], m['native_height']
    f = m['dest_format']
    if m['source'] == 4:
        # D3D12, and Vulkan's dumps with depth_bits: d24 << 8 | stencil; older Vulkan dumps:
        # R32 floats.
        if m['bpp'] == 4 and ('endian' in m or m.get('depth_bits')):
            v = raw.view('<u4')
            return np.stack([f20e4(v >> 8)] * 4, -1)
        v = raw.view('<f4')
        return np.stack([v] * 4, -1)
    if f == 6:
        v = raw.reshape(-1, 4).astype(np.float64) / 255.0
        return v
    if f == 7:
        v = raw.view('<u4')
        return np.stack([(v & 1023) / 1023, ((v >> 10) & 1023) / 1023, ((v >> 20) & 1023) / 1023, (v >> 30) / 3], -1)
    if f == 2:
        v = raw.astype(np.float64) / 255
        return np.stack([v, v * 0, v * 0, v * 0 + 1], -1)
    if f == 32:
        v = raw.view('<f2').astype(np.float64).reshape(-1, 4)
        return v
    if f == 31:
        v = raw.view('<f2').astype(np.float64).reshape(-1, 2)
        return np.concatenate([v, v * 0], -1)
    return None

a, b, frame = sys.argv[1], sys.argv[2], sys.argv[3]
ma, mb = load(a, frame), load(b, frame)
print(f'{len(ma)} vs {len(mb)} resolves')
for i in range(max(len(ma), len(mb))):
    la = ma[i] if i < len(ma) else None
    lb = mb[i] if i < len(mb) else None
    m = la or lb
    desc = f'r{i:<3} src{m["source"]} fmt{m["dest_format"]:<2} {m["dest_width"]}x{m["dest_height"]} @{m["dest_base"]:08X} rect{m["rect"]} swap{m["swap"]} bias{m["exp_bias"]} sel{m["sample_select"]}'
    def stat(d, mm):
        if not mm: return 'missing'
        v = decode(d, frame, mm)
        if v is None: return f'fmt {mm["dest_format"]}?'
        return ' '.join(f'{x:7.4f}' for x in v.mean(0))
    print(desc)
    print('    dx', stat(a, la))
    print('    vk', stat(b, lb))
