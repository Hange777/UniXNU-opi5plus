#!/usr/bin/env python3
"""Archive the weak no-op hooks of LinkStubs.c for the kernel link (CONFIG_XNU_EXTRA_LD_ARCHIVES).

Same shape as UniXNU-evb10/scripts/build-link-stubs.py: compile one translation unit and archive it with
llvm-ar. The kernel link picks it up through the generic extra-archive hook, so a board without the eMMC
driver still links. The definitions inside are weak, so force-loading the real libRk3588Emmc.a later
(M4, eMMC module fitted) replaces them without an edit here.

Locating the source is the one non-obvious part. This repository keeps it in package/LinkStubs.c, but
scripts run from the assembled tree, where scripts/unixnu/tree.py has linked the device package as
kits/<kit> (kits/opi5plus here) and left tree/package pointing at UniXNU's own package. So look in both,
in the order that matches where the file actually lives in each layout.
"""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CANDIDATES = (
    ROOT / 'package/LinkStubs.c',       # this repository, run directly
    ROOT / 'kits/opi5plus/LinkStubs.c',  # assembled tree (tree.py: device package -> kits/<kit>)
)
OUT = ROOT / 'build/link-stubs'


def main():
    source = next((p for p in CANDIDATES if p.is_file()), None)
    if source is None:
        raise SystemExit('LinkStubs.c is missing; looked in:\n  ' + '\n  '.join(map(str, CANDIDATES)))
    OUT.mkdir(parents=True, exist_ok=True)
    obj, archive = OUT / 'LinkStubs.o', OUT / 'libLinkStubs.a'

    # The SDK path comes from the xcrun shim on Linux and from the real xcrun on macOS; the kernel build
    # script exports SDKROOT either way, so prefer it and fall back to the tool.
    sdk = None
    try:
        sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        sdk = None

    command = ['clang', '-target', 'arm64-apple-ios17.0', '-mkernel', '-miphoneos-version-min=17.0',
               '-ffreestanding', '-fno-builtin', '-fno-stack-check', '-std=gnu11',
               '-Os', '-Wall', '-Werror']
    if sdk:
        command += ['-isysroot', sdk]
    command += ['-c', str(source), '-o', str(obj)]

    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (OUT / 'LinkStubs.log').write_text(result.stdout)
    if result.returncode:
        sys.stderr.write(result.stdout[-4000:])
        raise SystemExit('LinkStubs.c did not compile')

    archive.unlink(missing_ok=True)
    ar = shutil.which('llvm-ar') or shutil.which('ar')
    subprocess.run([ar, 'rcs', str(archive), str(obj)], check=True)
    print(archive)


if __name__ == '__main__':
    main()
