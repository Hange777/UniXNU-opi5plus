#!/usr/bin/env python3
"""Compile the board drivers of UniXNU-opi5plus against the actual source-kernel ABI.

Components of the UniXNU catalog (ArmDTPlatform, ArmCpu) are built by `component:<ID>` entries of
board.toml. This script builds what the catalog does not carry:

    build/rk3588-emmc/libRk3588Emmc.a      package/Rk3588Emmc  (submodule; bind-emmc-storage.py makes the archive)
    build/mali/libMali.a                   package/Mali        (submodule)
    build/opi5plus-touch/libOpi5plusTouch.a      package/Opi5plusTouch
    build/evb10-hid/                       package/Evb10Hid    (object; build-evb10-hid.py)
    build/native-legacy-display/           package/Evb10Display (object; build-native-legacy-display.py)
    build/opi5plus-usb/                    package/Opi5plusUsb (object; build-opi5plus-usb.py archives libOpi5plusUsb.a)

An XNU driver has to be compiled with the same flags, the same SDK and the same deployment target as
the kernel object it lives next to, or the Mach-O it produces will not link or will not run. The kernel
build records the exact command for every object it compiles in a `.cpo.json` file, so this script reads
that record, points it at the driver source, and runs the compiler. The board's package/ appears as kits/opi5plus in the build
tree; catalog drivers are under drivers/.

  python3 scripts/build-opi5plus-drivers.py Rk3588Emmc | Mali | Opi5plusTouch
  python3 scripts/build-opi5plus-drivers.py NativeLegacyDisplay | NativeGOPPresenter | NativeKeyStoreEvents | NativeHidInput

Derived from UniXNU-sheng's scripts/build-arm-driver.py, BSD-2-Clause-Patent,
Copyright (c) 2026, UniXNU contributors.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# component -> (sources relative to the build tree, output directory under build/, extra include directories under the
# tree root, whether scripts/bind-emmc-storage.py makes the archive). Headers next to a source are found
# through the quote include of that source; they are not listed here.
COMPONENTS = {
    # The display archive (scripts/build-native-legacy-display.py, run through scripts/build-opi5plus-display.py).
    # These leave an object only ('object'): that script generates the original-class header NativeLegacyDisplay
    # includes (hence the include of its own output directory), binds the objects to the original
    # IOMobileFramebufferLegacy and makes the archive. drivers/GopFramebuffer and drivers/AppleKeyStoreState are the
    # UniXNU components of those names; this board's display path is the firmware's GOP frame buffer, so the backend
    # is the component's own generic one -- its console, PE_current_console, already returns the surface the drivers
    # compare against (evidence/m5/M5-01), so no board fork and no console shim are needed.
    # Board fork (package/Opi5plusDisplay, taken from the Orange Pi 5 port): force primary and publish
    # IOMFBDisplayRefresh at sheng-vsync>=60 (the component's own class only does it above 60).
    'NativeLegacyDisplay': (('kits/opi5plus/Opi5plusDisplay/NativeLegacyDisplay.cpp',), 'native-legacy-display',
                            ('build/native-legacy-display', 'drivers/GopFramebuffer'), 'object'),
    'NativeGOPPresenter': (('drivers/GopFramebuffer/NativeGOPPresenter.cpp',), 'native-gop-presenter', (), 'object'),
    'NativeKeyStoreEvents': (('drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',), 'apple-key-store-state', (), 'object'),
    # The input device (scripts/build-opi5plus-hid.py, the HID archive). It compiles against the generated
    # original-class header in its own output directory, like the display backend above.
    'NativeHidInput': (('kits/opi5plus/Opi5plusHid/NativeHidInput.cpp',), 'opi5-hid', ('build/opi5-hid',), 'object'),
    # The Mali-G610 (package/Mali, the submodule shared with the EVB10): power domain, clocks, reset, the
    # CSF firmware, the MMU and its page tables, command-stream groups, tiler heaps and the Metal-facing
    # service. The display probe starts it with mali=<milestone>; without that argument mali_start()
    # returns at once. scripts/build-mali-firmware.py has to make package/Mali/mali_csffw.inc from
    # .cache/firmware/mali_csffw.bin first -- without that include this entry does not compile.
    'Mali': (('kits/opi5plus/Mali/Mali.cpp',), 'mali', (), False),
    # The polled OHCI USB host (package/Opi5plusUsb, the board copy of UniXNU-orangepi5's Opi5Usb): both USB2 host
    # companions, the boot-protocol HID keyboard / mouse and the native_usb_* mailbox. scripts/build-opi5plus-usb.py
    # archives it (build/opi5plus-usb/libOpi5plusUsb.a); the late probe calls opi5_usb_start(), which returns at once
    # without an opi5-usb argument, and opi5plus_display_defconfig force-loads the archive.
    'Opi5plusUsb': (('kits/opi5plus/Opi5plusUsb/NativeUsb.cpp',), 'opi5plus-usb', (), 'object'),
}

# The record of a kernel object compiled for the same machine configuration and
# SDK. Only its arguments matter: the source and output paths are replaced.
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

# MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_OBJECT.
MACHO = (0xfeedfacf, 0x100000c, 0, 1)
# LC_BUILD_VERSION with PLATFORM_IOS and the iOS 17.0 deployment target, which
# is what build-xnu-4k-baseline.sh passes (-miphoneos-version-min=17.0).
LC_BUILD_VERSION = 0x32
DEPLOYMENT = (2, 0x110000)


# The display drivers compare the console with their device memory; they get this board's console header
# force-included, the same way the Orange Pi 5 port hands its Evb10Console.h to them. Its variables stay zero
# here, so PE_current_console returns the firmware's GOP surface unless the probe allocates one of its own.
FORCE_INCLUDE = {
    'NativeLegacyDisplay': 'kits/opi5plus/Opi5plusConsole.h',
    'NativeGOPPresenter': 'kits/opi5plus/Opi5plusConsole.h',
}


def compiler_command(record, source, destination, includes=(), force_include=None):
    """The recorded command with this source and output, minus warnings XNU sets
    that would make the driver a hard error under -Werror."""
    command, index, arguments = [], 0, record['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command.extend([
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
        # XNU's OSMetaClass macros leave one field of the kalloc type descriptor out; the clang this build uses
        # warns about it inside the macro expansion, and the warning is an error under -Werror.
        '-Wno-missing-designated-field-initializers',
        # The driver includes XNU's private headers from the patched work clone.
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
        # warnings in the XNU headers themselves are not this driver's problem
        '-Wno-error=unknown-warning-option',
    ])
    for include in includes:
        command.extend(['-I', str(ROOT / include)])
    if force_include:
        command.extend(['-include', str(ROOT / force_include)])
    return command


def check_object(path):
    """Refuse an object the kernel would not accept: wrong architecture, wrong
    file type, or built for a different platform than the kernel."""
    data = path.read_bytes()
    magic, cpu, subtype, kind, count, _ = struct.unpack_from('<6I', data, 0)
    if (magic, cpu, subtype, kind) != MACHO:
        raise SystemExit('%s is not an ARM64 Mach-O object (magic %#x, cputype %#x, filetype %d)'
                         % (path, magic, cpu, kind))
    offset, deployment = 32, None
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, offset)
        if command == LC_BUILD_VERSION:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != DEPLOYMENT:
        raise SystemExit('%s is built for platform/deployment %s; the kernel is iOS 17.0 (%s)'
                         % (path, deployment, DEPLOYMENT))


def ar_tool():
    """`ar` that understands Mach-O: llvm-ar, or the Xcode one on a Mac."""
    for name in ('llvm-ar', 'ar'):
        found = shutil.which(name)
        if found:
            return found
    for candidate in ('/opt/homebrew/opt/llvm/bin/llvm-ar', '/usr/local/opt/llvm/bin/llvm-ar',
                      '/usr/bin/llvm-ar'):
        if Path(candidate).is_file():
            return candidate
    raise SystemExit('no llvm-ar on PATH; a Mach-O archive needs one')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('component', choices=sorted(COMPONENTS))
    args = parser.parse_args()

    relatives, directory, includes, bound = COMPONENTS[args.component]
    record_path = ROOT / RECORD
    if not record_path.is_file():
        raise SystemExit('%s is missing: the kernel step writes it on its first (bootstrap) pass, '
                         'so run "unixnu build kernel" before this builder' % record_path)
    # The kernel build writes the record with a trailing comma, as a fragment.
    record = json.loads(record_path.read_text().strip().rstrip(','))

    out = ROOT / 'build' / directory
    out.mkdir(parents=True, exist_ok=True)
    objects, commands, logs = [], [], []
    for relative in relatives:
        source = ROOT / relative
        if not source.is_file():
            raise SystemExit('%s is missing; board sources are package/ of the device repository (kits/opi5plus in the '
                             'tree), the others are UniXNU components under drivers/' % source)
        destination = out / (Path(relative).stem + '.o')
        destination.unlink(missing_ok=True)
        command = compiler_command(record, source, destination, includes, FORCE_INCLUDE.get(args.component))
        commands.append(command)
        result = subprocess.run(command, cwd=record['directory'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs.append(result.stdout)
        if result.returncode:
            sys.stderr.write(result.stdout[-4000:] + '\n')
            raise SystemExit('%s: the compiler rejected %s' % (args.component, relative))
        if not destination.is_file():
            raise SystemExit('%s: the compiler produced no object for %s' % (args.component, relative))
        check_object(destination)
        objects.append(destination)
    (out / 'compile-command.json').write_text(json.dumps(commands[0] if len(commands) == 1 else commands, indent=2) + '\n')
    (out / 'compile.log').write_text(''.join(logs))

    archive = out / ('lib' + args.component + '.a')
    archive.unlink(missing_ok=True)
    if bound == 'object':
        print('%s: -Werror build and ARM64/iOS 17 object audit passed -> %s (object only; the archive comes from '
              'scripts/build-native-legacy-display.py)' % (args.component, objects[0]))
        return
    if bound:
        print('%s: -Werror build and ARM64/iOS 17 object audit passed; the archive comes from '
              'scripts/bind-emmc-storage.py' % args.component)
        return
    subprocess.run([ar_tool(), 'rcs', str(archive)] + [str(path) for path in objects], check=True)
    print('%s: -Werror build and ARM64/iOS 17 object audit passed -> %s'
          % (args.component, archive))


if __name__ == '__main__':
    main()
