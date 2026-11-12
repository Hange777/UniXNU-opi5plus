#!/bin/bash
# Round 1 baseline rebuild: the exact command pair from plan.md section 5, no instrumentation.
set -u
export UNIXNU_OUT=/opt/unixnu-out
export UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553
cd /mnt/d/rk-mac/repos/UniXNU || exit 90
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display

echo "== before =="
date -Is
sha256sum "$W/.config"
sha256sum "$W/kernel/kernel.macho"

echo "== defconfig =="
./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
echo "defconfig exit=$?"
sha256sum "$W/.config"

echo "== build kernel =="
date -Is
./unixnu build kernel -D ../UniXNU-opi5plus
echo "build exit=$?"
date -Is

echo "== after =="
sha256sum "$W/kernel/kernel.macho"
sha256sum "$W/xnu-patches.list"
