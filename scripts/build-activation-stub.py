#!/usr/bin/env python3
"""Build the activation stub: a Mach-O executable that stands in for /usr/libexec/mobileactivationd.

The stub is not a kernel component and not a payload addon: it is a userland program that launchd starts in
place of the daemon that dies on this port (evidence/m7/M7-09). It is built here, ad-hoc signed, and its
cdhash is printed so the payload's owner trust cache can carry it.

    scripts/build-activation-stub.py            # -> build/activation-stub/mobileactivationd

Inputs come from the environment because they are machine-specific (repos/UniXNU-opi5plus/.env):
  LLVM_BIN  the cross toolchain that targets arm64-apple-ios (default /opt/metalwork/toolchain/bin)
  IOS_SDK   the iPhoneOS SDK (default /opt/metalwork/sdk/iPhoneOS16.5.sdk)
"""
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCE = os.path.join(ROOT, "package", "ActivationStub", "ActivationStub.m")
OUTDIR = os.path.join(ROOT, "build", "activation-stub")
OUTPUT = os.path.join(OUTDIR, "mobileactivationd")
LC_CODE_SIGNATURE = 0x1d


def cdhash_of(path):
	"""The cdhash AMFI matches against a trust cache: SHA-256 over the CodeDirectory, first 20 bytes."""
	raw = open(path, "rb").read()
	if struct.unpack_from("<I", raw, 0)[0] != 0xfeedfacf:
		sys.exit("%s is not a little-endian arm64 Mach-O" % path)
	at = 32
	for _ in range(struct.unpack_from("<I", raw, 16)[0]):
		command, size = struct.unpack_from("<II", raw, at)
		if command == LC_CODE_SIGNATURE:
			dataoff, datasize = struct.unpack_from("<II", raw, at + 8)
			super_blob = raw[dataoff:dataoff + datasize]
			offset = struct.unpack_from(">II", super_blob, 12)[1]
			length = struct.unpack_from(">II", super_blob, offset)[1]
			return hashlib.sha256(super_blob[offset:offset + length]).hexdigest()[:40]
		at += size
	sys.exit("%s has no LC_CODE_SIGNATURE (was -Wl,-adhoc_codesign lost?)" % path)


def main() -> int:
	llvm = os.environ.get("LLVM_BIN", "/opt/metalwork/toolchain/bin")
	sdk = os.environ.get("IOS_SDK", "/opt/metalwork/sdk/iPhoneOS16.5.sdk")
	clang = os.path.join(llvm, "clang-20")
	if not os.path.exists(clang):
		clang = os.path.join(llvm, "clang")
	if not os.path.isdir(sdk):
		sys.stderr.write("error: no SDK at %s (set IOS_SDK)\n" % sdk)
		return 2
	if not os.path.exists(SOURCE):
		sys.stderr.write("error: no source at %s\n" % SOURCE)
		return 2
	os.makedirs(OUTDIR, exist_ok=True)
	command = [
		clang, "-target", "arm64-apple-ios17.0", "-isysroot", sdk, "-O2", "-g0",
		"-fobjc-arc", "-Wall", "-Wextra", "-Wno-unused-parameter",
		"-fuse-ld=lld", "-B" + llvm, "-Wl,-adhoc_codesign", "-Wl,-dead_strip",
		"-framework", "Foundation",
		"-o", OUTPUT, SOURCE,
	]
	print(" ".join(command))
	result = subprocess.run(command)
	if result.returncode != 0:
		sys.stderr.write("error: the stub did not build\n")
		return result.returncode
	subprocess.run(["chmod", "0755", OUTPUT], check=True)
	size = os.path.getsize(OUTPUT)
	print("built %s (%d bytes)" % (OUTPUT, size))
	# The cdhash is what AMFI checks; print it so it can go into the payload's trust cache bundle
	# (scripts/add-trust-cdhash.py). It is the first 20 bytes of SHA-256 over the CodeDirectory blob, which is
	# what the kernel and AMFI compute; the check against a known binary is in evidence/m7/M7-10 (2e9bfb49...).
	print("cdhash %s" % cdhash_of(OUTPUT))
	return 0


if __name__ == "__main__":
	sys.exit(main())
