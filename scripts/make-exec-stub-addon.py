#!/usr/bin/env python3
"""Add the activation stand-in to the GPU add-on blob.

The add-on that `unixnu payload --gpu-addon` hands to the kernel is a container of (path, mode, bytes) entries
that the port's add-on installer writes under /private/var/ngpu/ and then mounts over /System/Library/Extensions
(scripts/make-gpu-addon.py writes it). The payload therefore already has a way to deliver files to the Data
volume before launchd runs, and this reuses it for the one binary that has to replace a sealed daemon: the
entry addons/exec/mobileactivationd, which patch 0007 mounts over /usr/libexec.

    scripts/make-exec-stub-addon.py IN.addon STUB OUT.addon

The result is the input blob plus that one entry, and the script refuses paths that would escape /private/var/ngpu.
"""
import struct
import sys
import zlib
from pathlib import Path

HEADER = b'NGPUADD2'
PAD = lambda data: data + bytes(-len(data) % 8)


def entries_of(blob: bytes):
    if blob[:8] != HEADER:
        sys.exit('not an addon blob: %r' % blob[:8])
    count, _, total = struct.unpack_from('<IIQ', blob, 8)
    at = 24
    out = []
    for _ in range(count):
        path_len, mode, size, packed_size = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        path = blob[at:at + path_len].rstrip(b'\0').decode()
        at += (path_len + 7) // 8 * 8
        packed = blob[at:at + packed_size]
        at += (packed_size + 7) // 8 * 8
        out.append((path, mode, zlib.decompress(packed)))
    return out


def main() -> int:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source, stub, output = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    files = entries_of(source.read_bytes())
    wanted = 'addons/exec/mobileactivationd'
    data = stub.read_bytes()
    if not data.startswith(b'\xcf\xfa\xed\xfe'):
        sys.exit('%s is not a little-endian arm64 Mach-O' % stub)
    files = [(p, m, d) for (p, m, d) in files if p != wanted]
    files.append((wanted, 0o755, data))
    body = b''
    for path, mode, payload in files:
        if '..' in path.split('/') or path.startswith('/'):
            sys.exit('bad entry path ' + path)
        encoded = path.encode() + b'\0'
        if len(encoded) > 160:
            sys.exit('entry path too long: ' + path)
        packed = zlib.compress(payload, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(payload), len(packed)) + PAD(encoded) + PAD(packed)
    blob = HEADER + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    output.write_bytes(blob)
    print('%s: %d entries, %d bytes (%s is %d bytes)' % (output, len(files), len(blob), wanted, len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
