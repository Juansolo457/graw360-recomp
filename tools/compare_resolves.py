#!/usr/bin/env python3
"""Compares the native renderer's resolve outputs with what the Xenos plugin wrote to guest
memory for the same frame (docs/NATIVE_RENDERER.md: which pass of a frame differs).

From pgr4-native 8ea20d5 scripts/analysis/compare_resolves.py (MIT, see native/README.md), without
its trace check.

Input: a directory written by --graw_native_resolve_dump (f<F>-resolves.jsonl,
f<F>-r<i>-native.bin = untiled little-endian texels, f<F>-r<i>-guest.bin = the guest memory at
the destination: tiled or linear, big-endian). Create <dir>/dump.request to dump the next frame;
run with --graw_native_render_thread=false so the guest waits in that frame's Swap while its
memory is read.

  python -I tools/compare_resolves.py <dir> [--frame F] [--png] [--csv out.csv]

Tiled addressing: texture_util::GetTiledOffset2D from the ReXGlue SDK (BSD-3, Xenia
developers), after gildor2/UModel.
"""
import argparse, glob, json, math, os, re, sys

import numpy as np

FORMATS = {2: 'k_8', 4: 'k_5_6_5', 5: 'k_6_5_5', 6: 'k_8_8_8_8', 7: 'k_2_10_10_10', 10: 'k_8_8', 30: 'k_16_FLOAT',
           31: 'k_16_16_FLOAT', 32: 'k_16_16_16_16_FLOAT', 36: 'k_32_FLOAT', 37: 'k_32_32_FLOAT',
           38: 'k_32_32_32_32_FLOAT'}


