"""Print an Xbox 360 executable's (XEX2) header: title, media, version, build time, original PE
name, image layout, entry point and the XDK libraries it links. Reads headers only.

    python -I tools/xexinfo.py <default.xex>
"""
import datetime
import struct
import sys

# Optional header keys (as in Xenia's xex2_info.h).
EXECUTION_INFO = 0x00040006
ORIGINAL_BASE = 0x00010001
ENTRY_POINT = 0x00010100
IMAGE_BASE = 0x00010201
CHECKSUM_TIMESTAMP = 0x00018002
ORIGINAL_PE_NAME = 0x000183FF
STATIC_LIBRARIES = 0x000200FF
IMPORT_LIBRARIES = 0x000103FF
FILE_FORMAT = 0x000003FF
STACK_SIZE = 0x00020200
HEAP_SIZE = 0x00020401
TITLE_WORKSPACE = 0x00040201
ALTERNATE_TITLES = 0x000407FF


def main():
    data = open(sys.argv[1], 'rb').read()
    magic, flags, pe_offset, _, security_offset, count = struct.unpack_from('>4sIIIII', data, 0)
    if magic != b'XEX2':
        raise SystemExit('not an XEX2 file')
    headers = {}
    for i in range(count):
        key, value = struct.unpack_from('>II', data, 24 + 8 * i)
        headers[key] = value

    def block(key):
        """The data of a header that points at a size-prefixed block."""
        offset = headers[key]
        size = struct.unpack_from('>I', data, offset)[0]
        return data[offset + 4:offset + size]

    print(f'module flags {flags:08X}, PE data at {pe_offset:#x}, {count} optional headers')
    if EXECUTION_INFO in headers:
        media, version, base_version, title, platform, table, disc, discs, save = struct.unpack_from(
            '>IIIIBBBBI', data, headers[EXECUTION_INFO])
        v = lambda x: f'{x >> 28}.{(x >> 24) & 0xF}.{(x >> 8) & 0xFFFF}.{x & 0xFF}'
        print(f'title {title:08X}, media {media:08X}, version {v(version)} (base {v(base_version)}), '
              f'disc {disc}/{discs}, savegame {save:08X}')
    if CHECKSUM_TIMESTAMP in headers:
        checksum, stamp = struct.unpack_from('>II', data, headers[CHECKSUM_TIMESTAMP])
        built = datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc)
        print(f'built {built:%Y-%m-%d %H:%M} UTC (PE checksum {checksum:08X})')
    if ORIGINAL_PE_NAME in headers:
        print('original PE name', block(ORIGINAL_PE_NAME).split(b'\0')[0].decode('latin-1'))
    for key, name in ((ORIGINAL_BASE, 'original base'), (IMAGE_BASE, 'image base'),
                      (ENTRY_POINT, 'entry point'), (STACK_SIZE, 'default stack'),
                      (HEAP_SIZE, 'default heap'), (TITLE_WORKSPACE, 'title workspace')):
        if key in headers:
            print(f'{name} {headers[key]:#010x}')
    # Security info: header size, image size, RSA signature, ..., load address.
    header_size, image_size = struct.unpack_from('>II', data, security_offset)
    load_address = struct.unpack_from('>I', data, security_offset + 0x110)[0]
    print(f'image size {image_size:#x}, load address {load_address:#010x}')
    if FILE_FORMAT in headers:
        encryption, compression = struct.unpack_from('>HH', data, headers[FILE_FORMAT] + 4)
        print(f'encryption {encryption} (1 = encrypted), compression {compression} '
              f'(1 = basic, 2 = LZX)')
    if ALTERNATE_TITLES in headers:
        ids = block(ALTERNATE_TITLES)
        print('alternate titles', ' '.join(f'{x:08X}' for x in struct.unpack(f'>{len(ids) // 4}I', ids)))
    if STATIC_LIBRARIES in headers:
        libs = block(STATIC_LIBRARIES)
        out = []
        for i in range(0, len(libs) - 15, 16):
            name = libs[i:i + 8].split(b'\0')[0].decode('latin-1')
            major, minor, build, qfe = struct.unpack_from('>HHHH', libs, i + 8)
            out.append(f'{name} {major}.{minor}.{build}.{qfe & 0xFF}')
        print('libraries:', ', '.join(out))


if __name__ == '__main__':
    main()
