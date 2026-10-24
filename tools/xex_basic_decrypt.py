"""Decrypt a retail XEX2 that uses AES + basic (zero-fill) compression into a flat image
mapped at the load address.  Use it when xexdump writes an empty image.

    python -I tools/xex_basic_decrypt.py <default.xex> <out.bin>
"""
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RETAIL_KEY = bytes(16)  # removed from history: supply your own key


def aes_cbc_decrypt(key, data):
    d = Cipher(algorithms.AES(key), modes.CBC(b"\0" * 16)).decryptor()
    return d.update(data) + d.finalize()


def main():
    xex = open(sys.argv[1], "rb").read()
    assert xex[:4] == b"XEX2", "not an XEX2"
    pe_off, _, sec_off, nhdr = struct.unpack_from(">IIII", xex, 8)
    headers = {k: v for k, v in (struct.unpack_from(">II", xex, 24 + i * 8) for i in range(nhdr))}
    ffi = headers[0x000003FF]
    info_size, enc, comp = struct.unpack_from(">IHH", xex, ffi)
    assert comp == 1, f"compression type {comp} is not basic"
    session = aes_cbc_decrypt(RETAIL_KEY, xex[sec_off + 0x150:sec_off + 0x160]) if enc == 1 else None
    payload = xex[pe_off:]
    payload = payload[:len(payload) // 16 * 16]
    if session:
        payload = aes_cbc_decrypt(session, payload)
    out, pos = bytearray(), 0
    for i in range((info_size - 8) // 8):
        data_size, zero_size = struct.unpack_from(">II", xex, ffi + 8 + i * 8)
        out += payload[pos:pos + data_size] + b"\0" * zero_size
        pos += data_size
    assert out[:2] == b"MZ", "decryption failed (no MZ header)"
    open(sys.argv[2], "wb").write(out)
    print(f"wrote {len(out):,} bytes")


if __name__ == "__main__":
    main()
