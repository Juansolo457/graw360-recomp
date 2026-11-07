"""PGO profile preparation for tools/pgo-merge.sh (from fear360-recomp).

    python3 -I tools/pgo_prepare.py last-snapshot <out folder> <.profraw>...
        Writes each raw profile's last snapshot to <out folder>/<n>.profraw. A training build
        before src/pgo.cpp's temporary-file writes appended each 20 s snapshot to the same file
        (the profile runtime opens it for appending), each holding the counts so far: only the
        last is the session's total. A file with one snapshot is copied as is.

    python3 -I tools/pgo_prepare.py drop-unreached <in.proftext> <out.proftext>
        Removes the functions whose counters are all zero from a text profile (llvm-profdata
        merge --text). LLVM marks a function with a zero-count record cold; without a record it
        is compiled as with no profile, which suits game code the training didn't reach.
"""
import os
import sys

# The raw profile header's first eight bytes (INSTR_PROF_RAW_MAGIC_64, little-endian).
MAGIC = bytes.fromhex('8172666f72706cff')


def last_snapshot(out_dir, paths):
    """Copies each raw profile that is a complete snapshot into out_dir, numbered."""
    os.makedirs(out_dir, exist_ok=True)
    for n, path in enumerate(paths):
        with open(path, 'rb') as f:
            data = f.read()
        if not data.startswith(MAGIC):
            raise SystemExit(f'{path}: not a raw profile')
        last = data.rfind(MAGIC)
        out = os.path.join(out_dir, f'{n}.profraw')
        with open(out, 'wb') as f:
            f.write(data[last:])
        print(f'{path}: {data.count(MAGIC)} snapshot(s), kept the last ({len(data) - last} bytes)')


def drop_unreached(src, dst):
    """Writes the text profile at src to dst without the functions no training run reached."""
    with open(src, encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()
    # The header (":ir" and its comment, written straight above the first function with no
    # blank line between them) is kept apart, so dropping that function can't drop it.
    header = []
    lines = text.split('\n')
    while lines and (lines[0].startswith('#') or lines[0].startswith(':')):
        header.append(lines.pop(0))
    blocks = '\n'.join(lines).split('\n\n')
    kept, dropped, functions = [], 0, 0
    for block in blocks:
        lines = block.split('\n')
        if '# Counter Values:' not in lines:
            kept.append(block)  # trailing whitespace
            continue
        start = lines.index('# Counter Values:') + 1
        values = []
        for line in lines[start:]:
            if line.startswith('#') or not line.strip():
                break
            values.append(line)
        if values and all(v.strip() == '0' for v in values):
            dropped += 1
        else:
            kept.append(block)
            functions += 1
    with open(dst, 'w', encoding='utf-8', errors='surrogateescape') as f:
        # As llvm-profdata wrote it: the header straight above the first function.
        f.write('\n'.join(header + ['\n\n'.join(kept)]))
    print(f'dropped {dropped} unreached function(s), kept {functions}')


def main():
    """Runs the subcommand named on the command line: last-snapshot or drop-unreached."""
    if len(sys.argv) >= 4 and sys.argv[1] == 'last-snapshot':
        last_snapshot(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) == 4 and sys.argv[1] == 'drop-unreached':
        drop_unreached(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
