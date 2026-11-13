#!/bin/bash
# pack-payload.sh -- the regular payload pack for this board (replaces the per-round
# tmp-payload-rN.sh copies: same skeleton, no per-round argument list).
#
# The boot arguments live in board.toml, [payload] kernel_args (51 arguments, 957 bytes of the
# 1000-byte limit). This script passes NO --kernel-arg on purpose: board.toml is the single argument
# source, so a plain rebuild cannot lose the display, SD or HID groups the round scripts used to
# carry. The merge rule is scripts/unixnu/payload.py:93-96 -- a board entry whose name a --kernel-arg
# repeats is dropped -- and with no --kernel-arg at all it cannot bite.
#
# Usage: run as root in the WSL distro, from $RK_MAC/repos/UniXNU (same environment as the
# round scripts):   bash "$RK_MAC/repos/UniXNU-opi5plus/tools/wsl/pack-payload.sh" [OUTPUT.img]
# Output: $1, default $RK_MAC/artifacts/m5/payload.img. Log: /tmp/pack-payload.log.
# The build step is NOT run here: it needs a fresh kernel.macho under $OUT/kernel/ (build it with
# ./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus && ./unixnu build kernel).
#
# UNIXNU_UNIQUE_CHIP_ID, RK_MAC and GIT_PROXY are read from the gitignored $RK_MAC/.env (KEY=VALUE;
# see the root .env.example) and the script refuses to run without them, so no host-specific value is
# hardcoded here.
set -u

ENV_FILE=${UNIXNU_ENV_FILE:-/mnt/d/rk-mac/.env}
if [ ! -f "$ENV_FILE" ]; then
  echo "error: no .env at $ENV_FILE (copy the root .env.example to .env and fill it in)" >&2
  exit 1
fi
set -a
. "$ENV_FILE"
set +a
: "${UNIXNU_UNIQUE_CHIP_ID:?set UNIXNU_UNIQUE_CHIP_ID in .env}"
: "${RK_MAC:?set RK_MAC in .env}"
: "${GIT_PROXY:?set GIT_PROXY in .env}"

OUT_ARG="${1:-$RK_MAC/artifacts/m5/payload.img}"
LOG=/tmp/pack-payload.log
export UNIXNU_OUT=/opt/unixnu-out
export UNIXNU_UNIQUE_CHIP_ID
export http_proxy="$GIT_PROXY" https_proxy="$GIT_PROXY"
DEV=$RK_MAC/repos/UniXNU-opi5plus
OUT=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
UNIXNU=$RK_MAC/repos/UniXNU/unixnu
ADDON=$RK_MAC/.tmp/native-plus-stub.addon
cd $RK_MAC/repos/UniXNU || exit 1
[ -f "$ADDON" ] || { echo "error: no addon at $ADDON (run make-exec-stub-addon.py)"; exit 1; }
$UNIXNU payload -D $DEV --out $OUT --kernel $OUT/kernel/kernel.macho \
  --owner-trust-cache "$DEV/.cache/gpu-trust.bundle" --gpu-addon "$ADDON" \
  --data-ramdisk $RK_MAC/artifacts/m3/data-seed-front2g.img \
  > "$LOG" 2>&1
echo "rc=$?"
tail -4 "$LOG"
P=$OUT/opi5plus/ipados-17.7.11-21H461/opi5plus/payload/payload.img
cp "$P" "$OUT_ARG"
ls -l "$OUT_ARG"
sha256sum "$OUT_ARG"
echo "== initrd line =="
mtype -i "$OUT_ARG" ::/EFI/UniXNU/unixnu.toml 2>/dev/null | grep -E 'args' | head -1 | cut -c1-200
