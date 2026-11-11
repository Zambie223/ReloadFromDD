"""Writes a copy of a save with its session name replaced by another of EXACTLY the same length.

The name is in the header (save name and session name) and in the compressed body (mSaveSessionName,
mReplicatedSessionName). Same length means no offset anywhere changes; each body chunk is recompressed at its
original boundary and only the chunk headers' compressed sizes change. The source file is never modified.
Usage: rename_session.py <source.sav> <dest.sav> <old name> <new name>
"""
import struct
import sys
import zlib

TAG = 0x9E2A83C1
src, dst, old, new = sys.argv[1:5]
old_b, new_b = old.encode("ascii"), new.encode("ascii")
assert len(old_b) == len(new_b), "names must have the same length"

data = open(src, "rb").read()
first = data.find(struct.pack("<I", TAG))
header = data[:first].replace(old_b, new_b)
header_hits = data[:first].count(old_b)

chunks = []  # (header bytes without sizes, uncompressed size)
body = bytearray()
pos = first
while pos < len(data):
    assert struct.unpack_from("<I", data, pos)[0] == TAG, f"bad chunk tag at {pos}"
    fixed = data[pos:pos + 17]
    comp, uncomp, comp2, uncomp2 = struct.unpack_from("<qqqq", data, pos + 17)
    assert comp == comp2 and uncomp == uncomp2
    raw = zlib.decompress(data[pos + 49: pos + 49 + comp])
    assert len(raw) == uncomp
    chunks.append((fixed, uncomp))
    body += raw
    pos += 49 + comp

body_hits = body.count(old_b)
new_body = bytes(body).replace(old_b, new_b)
assert len(new_body) == len(body)

out = bytearray(header)
offset = 0
for fixed, uncomp in chunks:
    piece = new_body[offset: offset + uncomp]
    offset += uncomp
    comp = zlib.compress(piece, 6)
    out += fixed + struct.pack("<qqqq", len(comp), uncomp, len(comp), uncomp) + comp
open(dst, "wb").write(out)
print(f"header hits {header_hits}, body hits {body_hits}, chunks {len(chunks)}, wrote {len(out)} bytes to {dst}")
