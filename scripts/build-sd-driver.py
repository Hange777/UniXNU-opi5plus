#!/usr/bin/env python3
"""Compile the SD storage driver of UniXNU-opi5plus against the actual source-kernel ABI.

Components of the UniXNU catalog (ArmDTPlatform, ArmCpu) are built by `component:<ID>` entries of
board.toml. This script builds what the catalog does not carry:

    build/native-sd/NativeSd.o        package/NativeSd/NativeSd.cpp (with SdHost.h beside it)

and leaves the object, not an archive: `scripts/bind-sd-storage.py` binds NativeSdBlock to the
original IOBlockStorageDevice and makes build/native-sd/libNativeSd.a, which the kernel link
force-loads through CONFIG_XNU_EXTRA_LD_ARCHIVES. Leaving an archive here would let the kernel link
a driver that was never bound, so this step deliberately does not produce one.

An XNU driver has to be compiled with the same flags, the same SDK and the same deployment target as
the kernel object it lives next to, or the Mach-O it produces will not link or will not run. The
kernel build records the exact command for every object it compiles in a `.cpo.json` file, so this
script reads that record, points it at the driver source, and runs the compiler. The board's
package/ appears as kits/opi5plus in the build tree; catalog drivers are under drivers/.

  python3 scripts/build-sd-driver.py NativeSd

Derived from UniXNU-evb10's scripts/build-evb10-drivers.py, BSD-2-Clause-Patent,
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

# component -> (sources relative to the build tree, output directory under build/, extra include
# directories under the tree root, whether a bind script makes the archive). Headers next to a source
# are found through the quote include of that source; they are not listed here.
COMPONENTS = {
    # The RK3588 SD host. Like the EVB10's eMMC driver it subclasses the original IOBlockStorageDevice,
    # so it compiles against the public IOStorageFamily headers the kernel step prepares in
    # build/storage-abi/include.
    #
    # `kits/opi5plus/SdHost` is listed even though it sits beside the source's parent: the reference
    # builder's comment ("headers next to a source are found through the quote include") holds only
    # when the header really is in the source's own directory, and ours are one level up in a sibling.
    # A quoted include searches the includer's directory first and then the -I list, so without this
    # entry SdHost.h, Gpt.h and RootManifest.h are all invisible.
    'NativeSd': (('kits/opi5plus/NativeSd/NativeSd.cpp',), 'native-sd',
                 ('build/storage-abi/include', 'kits/opi5plus/SdHost'), True),
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
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        index += 1
    command.extend([
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
        # The driver includes XNU's private headers from the patched work clone.
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
        # warnings in the XNU headers themselves are not this driver's problem
        '-Wno-error=unknown-warning-option',
        # NOTE: -Wmissing-designated-field-initializers and -Wunused-but-set-global are NOT listed
        # here on purpose. They fire from XNU's own headers under clang 24 and would be hard errors
        # under -Werror, but the kernel step already suppresses them for every process it spawns:
        # scripts/unixnu/kernel.py:80 exports
        #   CCC_OVERRIDE_OPTIONS='# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global'
        # and kernel.py:62-64 records why -- the clang *driver* reads that variable, so it also
        # applies to the archive builders that replay xnu's compile records. Adding the flags here
        # would be harmless but would hide that mechanism; running this script by hand (outside
        # `unixnu build kernel`) needs the same export, which tools/wsl/193-run-sd-chain.sh does.
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
            raise SystemExit('%s is missing; board sources are package/ of the device repository '
                             '(kits/opi5plus in the tree)' % source)
        destination = out / (Path(relative).stem + '.o')
        destination.unlink(missing_ok=True)
        command = compiler_command(record, source, destination, includes)
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
    (out / 'compile-command.json').write_text(
        json.dumps(commands[0] if len(commands) == 1 else commands, indent=2) + '\n')
    (out / 'compile.log').write_text(''.join(logs))

    archive = out / ('lib' + args.component + '.a')
    # A stale archive from an earlier, differently built source must never be linkable.
    archive.unlink(missing_ok=True)
    if bound:
        print('%s: -Werror build and ARM64/iOS 17 object audit passed; the archive comes from '
              'scripts/bind-sd-storage.py' % args.component)
        return
    subprocess.run([ar_tool(), 'rcs', str(archive)] + [str(path) for path in objects], check=True)
    print('%s: -Werror build and ARM64/iOS 17 object audit passed -> %s'
          % (args.component, archive))


if __name__ == '__main__':
    main()
