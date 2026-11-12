#!/bin/bash
# pack-payload.sh -- the regular payload pack for this board (replaces the per-round
# tmp-payload-rN.sh copies: same skeleton, no per-round argument list).
#
# The boot arguments live in board.toml, [payload] kernel_args (53 arguments, 981 bytes of the
# 1000-byte limit). This script passes NO --kernel-arg on purpose: board.toml is the single argument
# source, so a plain rebuild cannot lose the display, SD or HID groups the round scripts used to
# carry. The merge rule is scripts/unixnu/payload.py:93-96 -- a board entry whose name a --kernel-arg
# repeats is dropped -- and with no --kernel-arg at all it cannot bite.
#
# Usage: run as root in the WSL distro, from /mnt/d/rk-mac/repos/UniXNU (same environment as the
# round scripts):   bash /mnt/d/rk-mac/repos/UniXNU-opi5plus/tools/wsl/pack-payload.sh [OUTPUT.img]
# Output: $1, default /mnt/d/rk-mac/artifacts/m5/payload.img. Log: /tmp/pack-payload.log.
# The build step is NOT run here: it needs a fresh kernel.macho under $OUT/kernel/ (build it with
# ./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus && ./unixnu build kernel).
set -u
OUT_ARG="${1:-/mnt/d/rk-mac/artifacts/m5/payload.img}"
LOG=/tmp/pack-payload.log
export UNIXNU_OUT=/opt/unixnu-out
export UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553
DEV=/mnt/d/rk-mac/repos/UniXNU-opi5plus
OUT=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
UNIXNU=/mnt/d/rk-mac/repos/UniXNU/unixnu
ADDON=/mnt/d/rk-mac/.tmp/native-plus-stub.addon
cd /mnt/d/rk-mac/repos/UniXNU || exit 1
[ -f "$ADDON" ] || { echo "error: no addon at $ADDON (run make-exec-stub-addon.py)"; exit 1; }
$UNIXNU payload -D $DEV --out $OUT --kernel $OUT/kernel/kernel.macho \
  --owner-trust-cache "$DEV/.cache/gpu-trust.bundle" --gpu-addon "$ADDON" \
  --data-ramdisk /mnt/d/rk-mac/artifacts/m3/data-seed-front2g.img \
  > "$LOG" 2>&1
echo "rc=$?"
tail -4 "$LOG"
P=$OUT/opi5plus/ipados-17.7.11-21H461/opi5plus/payload/payload.img
cp "$P" "$OUT_ARG"
ls -l "$OUT_ARG"
sha256sum "$OUT_ARG"
echo "== initrd line =="
mtype -i "$OUT_ARG" ::/EFI/UniXNU/unixnu.toml 2>/dev/null | grep -E 'args' | head -1 | cut -c1-200
