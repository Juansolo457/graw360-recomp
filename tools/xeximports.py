"""List an Xbox 360 executable's kernel imports (xboxkrnl.exe / xam.xex ordinals) and check
them against a ReXGlue SDK source tree: which ones have no implementation there.

    python -I tools/xeximports.py <default.xex> <decrypted image at 0x82000000> [<rexglue-sdk src>]

The decrypted image comes from tools/xex_basic_decrypt.py. Names come from the SDK's export
tables (Xenia's), so without an SDK path only the ordinals are printed.
"""
import glob
import re
import struct
import sys

BASE = 0x82000000
IMPORT_LIBRARIES = 0x000103FF


def import_ordinals(xex, img):
    count = struct.unpack_from('>I', xex, 20)[0]
    headers = dict(struct.unpack_from('>II', xex, 24 + 8 * i) for i in range(count))
    off = headers[IMPORT_LIBRARIES]
    _, string_table_size, lib_count = struct.unpack_from('>III', xex, off)
    names = [n.decode() for n in xex[off + 12:off + 12 + string_table_size].split(b'\0') if n]
    p = off + 12 + string_table_size
    result = {}
    for _ in range(lib_count):
        size, = struct.unpack_from('>I', xex, p)
        name_index, n = struct.unpack_from('>HH', xex, p + 36)
        ords = set()
        for address in struct.unpack_from(f'>{n}I', xex, p + 40):
            value, = struct.unpack_from('>I', img, address - BASE)
            if (value >> 24) & 0xFF == 0:        # record type 0 names the import; 1 is its thunk
                ords.add(value & 0xFFFF)
        result[names[name_index].split('.')[0]] = sorted(ords)
        p += size
    return result


def export_table(sdk, lib):
    table = {}
    for line in open(f'{sdk}/src/kernel/{lib}/export_table.inc', encoding='utf-8', errors='replace'):
        m = re.match(r'XE_EXPORT\(\w+,\s*0x([0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)', line.strip())
        if m:
            table[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return table


def main():
    xex = open(sys.argv[1], 'rb').read()
    img = open(sys.argv[2], 'rb').read()
    sdk = sys.argv[3] if len(sys.argv) > 3 else None
    for lib, ords in import_ordinals(xex, img).items():
        print(f'== {lib}: {len(ords)} imports')
        if not sdk:
            print(' '.join(map(str, ords)))
            continue
        table = export_table(sdk, lib)
        source = ''.join(open(f, encoding='utf-8', errors='replace').read()
                         for f in glob.glob(f'{sdk}/src/kernel/{lib}/**/*.cpp', recursive=True))
        missing = []
        for o in ords:
            name, kind = table.get(o, (f'ordinal_{o}', '?'))
            implemented = re.search(r'EXPORT\w*\s*\(\s*(__imp__)?' + re.escape(name) + r'\b', source) \
                or re.search(r'\b' + re.escape(name) + r'_entry\b', source)
            print(f'  {o:4} {name}{"" if implemented else "   <- no implementation found"}')
            if not implemented:
                missing.append(f'{name} ({kind})')
        print(f'-- {len(missing)} without an implementation: {", ".join(missing)}')


if __name__ == '__main__':
    main()
