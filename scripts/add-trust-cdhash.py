#!/usr/bin/env python3
"""Add one cdhash to the owner module of a payload trust bundle.

The bundle DarwinBoot hands to the kernel is a flat container of modules, and the port's payload carries two
Apple modules plus one owner module that lists the files the GPU add-on installs (scripts/build-gpu-trust-bundle.py
builds it from scratch; this edits the finished bundle in place, which is what a payload that only adds one binary
needs). Entries must stay strictly ascending inside a module, because the kernel binary-searches them.

    scripts/add-trust-cdhash.py BUNDLE CDHASH_HEX [--module N]

Without --module the last module (the owner's) gets the entry.
"""
import struct
import sys
from pathlib import Path

ENTRY = 24


def modules_of(bundle: bytes):
    count = struct.unpack_from('<I', bundle)[0]
    if not 0 < count <= 128:
        sys.exit('%d modules: not a trust bundle' % count)
    starts = [struct.unpack_from('<I', bundle, 4 + 4 * i)[0] for i in range(count)]
    out = []
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < count else len(bundle)
        out.append(bundle[start:end])
    return out


def entries_of(module: bytes):
    if struct.unpack_from('<I', module)[0] != 2:
        sys.exit('module is not version 2')
    count = struct.unpack_from('<I', module, 20)[0]
    if len(module) != 24 + count * ENTRY:
        sys.exit('module length %d does not match %d entries' % (len(module), count))
    return [module[24 + i * ENTRY:24 + (i + 1) * ENTRY] for i in range(count)]


def main() -> int:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    bundle_path = Path(sys.argv[1])
    cdhash = bytes.fromhex(sys.argv[2])
    if len(cdhash) != 20:
        sys.exit('a cdhash is 20 bytes, got %d' % len(cdhash))
    modules = modules_of(bundle_path.read_bytes())
    index = len(modules) - 1
    if '--module' in sys.argv:
        index = int(sys.argv[sys.argv.index('--module') + 1])
    module = modules[index]
    entries = entries_of(module)
    new_entry = cdhash + module[24 + 20:24 + 24]      # keep the flags byte of the first entry
    if any(e[:20] == cdhash for e in entries):
        print('%s is already in module %d' % (sys.argv[2], index))
        return 0
    entries.append(new_entry)
    entries.sort(key=lambda e: e[:20])
    rebuilt = module[:20] + struct.pack('<I', len(entries)) + b''.join(entries)
    modules[index] = rebuilt
    count = len(modules)
    header = struct.pack('<I', count) + b'\0' * (4 * count)
    offsets, blob = [], b''
    for m in modules:
        offsets.append(len(header) + len(blob))
        blob += m
    out = struct.pack('<I', count) + b''.join(struct.pack('<I', o) for o in offsets) + blob
    bundle_path.write_bytes(out)
    print('%s: module %d now has %d entries (%s added), bundle %d bytes'
          % (bundle_path, index, len(entries), sys.argv[2], len(out)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
