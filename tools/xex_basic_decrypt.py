"""Decrypt a retail XEX2 that uses AES + basic (zero-fill) compression into a flat image
mapped at the load address.  Use it when xexdump writes an empty image.

    python -I tools/xex_basic_decrypt.py <default.xex> <out.bin>

The XEX's session key is encrypted with the console's retail XEX key, which this repository
doesn't include. Supply it yourself, as 32 hex digits, in either place:
  - the environment variable GRAW_XEX_KEY;
  - the file tools/xex_key.txt (ignored by git; nothing else in the file).
The key is never printed. Never commit it, paste it into an issue or put it in a log.
"""
import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEY_VARIABLE = "GRAW_XEX_KEY"
KEY_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xex_key.txt")


def retail_key():
    """The retail XEX key the user supplied (environment variable, then the local file)."""
    text, source = os.environ.get(KEY_VARIABLE, ""), KEY_VARIABLE
    if not text.strip() and os.path.exists(KEY_FILE):
        with open(KEY_FILE, encoding="ascii", errors="replace") as f:
            text, source = f.read(), KEY_FILE
    text = "".join(text.split())
    if not text:
        sys.exit(f"No retail XEX key: set {KEY_VARIABLE} or put it in {KEY_FILE} "
                 "(32 hex digits; it isn't included in this repository).")
    try:
        key = bytes.fromhex(text)
    except ValueError:
        key = b""
    if len(key) != 16:
        sys.exit(f"The key in {source} isn't 32 hex digits (16 bytes).")
    return key


def aes_cbc_decrypt(key, data):
    d = Cipher(algorithms.AES(key), modes.CBC(b"\0" * 16)).decryptor()
    return d.update(data) + d.finalize()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    xex = open(sys.argv[1], "rb").read()
    assert xex[:4] == b"XEX2", "not an XEX2"
    pe_off, _, sec_off, nhdr = struct.unpack_from(">IIII", xex, 8)
    headers = {k: v for k, v in (struct.unpack_from(">II", xex, 24 + i * 8) for i in range(nhdr))}
    ffi = headers[0x000003FF]
    info_size, enc, comp = struct.unpack_from(">IHH", xex, ffi)
    assert comp == 1, f"compression type {comp} is not basic"
    session = aes_cbc_decrypt(retail_key(), xex[sec_off + 0x150:sec_off + 0x160]) if enc == 1 else None
    payload = xex[pe_off:]
    payload = payload[:len(payload) // 16 * 16]
    if session:
        payload = aes_cbc_decrypt(session, payload)
    out, pos = bytearray(), 0
    for i in range((info_size - 8) // 8):
        data_size, zero_size = struct.unpack_from(">II", xex, ffi + 8 + i * 8)
        out += payload[pos:pos + data_size] + b"\0" * zero_size
        pos += data_size
    assert out[:2] == b"MZ", "decryption failed (no MZ header): check the key"
    open(sys.argv[2], "wb").write(out)
    print(f"wrote {len(out):,} bytes")


if __name__ == "__main__":
    main()