def tiled_offset_2d(x, y, pitch, bpp_log2):
    """Vectorized GetTiledOffset2D (x, y int arrays)."""
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpp_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpp_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def endian_swap(buf, endian):
    a = np.frombuffer(buf[:len(buf) // 4 * 4], dtype='<u4').copy()
    if endian == 1:
        a = ((a & 0x00FF00FF) << 8) | ((a & 0xFF00FF00) >> 8)
    elif endian == 2:
        a = a.byteswap()
    elif endian == 3:
        a = (a << 16) | (a >> 16)
    return a.astype('<u4').tobytes()


def guest_image(m, data):
    """Untiled, endian-swapped guest texels (h, w*bpp) covering the native extent."""
    w, h, bpp = m['native_width'], m['native_height'], m['bpp']
    data = endian_swap(data, m['endian'])
    raw = np.frombuffer(data, dtype=np.uint8)
    ys, xs = np.mgrid[0:h, 0:w]
    if m['tiled']:
        off = tiled_offset_2d(xs.astype(np.int64), ys.astype(np.int64), m['pitch_blocks'], int(math.log2(bpp)))
    else:
        off = (ys * m['pitch_blocks'] + xs).astype(np.int64) * bpp
    out = np.zeros((h, w, bpp), dtype=np.uint8)
    valid = off + bpp <= raw.size
    for b in range(bpp):
        o = np.where(valid, off + b, 0)
        out[..., b] = np.where(valid, raw[o], 0)
    return out, int((~valid).sum())


def to_host_layout(img, fmt):
    """Guest 16-bit texels -> the texture cache's host layout (texture_layout.cpp conversions),
    which is what the native resolve writes."""
    if fmt not in (4, 5):
        return img
    h, w, _ = img.shape
    v = img.copy().view('<u2').reshape(h, w).astype(np.uint32)
    if fmt == 4:  # kR5G6B5ToB5G6R5
        v = (v & 0x07E0) | ((v & 0x001F) << 11) | ((v & 0xF800) >> 11)
    else:  # kR5G5B6ToB5G6R5WithRBGASwizzle
        v = (v & 0x001F) | ((v & 0x03E0) << 6) | ((v & 0xFC00) >> 5)
    return v.astype('<u2').view(np.uint8).reshape(h, w, 2)


def channels(img, fmt, depth):
    """Texels (h, w, bpp bytes) -> float channels for comparison, plus the comparison scale."""
    h, w, bpp = img.shape
    if depth:  # k_8_8_8_8 holding (depth24 << 8) | stencil
        v = img.copy().view('<u4').reshape(h, w).astype(np.int64)
        return np.stack([(v >> 8) & 0xFFFFFF, v & 0xFF], -1).astype(np.float64), None
    if fmt in (2, 6, 10):
        return img.astype(np.float64) / 255.0, 1.0
    if fmt == 7:
        v = img.copy().view('<u4').reshape(h, w).astype(np.int64)
        return np.stack([(v & 1023) / 1023, ((v >> 10) & 1023) / 1023, ((v >> 20) & 1023) / 1023,
                         ((v >> 30) & 3) / 3], -1), 1.0
    if fmt in (4, 5):  # host layout (B5G6R5): see to_host_layout
        v = img.copy().view('<u2').reshape(h, w).astype(np.int64)
        lo, mid, hi = (v & 31) / 31, ((v >> 5) & 63) / 63, ((v >> 11) & 31) / 31
        return (np.stack([hi, mid, lo], -1) if fmt == 4 else np.stack([lo, hi, mid], -1)), 1.0
    if fmt in (30, 31, 32):
        return img.copy().view('<f2').reshape(h, w, -1).astype(np.float64), None
    if fmt in (36, 37, 38):
        return img.copy().view('<f4').reshape(h, w, -1).astype(np.float64), None
    raise ValueError(f'format {fmt}')


def to_png_rgb(ch, fmt, depth):
    if depth:
        v = ch[..., 0] / float(0xFFFFFF)
        return np.repeat((np.clip(v, 0, 1) * 255).astype(np.uint8)[..., None], 3, -1)
    c = ch
    if c.shape[-1] == 1:
        c = np.repeat(c, 3, -1)
    elif c.shape[-1] == 2:
        c = np.concatenate([c, np.zeros_like(c[..., :1])], -1)
    c = c[..., :3]
    if fmt not in (2, 4, 5, 6, 7, 10):  # float: show [0, 1] after a simple Reinhard
        c = c / (1.0 + np.abs(c))
    return (np.clip(np.nan_to_num(c), 0, 1) * 255 + 0.5).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--frame', type=int)
    ap.add_argument('--png', action='store_true', help='write side-by-side PNGs next to the dumps')
    ap.add_argument('--csv')
    a = ap.parse_args()
    metas = sorted(glob.glob(os.path.join(a.dir, 'f*-resolves.jsonl')),
                   key=lambda p: int(re.search(r'f(\d+)-', os.path.basename(p)).group(1)))
    if a.frame is not None:
        metas = [p for p in metas if os.path.basename(p) == f'f{a.frame}-resolves.jsonl']
    if not metas:
        sys.exit('no f*-resolves.jsonl')
    meta_path = metas[-1]
    prefix = meta_path[:-len('-resolves.jsonl')]
    rows = []
    worst = 0.0
    print(f'# {meta_path}')
    print(f'{"i":>3} {"src":>5} {"dest":>8} {"format":<20} {"size":>9} {"exact%":>7} {"maxdiff":>9} {"psnr":>6} note')
    all_metas = [json.loads(line) for line in open(meta_path)]
    # The guest dump is the frame's final memory: a later resolve to the same address wins.
    later = {mm['index']: all_metas[k + 1:] for k, mm in enumerate(all_metas)}
    for m in all_metas:
        i = m['index']
        depth = m['source'] == 4
        nat_path, gst_path = f'{prefix}-r{i}-native.bin', f'{prefix}-r{i}-guest.bin'
        name = FORMATS.get(m['dest_format'], str(m['dest_format']))
        size = f'{m["native_width"]}x{m["native_height"]}'
        if not (os.path.exists(nat_path) and os.path.exists(gst_path)):
            print(f'{i:>3} missing dump files')
            continue
        w, h, bpp = m['native_width'], m['native_height'], m['bpp']
        nat = np.frombuffer(open(nat_path, 'rb').read(), dtype=np.uint8)
        if nat.size != w * h * bpp:
            print(f'{i:>3} native size {nat.size} != {w}x{h}x{bpp}')
            continue
        nat = nat.reshape(h, w, bpp)
        gst, oob = guest_image(m, open(gst_path, 'rb').read())
        gst = to_host_layout(gst, m['dest_format'])
        # Only the written rectangle is meaningful (dest point onwards).
        x0, y0 = m['dest_point']
        nat_r, gst_r = nat[y0:, x0:], gst[y0:, x0:]
        exact = float((nat_r == gst_r).all(-1).mean() * 100) if nat_r.size else 0.0
        cn, scale = channels(nat_r, m['dest_format'], depth)
        cg, _ = channels(gst_r, m['dest_format'], depth)
        diff = np.abs(np.nan_to_num(cn) - np.nan_to_num(cg))
        maxdiff = float(diff.max()) if diff.size else 0.0
        note = []
        if depth:
            note.append(f'stencil_eq={float((cn[..., 1] == cg[..., 1]).mean() * 100):.1f}%')
            note.append(f'depth24 max diff {int(diff[..., 0].max()) if diff.size else 0}')
            psnr = float('nan')
        else:
            mse = float((diff ** 2).mean()) if diff.size else 0.0
            peak = 1.0 if scale else max(1e-6, float(np.abs(cg).max()) if cg.size else 1.0)
            psnr = 99.0 if mse == 0 else 10 * math.log10(peak * peak / mse)
            if diff.shape[-1] == 4:  # RGB alone (alpha can differ where nothing samples it)
                mse_rgb = float((diff[..., :3] ** 2).mean()) if diff.size else 0.0
                note.append(f'rgb {99.0 if mse_rgb == 0 else 10 * math.log10(peak * peak / mse_rgb):.1f} dB'
                            f' (alpha max diff {float(diff[..., 3].max()):.4g})')
            worst = max(worst, maxdiff if scale else 0)
        if any(n['dest_base'] == m['dest_base'] for n in later[i]):
            note.append('guest = a later resolve to this address')
        if oob:
            note.append(f'{oob} texels past the guest dump')
        if m['clear_flags']:
            note.append(f'clear={m["clear_flags"]:X}')
        print(f'{i:>3} {"depth" if depth else "c%d" % m["source"]:>5} {m["dest_base"]:08X} {name:<20} {size:>9} '
              f'{exact:7.2f} {maxdiff:9.4g} {psnr:6.1f} {" ".join(note)}')
        rows.append([i, 'depth' if depth else f'color{m["source"]}', f'{m["dest_base"]:08X}', name, w, h,
                     f'{exact:.2f}', f'{maxdiff:.6g}', f'{psnr:.2f}', ' '.join(note)])
        if a.png:
            from PIL import Image
            left = to_png_rgb(cn, m['dest_format'], depth)
            right = to_png_rgb(cg, m['dest_format'], depth)
            Image.fromarray(np.concatenate([left, right], 1)).save(f'{prefix}-r{i}-native-vs-xenos.png')
    if a.csv:
        with open(a.csv, 'w') as f:
            f.write('index,source,dest_base,format,width,height,exact_pct,max_diff,psnr_db,note\n')
            for r in rows:
                f.write(','.join(str(x) for x in r) + '\n')


if __name__ == '__main__':
    main()
