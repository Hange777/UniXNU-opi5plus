#!/usr/bin/env python3
"""Build libOpi5plusUsb.a from package/Opi5plusUsb (polled OHCI + HID mailbox).

Board copy of UniXNU-orangepi5's scripts/build-orangepi5-usb.py (BSD-2-Clause-Patent,
Copyright (c) 2026, UniXNU contributors), renamed per this repository's rules and pointed
at the board driver script: build-opi5plus-drivers.py compiles package/Opi5plusUsb/NativeUsb.cpp
with the kernel's recorded compiler flags from .cpo.json (object-only), and this script makes
the Mach-O archive. No Apple USBFamily ABI audit. Not referenced by the packaging yet: adding
it to opi5plus_display_defconfig's CONFIG_XNU_EXTRA_LD_ARCHIVES (force_load) is the step that
wires the driver into the kernel link.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'opi5plus-usb'


def ar_tool():
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
    OUT.mkdir(parents=True, exist_ok=True)
    driver = ROOT / 'scripts' / 'build-opi5plus-drivers.py'
    result = subprocess.run([sys.executable, str(driver), 'Opi5plusUsb'], cwd=ROOT)
    if result.returncode:
        raise SystemExit(result.returncode)
    obj = OUT / 'NativeUsb.o'
    if not obj.is_file():
        raise SystemExit('Opi5plusUsb: expected %s after driver build' % obj)
    archive = OUT / 'libOpi5plusUsb.a'
    archive.unlink(missing_ok=True)
    subprocess.run([ar_tool(), 'rcs', str(archive), str(obj)], check=True)
    print('opi5plus-usb: archive -> %s' % archive)


if __name__ == '__main__':
    main()
