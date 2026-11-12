#!/usr/bin/env python3
"""Semantic-equivalence judge for two prelinked opi5plus kernel.macho builds.

Byte-compares the two images after masking the metadata a rebuild may lawfully
change (verified against the 2026-11-12 reproducibility experiment, see
evidence/sd-clock-investigation.md S6):

  - LC_UUID payloads: an identifying UUID per prelinked kext / the kernel,
  - N_OSO stab values (n_type 0x66): ld64 records each linked object path in
    the symbol table with the object file's mtime as n_value; incremental
    rebuilds rewrite some .o files, so these stamps move with wall-clock time
    (467 of them in one build). SOURCE_DATE_EPOCH does not reach them.
  - _PrelinkKCID values in __PRELINK_INFO: derived by kits/XnuPrelink/
    emit_prelink.py as sha256(kernel + original)[:16], so they change wheneve
    any masked byte above changes, even if code is identical.

Usage: python3 compare-kernel.py FILE_A FILE_B [--verbose]
Exit status 0 = semantically equivalent (only masked metadata differs),
1 = real differences remain (they are listed).
"""
import argparse
import hashlib
import re
import struct
import sys

M = bytearray()  # global mask map for residual-diff reporting


def load_commands(data):
    if struct.unpack_from('<III', data) != (0xfeedfacf, 0x0100000c, 0):
        raise ValueError('expected a 64-bit ARM64 Mach-O')
    at, out = 32, []
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if size < 8 or at + size > len(data):
            raise ValueError(f'malformed load command at 0x{at:x}')
        out.append((command, size, at))
        at += size
    return out


def masked(data, verbose=False):
    """Return (bytes with volatile metadata zeroed, dict of masked-field counts)."""
    b = bytearray(data)
    counts = {'uuid': 0, 'oso_timestamps': 0, 'prelink_kcid': 0}
    for command, size, at in load_commands(b):
        if command == 0x1b:  # LC_UUID
            if size != 24:
                raise ValueError(f'malformed LC_UUID at 0x{at:x}')
            b[at + 8:at + 24] = bytes(16)
            counts['uuid'] += 1
        elif command == 0x02:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', b, at + 8)
            if symoff + nsyms * 16 > len(b) or stroff + strsize > len(b):
                raise ValueError(f'malformed LC_SYMTAB at 0x{at:x}')
            for n in range(nsyms):
                item = symoff + n * 16
                if b[item + 4] == 0x66:  # N_OSO: object-path entry, value is the .o mtime
                    b[item + 8:item + 16] = bytes(8)
                    counts['oso_timestamps'] += 1
        elif command == 0x19 and bytes(b[at + 8:at + 24]).rstrip(b'\0') == b'__PRELINK_INFO':
            offset, length = struct.unpack_from('<QQ', b, at + 40)
            if offset + length > len(b):
                raise ValueError('malformed __PRELINK_INFO')
            for m in re.finditer(rb'<key>_PrelinkKCID</key><data>([A-Za-z0-9+/]{22}==)</data>',
                                 bytes(b[offset:offset + length])):
                lo, hi = m.span(1)
                b[offset + lo:offset + hi] = b'0' * (hi - lo)
                counts['prelink_kcid'] += 1
    if not counts['uuid'] or not counts['oso_timestamps'] or not counts['prelink_kcid']:
        raise ValueError(f'unexpected metadata counts: {counts} (not a prelinked kernel?)')
    return bytes(b), counts


def residual_regions(x, y, gap=64):
    cl = []
    n = min(len(x), len(y))
    for i in range(n):
        if x[i] != y[i]:
            if cl and i - cl[-1][1] <= gap:
                cl[-1][1] = i + 1
            else:
                cl.append([i, i + 1])
    if len(x) != len(y):
        cl.append([n, max(len(x), len(y))])
    return cl


def main():
    p = argparse.ArgumentParser(description='semantic-equivalence judge for prelinked kernels')
    p.add_argument('file_a')
    p.add_argument('file_b')
    p.add_argument('--verbose', action='store_true', help='list every residual diff region')
    args = p.parse_args()
    a = open(args.file_a, 'rb').read()
    b = open(args.file_b, 'rb').read()
    xa, ca = masked(a)
    xb, cb = masked(b)
    print(f'A sha256 {hashlib.sha256(a).hexdigest()} ({len(a)} B)')
    print(f'B sha256 {hashlib.sha256(b).hexdigest()} ({len(b)} B)')
    print(f'masked in A: {ca}')
    if ca != cb:
        print('NOT EQUIVALENT: masked-field counts differ (different build structure)')
        return 1
    if xa == xb:
        print('SEMANTICALLY EQUIVALENT: identical beyond LC_UUID / N_OSO mtimes / _PrelinkKCID')
        print(f'normalized sha256 {hashlib.sha256(xa).hexdigest()}')
        return 0
    cl = residual_regions(xa, xb)
    print(f'NOT EQUIVALENT: {len(cl)} residual region(s), {sum(e - s for s, e in cl)} byte(s) '
          'beyond the masked metadata:')
    for s, e in cl:
        head = xa[s:s + 24].hex(' ')
        yhead = xb[s:s + 24].hex(' ')
        print(f'  0x{s:08x}-0x{e:08x} ({e - s} B) A: {head}')
        print(f'                              B: {yhead}')
        if not args.verbose and len(cl) > 40:
            print('  ... use --verbose for the full list')
            break
    return 1


if __name__ == '__main__':
    sys.exit(main())
