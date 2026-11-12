#!/usr/bin/env python3
"""Wrap the original static/system trust bundle with one owner module for the Mali Metal plug-in.

`unixnu payload --owner-trust-cache` replaces `--trust-cache`, so the bundle handed to the boot loader has
to carry the two original Apple modules unchanged *and* an owner module for the files the GPU add-on
installs: without it AMFI refuses to map the plug-in's code and Metal never sees the driver.

The container is the one DarwinBoot's ValidTrustCaches checks (drivers/DarwinBoot .../DarwinBoot.c):
a u32 module count, one u32 offset per module, then the modules back to back. A version-2 module is
<u32 2, uuid[16], u32 entries> and that many 24-byte entries (20-byte cdhash, then flags), and a module's
entries must be strictly ascending -- the kernel binary-searches them.

    scripts/build-gpu-trust-bundle.py ORIGINAL.bundle --cdhash HEX [--cdhash HEX ...] --out FILE

The original bundle's digest is not pinned here on purpose: this board builds its payload from whatever
`build-original-system-trust.py` produced, and the payload step is the place that pins the kernel.
"""
import argparse
import hashlib
import struct
import sys
import uuid
from pathlib import Path


def modules_of(bundle: bytes):
    count = struct.unpack_from('<I', bundle)[0]
    if not 0 < count <= 128:
        sys.exit(f'{count} modules in the original bundle: not a trust bundle')
    starts = [struct.unpack_from('<I', bundle, 4 + 4 * i)[0] for i in range(count)]
    out = []
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < count else len(bundle)
        out.append(bundle[start:end])
    return out


def check(bundle: bytes) -> bool:
    count = struct.unpack_from('<I', bundle)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from('<I', bundle, 4 + 4 * index)[0]
        end = struct.unpack_from('<I', bundle, 8 + 4 * index)[0] if index + 1 < count else len(bundle)
        if start < 4 + 4 * count or start & 3 or end > len(bundle) or end - start < 24 or struct.unpack_from('<I', bundle, start)[0] != 2:
            return False
        entries = struct.unpack_from('<I', bundle, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= bundle[at + 20] <= 4 or (entry and bundle[at - 24:at - 4] >= bundle[at:at + 20]):
                return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('original', type=Path, help='the original static/system trust bundle')
    parser.add_argument('--cdhash', action='append', default=[], metavar='HEX',
                        help='a 20-byte cdhash to trust, as 40 hex digits (repeatable)')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()

    original = args.original.read_bytes()
    originals = modules_of(original)
    hashes = sorted({bytes.fromhex(h) for h in args.cdhash})
    for h in hashes:
        if len(h) != 20:
            sys.exit(f'{h.hex()} is not a 20-byte cdhash')
    if len(hashes) != len(args.cdhash):
        sys.exit('duplicate cdhash')
    if not hashes:
        sys.exit('at least one --cdhash is needed')

    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'opi5plus-gpu-addon:' + ''.join(h.hex() for h in hashes))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
    modules = [own, *originals]

    cursor, starts = 4 + 4 * len(modules), []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    bundle = struct.pack('<%dI' % (1 + len(modules)), len(modules), *starts) + b''.join(modules)

    if not check(bundle):
        sys.exit('the trust bundle would be refused by the boot loader')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(bundle)
    print(f'{args.out}: {len(modules)} modules, {len(hashes)} owner cdhash(es), '
          f'sha256 {hashlib.sha256(bundle).hexdigest()}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
