"""Minimal XDVDFS reader for Xbox 360 ISOs: list files, extract selected ones.
    python -I xdvdfs.py <iso> list
    python -I xdvdfs.py <iso> extract <outdir> [name-substring ...]
"""
import os, struct, sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 2048


def find_partition(f):
    for base in (0x0, 0xFD90000, 0x2080000, 0x18300000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("no XDVDFS partition found")


def walk(f, base, sector, size, path=""):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off * 4 + 14 > len(data):
            continue
        seen.add(off)
        p = off * 4
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, p)
        if left == 0xFFFF:
            continue
        name = data[p + 14:p + 14 + nlen].decode("latin-1")
        full = f"{path}/{name}"
        if attr & 0x10:
            if length:
                yield from walk(f, base, start, length, full)
        else:
            yield full, base + start * SECTOR, length
        if left:
            stack.append(left)
        if right:
            stack.append(right)


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        files = sorted(walk(f, base, root_sector, root_size))
        if cmd == "list":
            print(f"partition base 0x{base:X}, {len(files)} files, "
                  f"{sum(l for _, _, l in files) / 2**30:.2f} GiB")
            for name, off, length in files:
                print(f"{length:>12}  {name}")
        elif cmd == "extract":
            out, pats = sys.argv[3], [p.lower() for p in sys.argv[4:]]
            for name, off, length in files:
                if pats and not any(p in name.lower() for p in pats):
                    continue
                dst = os.path.join(out, name.lstrip("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                f.seek(off)
                with open(dst, "wb") as o:
                    left = length
                    while left:
                        chunk = f.read(min(left, 1 << 24))
                        o.write(chunk)
                        left -= len(chunk)
                print("extracted", name, length)


if __name__ == "__main__":
    main()
