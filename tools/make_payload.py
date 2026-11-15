"""Build the installer payload archive and resource script.

The installer is a plain Win32 executable: it has no Qt dependency, so the
only thing it needs is zlib (statically linked) to inflate this archive.

Archive layout (little endian):
    magic   : b"FLPAY1\\n\\0"            (8 bytes)
    count   : uint32
    entries : count * { uint32 pathLen, path(UTF-8),
                        uint32 rawLen, uint32 compLen }
    data    : the deflate streams, back to back, in entry order
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(ROOT, "stage")
OUT = os.path.join(ROOT, "installer")


def collect():
    files = []
    for base, _dirs, names in os.walk(STAGE):
        for name in names:
            full = os.path.join(base, name)
            rel = os.path.relpath(full, STAGE).replace("\\", "/")
            files.append((rel, full))
    files.sort()
    return files


def build():
    files = collect()
    if not files:
        raise SystemExit("stage/ is empty")

    headers = bytearray()
    blobs = bytearray()
    total_raw = 0
    for rel, full in files:
        raw = open(full, "rb").read()
        comp = zlib.compress(raw, 9)
        if len(comp) >= len(raw):
            # Storing is cheaper; the reader treats compLen == rawLen as stored.
            comp = raw
        name = rel.encode("utf-8")
        headers += struct.pack("<I", len(name)) + name
        headers += struct.pack("<II", len(raw), len(comp))
        blobs += comp
        total_raw += len(raw)

    archive = b"FLPAY1\n\x00" + struct.pack("<I", len(files)) + bytes(headers) + bytes(blobs)
    out = os.path.join(OUT, "payload.bin")
    with open(out, "wb") as fh:
        fh.write(archive)
    print("payload.bin: %d files, %.1f MB raw -> %.1f MB archive"
          % (len(files), total_raw / 1048576, len(archive) / 1048576))
    for rel, _ in files[:5]:
        print("   ", rel)
    return out


if __name__ == "__main__":
    build()
