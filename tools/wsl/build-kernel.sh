#!/bin/bash
# build-kernel.sh - reproducible-build harness for the opi5plus_display kernel.
#
# Run inside the unixnu WSL distro as root:
#   bash tools/wsl/build-kernel.sh <run-id>
#
# What it does:
#   1. pins SOURCE_DATE_EPOCH to the last device-repo commit time (stable for one
#      source state; override with SOURCE_DATE_EPOCH=<n> in the environment),
#   2. cleans the three incremental-build residues the legacy XNU build leaves:
#      a. moves the stale startup.o.json compile record to tree/logs/ (the build
#         refuses to continue over a record it did not make),
#      b. touches the tree's xnu-work startup.c so the bootstrap pass rewrites
#         the -MJ record the archive builders need,
#      c. deletes the regenerated bsd/DEVELOPMENT/Makefile that points SRCROOT
#         at osfmk and breaks the bsd pass,
#   3. runs `unixnu defconfig opi5plus_display_defconfig` and `unixnu build kernel`
#      with the environment the other build scripts use (proxy via GIT_PROXY from
#      .env, UNIXNU_UNIQUE_CHIP_ID platform seed),
#   4. keeps a copy of the linked kernel as kernel/macho kernel.macho.<run-id>
#      next to the real output and prints its sha256, so two runs can be compared.
#
# The kernel step (scripts/unixnu/kernel.py) sets its own toolchain/env variables
# and does not set SOURCE_DATE_EPOCH; exporting it here reaches the build without
# being overwritten (unlike the firmware step, which setdefaults its own value).
# VERIFIED 2026-11-12 (evidence/sd-clock-investigation.md S6a): pinning
# SOURCE_DATE_EPOCH does NOT make the kernel byte-reproducible — the volatile
# bytes are the N_OSO object mtimes ld64 embeds at link time and the derived
# _PrelinkKCID; judge two builds with tools/wsl/compare-kernel.py instead.
set -u

# Host paths, the platform seed and the git proxy come from the gitignored .env at the root of
# RK_MAC (KEY=VALUE; see the root .env.example); the script refuses to run without them, so no
# host-specific value is hardcoded here.
ENV_FILE=${UNIXNU_ENV_FILE:-/mnt/d/rk-mac/.env}
if [ ! -f "$ENV_FILE" ]; then
  echo "build-kernel: no .env at $ENV_FILE (copy the root .env.example to .env and fill it in)" >&2
  exit 1
fi
set -a
. "$ENV_FILE"
set +a
: "${UNIXNU_UNIQUE_CHIP_ID:?set UNIXNU_UNIQUE_CHIP_ID in .env}"
: "${RK_MAC:?set RK_MAC in .env}"
: "${GIT_PROXY:?set GIT_PROXY in .env}"

RUN_ID=${1:?usage: build-kernel.sh RUN_ID}
ROOT=$RK_MAC
DEV="$ROOT/repos/UniXNU-opi5plus"
XNU="$ROOT/repos/UniXNU"
OUT=${UNIXNU_OUT:-/opt/unixnu-out}
BASE=$OUT/opi5plus/ipados-17.7.11-21H461/opi5plus_display
W=$BASE/tree
OBJ=$W/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837
LOG="$ROOT/logs/build-repro-$RUN_ID.log"

export UNIXNU_OUT=$OUT
export UNIXNU_UNIQUE_CHIP_ID
export CCC_OVERRIDE_OPTIONS=${CCC_OVERRIDE_OPTIONS:-'# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global'}
export http_proxy="$GIT_PROXY" https_proxy="$GIT_PROXY"
export no_proxy=api.github.com NO_PROXY=api.github.com

if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
  SOURCE_DATE_EPOCH=$(git -C "$DEV" log -1 --format=%ct 2>/dev/null)
  [ -n "$SOURCE_DATE_EPOCH" ] || SOURCE_DATE_EPOCH=0
  export SOURCE_DATE_EPOCH
fi

if [ ! -d "$XNU" ] || [ ! -d "$DEV" ]; then
  echo "build-kernel: missing checkout: $XNU or $DEV" >&2
  exit 1
fi

clean_residues() {
  echo "== residues (run $RUN_ID) =="
  mkdir -p "$W/logs"
  if [ -f "$OBJ/osfmk/DEVELOPMENT/startup.o.json" ]; then
    mv "$OBJ/osfmk/DEVELOPMENT/startup.o.json" "$W/logs/startup.o.json.bak-$RUN_ID"
    echo "  startup.o.json -> tree/logs/"
  fi
  touch "$W/.cache/xnu-work/osfmk/kern/startup.c"
  echo "  touched xnu-work startup.c"
  if [ -f "$OBJ/bsd/DEVELOPMENT/Makefile" ]; then
    rm -f "$OBJ/bsd/DEVELOPMENT/Makefile"
    echo "  removed bsd/DEVELOPMENT/Makefile"
  fi
}

clean_residues

cd "$XNU" || exit 1
: > "$LOG"
{
  echo "== defconfig =="
  ./unixnu defconfig opi5plus_display_defconfig -D "$DEV"
  echo "DEFCONFIG_RC=$?"
  echo "== post-defconfig re-clean =="
  rm -f "$OBJ/bsd/DEVELOPMENT/Makefile"
  touch "$W/.cache/xnu-work/osfmk/kern/startup.c"
  echo "== build kernel =="
  ./unixnu build kernel -D "$DEV"
  echo "BUILD_RC=$?"
} >> "$LOG" 2>&1

echo "=== errors: $(grep -c 'error:' "$LOG") ==="
if [ ! -f "$BASE/kernel/kernel.macho" ]; then
  echo "build-kernel: no kernel.macho produced; see $LOG" >&2
  exit 1
fi
cp "$BASE/kernel/kernel.macho" "$BASE/kernel/kernel.macho.$RUN_ID"
echo "=== sha256 (run $RUN_ID) ==="
sha256sum "$BASE/kernel/kernel.macho.$RUN_ID"
echo "=== log tail ==="
tail -5 "$LOG"
