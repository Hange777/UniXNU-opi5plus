#!/usr/bin/env python3
"""Bind an owned IOHIDDevice subclass to the unchanged original IOHIDFamily, and archive it.

build-orangepi5-hid.py of UniXNU-orangepi5 (itself build-sheng-hid-input.py of UniXNU-sheng, BSD-2-Clause-Patent,
Copyright (c) 2026, UniXNU contributors). The mechanism is theirs unchanged: NativeHidInput is an IOHIDDevice
subclass over opaque storage, its metaclass initializer is deferred out of __mod_init_func so the probe can run it
after the original IOHIDFamily is loaded, and every imported IOHIDDevice method is bound to the original vtable
entry or constructor.

The difference is that this board's script finds those addresses instead of carrying them: the Orange Pi 5 script
hard-codes the absolute addresses of the kernel it was written against (vtable, constructor, destructor,
gMetaClass). Here they are derived from the reference candidate that build-display-abi.py copies (the display-less
kernel whose prelink plan carries com.apple.iokit.IOHIDFamily):

  * the 207-slot IOHIDDevice vtable by its fingerprint -- the slots the class overrides on IOService and slot 180
    (__cxa_pure_virtual) -- inside the module's own __DATA_CONST extent;
  * the protected constructor by its instruction bytes inside the module's __TEXT_EXEC extent, and the destructor
    0x20 after it;
  * gMetaClass as the OSMetaClass instance in the module's __DATA whose superClassLink is IOService::gMetaClass
    and whose classSize is the 0xd0 instance size.

The content checks of the original script stay: the original IOHIDFamily instruction hash must equal the plan's,
the vtable must have exactly 207 slots with slot 180 pure, NativeHidInput may override only the ten qualified
slots, and the object may import only IOHIDDevice symbols we bind.

Run in the build tree, after build-native-legacy-display.py (scripts/build-opi5plus-display.py does that).
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/opi5-hid'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports   # noqa: E402


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=180, **kw)


def require(value, why):
    if not value:
        raise SystemExit(why)


CANDIDATE = ROOT / 'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
require(CANDIDATE.exists(), 'No reference candidate available (build-display-abi.py makes it)')
image = CANDIDATE.read_bytes()
plan = json.loads(CANDIDATE.with_suffix('.json').read_text())
image_map = macho(image)
require(hashlib.sha256(image).hexdigest() == plan['candidate_sha256'], 'Reference candidate matches its plan')
symbols = {name: address for address, names in exports(image, image_map).items() for name in names}

HID_TEXT_SHA = plan['original_executable_sha256'].get('com.apple.iokit.IOHIDFamily')
require(HID_TEXT_SHA, 'The prelink plan must carry com.apple.iokit.IOHIDFamily')
extents = {e['segment']: e for e in plan['extents'] if e['module'] == 'com.apple.iokit.IOHIDFamily'}
for segment in ('__TEXT_EXEC', '__DATA_CONST', '__DATA'):
    require(segment in extents, 'IOHIDFamily has no ' + segment)


def read(address, size):
    return image[backed_offset(image_map['segments'], address, size, len(image)):][:size]


def u64(address):
    return struct.unpack('<Q', read(address, 8))[0]


# The original module's instructions, byte for byte, as the plan recorded them.
text = extents['__TEXT_EXEC']
require(hashlib.sha256(read(text['va'], text['size'])).hexdigest() == HID_TEXT_SHA,
        'Complete original HID instructions unchanged')

# 1. The 207-slot IOHIDDevice vtable, found by the slots the class overrides on IOService.
ISOSERVICE_METHODS = 168
base = struct.unpack('<168Q', read(symbols['__ZTV9IOService'] + 16, ISOSERVICE_METHODS * 8))
pure = symbols['___cxa_pure_virtual']
# The native-base slots IOHIDDevice overrides -- a property of the class, checked against the table we find.
CHANGED = [0, 1, 7, 12, 14, 21, 23, 76, 81, 82, 86, 87, 91, 92, 93, 101, 132, 140]
SLOTS = 207
TABLE_SLOT = 180
INSTANCE = 0xd0
data_const = extents['__DATA_CONST']
vptr = None
for offset in range(0, data_const['size'] - SLOTS * 8 + 1, 8):
    address = data_const['va'] + offset
    if u64(address + TABLE_SLOT * 8) != pure:
        continue
    table = struct.unpack('<207Q', read(address, SLOTS * 8))
    if not all(table):
        continue
    if any((table[i] == base[i]) == (i in CHANGED) for i in range(ISOSERVICE_METHODS)):
        continue
    vptr = address
    break
require(vptr, 'No 207-slot IOHIDDevice vtable in the original module')
methods = struct.unpack('<207Q', read(vptr, SLOTS * 8))
require([i for i, a in enumerate(methods) if a == pure] == [TABLE_SLOT], 'Exact original 207-slot abstract device')
require(read(vptr - 16, 8) == bytes(8), 'The vtable has the usual zero offset-to-top')

# 2. The protected constructor, by its instructions, and the destructor 0x20 after it.
CTOR = bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6')
text_bytes = read(text['va'], text['size'])
found = [text['va'] + i for i in range(0, len(text_bytes) - len(CTOR) + 1, 4)
         if text_bytes[i:i + len(CTOR)] == CTOR]
require(len(found) == 1, 'Expected exactly one IOHIDDevice constructor, found %d' % len(found))
ctor = found[0]
dtor = ctor + 0x20

# 3. The metaclass needs no address: NativeHidInput takes its superclass from the OSMetaClass registry at run time
# (package/Opi5plusHid/NativeHidInput.cpp explains why), because this kernel does not export IOHIDDevice::gMetaClass.

print('IOHIDDevice: vtable %#x constructor %#x destructor %#x (metaclass resolved at run time)' % (vptr, ctor, dtor))

# The opaque header: the original class' storage and the exact slot prototypes NativeHidInput may use.
HEADER = ROOT / 'build/opi5-hid/OriginalHidDeviceABI.h'
header = '''// Generated from the qualified original image; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
for index in range(ISOSERVICE_METHODS, SLOTS):
    if index == 171:
        header += ' virtual OSString *newTransportString() const;\n'
    elif index == 173:
        header += ' virtual OSString *newProductString() const;\n'
    elif index == TABLE_SLOT:
        header += ' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
    elif index == 181:
        header += ' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
    else:
        header += (' virtual __attribute__((deprecated("Unqualified prototype: never call from source")))'
                   ' void opaque%d();\n' % index)
header += '};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'
HEADER.write_text(header)

# The owned device object, compiled by the board driver script against the original kernel's flags.
print(run([sys.executable, ROOT / 'scripts/build-opi5plus-drivers.py', 'NativeHidInput'], text=True), end='')
obj = OUT / 'NativeHidInput.o'
raw = obj.read_bytes()
image_map = macho(raw)
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
symbol_table = next(p for c, p, _ in image_map['commands'] if c == 0x2)
symoff, nsyms, stroff, _ = struct.unpack_from('<4I', raw, symbol_table + 8)
syms = []
for index in range(nsyms):
    strx, kind, section, _, address = struct.unpack_from('<IBBHQ', raw, symoff + index * 16)
    name = raw[stroff + strx:raw.index(b'\0', stroff + strx)].decode()
    syms.append((name, address, kind))
vt = next(address for name, address, _ in syms if name == '__ZTV14NativeHidInput')
relocations = {}
for segment in image_map['segments']:
    for section in segment['sections']:
        if section['va'] <= vt < section['va'] + section['size']:
            reloff, relcount = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
            for index in range(relcount):
                at, bits = struct.unpack_from('<2I', raw, reloff + index * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[section['va'] + at] = syms[bits & 0xffffff][0]
compiled = [relocations.get(vt + 16 + index * 8) for index in range(SLOTS)]
require(all(compiled) and vt + 16 + SLOTS * 8 not in relocations, 'Compiled exact 207-slot vtable')

bindings = {'__ZN11IOHIDDeviceC2EPK11OSMetaClass': ctor,
            '__ZN11IOHIDDeviceD2Ev': dtor,
            '__ZTV11IOHIDDevice': vptr - 16}
owned = []
for index, name in enumerate(compiled):
    if name.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput')):
        owned.append(index)
    elif '11IOHIDDevice' in name:
        bindings[name] = methods[index]
    else:
        require(index < ISOSERVICE_METHODS and symbols.get(name) == methods[index],
                'Actual inherited native method ' + str(index))
require(owned == [0, 1, 7, 14, 86, 87, 111, 171, 173, 180], 'Only qualified HID methods overridden')
undefined = {name for name, _, kind in syms if kind == 1 and '11IOHIDDevice' in name}
require(undefined == set(bindings), 'All and only actual original HID imports bound: %s' % (undefined ^ set(bindings)))

# The bindings become assembly, assembled with the recorded kernel compiler so the Mach-O matches the object.
assembly = OUT / 'original-bindings.S'
assembly.write_text(''.join('.globl %s\n.set %s, %#x\n' % (name, name, address)
                            for name, address in sorted(bindings.items())))
run([record['arguments'][0], '--target=arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot',
     '-c', assembly, '-o', OUT / 'hid-original-bindings.o'], cwd=record['directory'])


def section_remover():
    """An llvm-objcopy that can remove a Mach-O section: $UNIXNU_LLVM_OBJCOPY, then PATH."""
    import os
    import tempfile
    for candidate in [os.environ.get('UNIXNU_LLVM_OBJCOPY'), shutil.which('llvm-objcopy')]:
        if not candidate or not Path(candidate).is_file():
            continue
        probe = subprocess.run([candidate, '--remove-section=__DATA,__mod_init_func', str(obj),
                                str(Path(tempfile.gettempdir()) / 'opi5plus-hid-objcopy-probe.o')],
                               capture_output=True)
        if probe.returncode == 0:
            return candidate
    raise SystemExit('no llvm-objcopy that removes Mach-O sections: set UNIXNU_LLVM_OBJCOPY')


def globalize(path, name):
    """Make the local initializer symbol external (llvm-objcopy --globalize-symbol is ELF only), keeping the
    symbol table ordered the way clang wrote it and following the swap through every relocation."""
    data = bytearray(Path(path).read_bytes())
    info = macho(bytes(data))
    table = next(p for c, p, _ in info['commands'] if c == 0x2)
    dynamic = next(p for c, p, _ in info['commands'] if c == 0xb)
    symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, table + 8)
    ilocal, nlocal, iext, next_, _, _ = struct.unpack_from('<6I', data, dynamic + 8)

    def entry(index):
        return symoff + 16 * index

    def called(index):
        strx = struct.unpack_from('<I', data, entry(index))[0]
        return bytes(data[stroff + strx:data.index(b'\0', stroff + strx)]).decode()

    found = [i for i in range(nsyms) if called(i) == name]
    require(len(found) == 1, 'symbol ' + name + ' not found exactly once in ' + str(path))
    index = found[0]
    require(data[entry(index) + 4] & 0xef == 0x0e, name + ' is not a defined local symbol (N_SECT, not N_EXT)')
    require(ilocal <= index < ilocal + nlocal and iext == ilocal + nlocal, 'unexpected LC_DYSYMTAB layout')
    last = ilocal + nlocal - 1
    if index != last:
        a, b = bytes(data[entry(index):entry(index) + 16]), bytes(data[entry(last):entry(last) + 16])
        data[entry(index):entry(index) + 16] = b
        data[entry(last):entry(last) + 16] = a
        remap = {index: last, last: index}
        for segment in info['segments']:
            for section in segment['sections']:
                reloff, count = struct.unpack_from('<2I', data, section['command_offset'] + 56)
                for k in range(count):
                    at = reloff + 8 * k
                    address, bits = struct.unpack_from('<2I', data, at)
                    require(not address >> 31, 'scattered relocation')
                    if bits >> 27 & 1 and bits & 0xffffff in remap:
                        struct.pack_into('<I', data, at + 4, (bits & ~0xffffff) | remap[bits & 0xffffff])
        indirect, count = struct.unpack_from('<2I', data, dynamic + 56)
        for k in range(count):
            value = struct.unpack_from('<I', data, indirect + 4 * k)[0]
            if value in remap:
                struct.pack_into('<I', data, indirect + 4 * k, remap[value])
    data[entry(last) + 4] |= 1
    struct.pack_into('<4I', data, dynamic + 8, ilocal, nlocal - 1, last, next_ + 1)
    Path(path).write_bytes(bytes(data))


def late_object(source, target, initializer):
    """The object without __mod_init_func -- the probe runs the initializer between preModLoad and postModLoad -- and
    with that initializer external."""
    run([remover, '--remove-section=__DATA,__mod_init_func', source, target])
    globalize(target, initializer)


remover = section_remover()
late = OUT / 'NativeHidInput.late.o'
late_object(obj, late, '__GLOBAL__sub_I_NativeHidInput.cpp')


def text_section(data, info):
    section = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[section['fileoff']:section['fileoff'] + section['size']]


require(text_section(raw, image_map) == text_section(late.read_bytes(), macho(late.read_bytes())),
        'Owned code unchanged by deferred metaclass registration')

# The probe (starts the device on the IOHIDResource) compiles like the display archive's KeyStoreProbe.cpp.
source = ROOT / 'kits/opi5plus/Opi5plusHidProbe.cpp'
probe = OUT / 'Opi5plusHidProbe.o'
command, index, arguments = [], 0, record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command += ['-o', str(probe)]
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier',
            '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-missing-designated-field-initializers', '-Wno-unused-but-set-global',
            '-I', str(ROOT / '.cache/xnu-work/osfmk')]
run(command, cwd=record['directory'])

archive = OUT / 'libOpi5plusHid.a'
archive.unlink(missing_ok=True)
run([shutil.which('llvm-ar') or 'llvm-ar', 'rcs', archive, late, OUT / 'hid-original-bindings.o', probe])
(OUT / 'abi-audit.json').write_text(json.dumps({
    'reference_candidate_sha256': plan['candidate_sha256'],
    'original_text_sha256': HID_TEXT_SHA,
    'original_slots': SLOTS,
    'original_instance_size': INSTANCE,
    'vtable': hex(vptr), 'constructor': hex(ctor), 'destructor': hex(dtor), 'metaclass': 'run-time registry lookup',
    'owned_slots': owned,
    'bindings': {k: hex(v) for k, v in bindings.items()},
    'runtime_qualified': False,
    'source_sha256': hashlib.sha256((ROOT / 'kits/opi5plus/Opi5plusHid/NativeHidInput.cpp').read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
}, indent=2) + '\n')
print('PASS: original HID instruction identity, base imports and exact owned vtable; archive: ' + str(archive))
