#!/usr/bin/env python3
"""Bind NativeSdBlock to the original IOBlockStorageDevice of the pinned kernelcache.

NativeSdBlock is a built-in subclass of the IOBlockStorageDevice that lives in the original
IOStorageFamily of the ordinary iPadOS kernelcache: the family's own IOBlockStorageDriver,
partition schemes, IOMedia and APFS stay original and talk to this class through the 190-slot
vtable of that base. The compiled object therefore names a handful of symbols that exist only in
the original code (the protected constructor, the base destructor, the metaclass, the vtable and
the base methods the subclass does not override). The kernel does not export them, so this step
turns each of them into an absolute symbol at the address that scripts/audit-sd-storage.py
recorded, after checking the object against that record:

  * the vtable of NativeSdBlock has exactly 190 slots; every slot is either the base method of
    the recorded name or an own method of the same name; the 14 base methods that are pure
    virtual are all implemented here;
  * every undefined symbol that mentions IOBlockStorageDevice has a recorded address, and the
    abstract complete/deleting destructors (D1/D0, traps in the original) are never used.

It then adds the bindings object to build/native-sd/libNativeSd.a, which the kernel link
force-loads. Nothing in the original code is modified or relocated.

scripts/build-sd-driver.py deliberately leaves no archive: it compiles package/NativeSd/NativeSd.cpp
into build/native-sd/NativeSd.o and stops, so the kernel link can never pick up a driver that was
not bound. This script is the only producer of the archive.

  python3 scripts/bind-sd-storage.py                                  # kernel step, after build-sd-driver.py
  python3 scripts/bind-sd-storage.py --verify-kernel KERNEL.json      # after the link

--verify-kernel is the second half of the contract: the addresses are only valid for the kernel
image they were recorded from, so the finished kernel must still carry the same original
IOStorageFamily at the same addresses.

The addresses themselves are a property of the pinned original kernelcache (sha256
f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7), not of this board or of this
driver: see the header of scripts/audit-sd-storage.py.

Derived from UniXNU's scripts/build-virtio-mmio-bindings.py through UniXNU-evb10's
scripts/bind-emmc-storage.py, BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np  # noqa: E402

AUDIT = ROOT / 'scripts/data/opi5plus-storage-interface-audit.json'
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
CLASS = 'NativeSdBlock'
MODULE = 'com.apple.iokit.IOStorageFamily'
SLOTS = 190


def fail(message):
    raise SystemExit('bind-sd-storage: ' + message)


def load_audit():
    if not AUDIT.is_file():
        fail('%s is missing; run scripts/audit-sd-storage.py first' % AUDIT)
    try:
        audit = json.loads(AUDIT.read_text())
    except ValueError as error:
        fail('%s is not valid JSON (%s); regenerate it with scripts/audit-sd-storage.py' % (AUDIT, error))
    if audit.get('schema') != 'opi5plus-storage-interface-audit/1' or audit.get('method_slots') != SLOTS \
            or not audit.get('abstract_complete_destructors_trap'):
        fail('%s is not a usable storage audit; run scripts/audit-sd-storage.py' % AUDIT)
    return audit


def read_object(path):
    obj = path.read_bytes()
    image = np.macho(obj)
    offset = next(at for cmd, at, size in image['commands'] if cmd == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', obj, offset + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, symoff + i * 16)
        symbols.append((obj[stroff + ix:obj.index(b'\0', stroff + ix)].decode(), value, kind))
    return obj, image, symbols


def vtable_slots(obj, image, symbols, vtable):
    """The symbol every pointer of the first SLOTS vtable entries is relocated against."""
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                ro, rc = struct.unpack_from('<II', obj, section['command_offset'] + 56)
                for i in range(rc):
                    address, word = struct.unpack_from('<II', obj, ro + i * 8)
                    if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                        relocations[section['va'] + address] = symbols[word & 0xffffff][0]
    slots = [relocations.get(vtable + 16 + i * 8) for i in range(SLOTS)]
    if not all(slots) or vtable + 16 + SLOTS * 8 in relocations or '___cxa_pure_virtual' in slots:
        fail('%s has missing, additional or pure virtual methods' % CLASS)
    return slots


def demangle(names):
    tool = shutil.which('llvm-cxxfilt') or shutil.which('c++filt')
    if not tool:
        fail('no c++filt or llvm-cxxfilt on PATH')
    text = subprocess.check_output([tool], input='\n'.join(n[1:] for n in names) + '\n', text=True)
    return dict(zip(names, text.splitlines()))


def assembler():
    """The compiler the kernel itself was built with, taken from the recorded command."""
    record_path = ROOT / RECORD
    if not record_path.is_file():
        fail('%s is missing; the kernel step writes it on its first (bootstrap) pass, so run '
             '"unixnu build kernel" before this step' % record_path)
    record = json.loads(record_path.read_text().strip().rstrip(','))
    return record['arguments'][0], record['directory']


def archiver():
    for name in ('llvm-ar', 'ar'):
        found = shutil.which(name)
        if found:
            return found
    for candidate in ('/opt/homebrew/opt/llvm/bin/llvm-ar', '/usr/local/opt/llvm/bin/llvm-ar', '/usr/bin/llvm-ar'):
        if Path(candidate).is_file():
            return candidate
    fail('no llvm-ar on PATH; a Mach-O archive needs one')


def bind(directory):
    audit = load_audit()
    mangled = str(len(CLASS)) + CLASS
    object_path = directory / 'NativeSd.o'
    if not object_path.is_file():
        fail('%s is missing; run scripts/build-sd-driver.py NativeSd first' % object_path)
    obj, image, symbols = read_object(object_path)
    vtable = next((value for name, value, kind in symbols if name == '__ZTV' + mangled), None)
    if vtable is None:
        fail('%s defines no %s vtable' % (object_path, CLASS))
    compiled = vtable_slots(obj, image, symbols, vtable)
    names = list(dict.fromkeys(compiled + [row['reference_name'] for row in audit['methods']]))
    readable = demangle(names)
    own_prefixes = ('__ZN' + mangled, '__ZNK' + mangled)
    overrides = 0
    for row, actual in zip(audit['methods'], compiled):
        expected = row['reference_name']
        own = actual.startswith(own_prefixes)
        if expected == '___cxa_pure_virtual':
            if not own:
                fail('a required block operation is not implemented (slot %d)' % row['slot'])
        elif actual != expected:
            # slots 0 and 1 are the destructors, whose names differ with the class
            if not own or (row['slot'] not in (0, 1) and
                           readable[actual].split('::', 1)[1] != readable[expected].split('::', 1)[1]):
                fail('%s dispatch mismatch at slot %d: %s' % (CLASS, row['slot'], readable.get(actual, actual)))
        overrides += own

    # Only the slots that the candidate kernel does not export need an absolute address: the others
    # are ordinary kernel symbols the link resolves. The 14 non-exported ones point into the original
    # IOStorageFamily text, so they are a property of the original kernelcache.
    possible = {row['reference_name']: int(row['candidate_target'], 16) for row in audit['methods'] if not row['kernel_names']}
    possible.update({'__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': int(audit['original_protected_constructor'], 16),
                     '__ZN20IOBlockStorageDeviceD2Ev': int(audit['original_base_destructor'], 16),
                     '__ZN20IOBlockStorageDevice10gMetaClassE': int(audit['original_metaclass'], 16),
                     '__ZTV20IOBlockStorageDevice': int(audit['vtable'], 16)})
    undefined = [name for name, value, kind in symbols if kind == 1 and 'IOBlockStorageDevice' in name]
    unknown = [name for name in undefined if name not in possible]
    if unknown:
        fail('unqualified original storage import: ' + ', '.join(unknown))
    # The abstract complete/deleting destructors are traps in the original; only the base D2 may be used.
    if any(re.search(r'IOBlockStorageDeviceD[01]Ev$', name) for name in undefined):
        fail('%s must not invoke the abstract complete/deleting destructors' % CLASS)
    bindings = {name: possible[name] for name in undefined}

    assembly = directory / 'original-bindings.S'
    assembly.write_text(''.join('.globl %s\n.set %s, %#x\n' % (name, name, value) for name, value in sorted(bindings.items())))
    clang, cwd = assembler()
    bound = directory / 'original-bindings.o'
    bound.unlink(missing_ok=True)
    result = subprocess.run([clang, '--target=arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot', '-c',
                             str(assembly), '-o', str(bound)], cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        fail('%s rejected %s:\n%s' % (clang, assembly, (result.stdout + result.stderr)[-3000:]))
    archive = directory / 'libNativeSd.a'
    archive.unlink(missing_ok=True)
    ar = archiver()
    result = subprocess.run([ar, 'rcs', str(archive), str(object_path), str(bound)], capture_output=True, text=True)
    if result.returncode:
        fail('%s could not write %s:\n%s' % (ar, archive, (result.stdout + result.stderr)[-3000:]))
    (directory / 'bindings.json').write_text(json.dumps(dict(
        audit=str(AUDIT.relative_to(ROOT)), original_sha256=audit['original_sha256'],
        audited_candidate_sha256=audit['audited_candidate_sha256'],
        compiled_object_sha256=hashlib.sha256(obj).hexdigest(),
        virtual_methods_checked=SLOTS, concrete_overrides=overrides, unresolved_original_imports=0,
        bindings={name: hex(value) for name, value in sorted(bindings.items())},
        original_code_modified=False, actual_io_qualified=False), indent=2) + '\n')
    print('%s: %d slots, %d own methods, %d original imports bound -> %s' % (CLASS, SLOTS, overrides, len(bindings), archive))


def verify_kernel(path):
    """The kernel that was linked must carry the same original IOStorageFamily at the recorded addresses."""
    audit = load_audit()
    path = Path(path)
    if not path.is_file():
        fail('%s is missing; run "unixnu build kernel" first' % path)
    plan = json.loads(path.read_text())
    if plan['original_sha256'] != audit['original_sha256']:
        fail('the kernel is not built from the original kernelcache the storage audit was recorded for')
    extents = {e['segment']: e for e in plan['extents'] if e['module'] == MODULE}
    for segment, record in audit['storage_extents'].items():
        actual = extents.get(segment)
        if not actual or hex(actual['va']) != record['va'] or hex(actual['size']) != record['size']:
            fail('original IOStorageFamily %s moved: recorded %s+%s, kernel has %s' % (
                segment, record['va'], record['size'], actual and (hex(actual['va']), hex(actual['size']))))
    if plan['original_executable_sha256'][MODULE] != audit['storage_text_sha256']:
        fail('original IOStorageFamily executable code differs from the audited one')

    def inside(address, segment):
        record = audit['storage_extents'][segment]
        return int(record['va'], 16) <= address < int(record['va'], 16) + int(record['size'], 16)

    for name, value in (('vtable', audit['vtable']), ):
        if not inside(int(value, 16), '__DATA_CONST'):
            fail('recorded %s is outside the original __DATA_CONST' % name)
    if not inside(int(audit['original_metaclass'], 16), '__DATA'):
        fail('recorded metaclass is outside the original __DATA__')
    for value in [audit['original_protected_constructor'], audit['original_base_destructor']] + \
            [row['candidate_target'] for row in audit['methods'] if not row['kernel_names']]:
        if not inside(int(value, 16), '__TEXT_EXEC'):
            fail('recorded function %s is outside the original __TEXT_EXEC' % value)
    print('PASS: the kernel carries the audited original IOStorageFamily (%s), %d bound addresses inside it' % (
        plan['original_sha256'][:16], sum(1 for row in audit['methods'] if not row['kernel_names']) + 4))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--directory', type=Path, default=ROOT / 'build/native-sd',
                        help='directory of NativeSd.o; the archive and bindings.json are written here')
    parser.add_argument('--verify-kernel', metavar='KERNEL.json', help='check a linked kernel against the audit instead of binding')
    args = parser.parse_args()
    if args.verify_kernel:
        verify_kernel(args.verify_kernel)
    else:
        bind(args.directory.resolve())


if __name__ == '__main__':
    main()
