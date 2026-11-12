#!/bin/bash
# Read-only: HEAD of every cached feed checkout and the pinned commits from the device lock.
set -u
echo "== feed cache HEADs =="
for d in /opt/unixnu-out/feeds/*/; do
  printf '%-58s %s\n' "$d" "$(git -C "$d" rev-parse HEAD 2>/dev/null || echo NONE)"
done
echo
echo "== device unixnu.lock =="
cat /mnt/d/rk-mac/repos/UniXNU-opi5plus/unixnu.lock
echo
echo "== rk3588 feed working tree state =="
git -C /opt/unixnu-out/feeds/UniXNU-rk3588-common-75c7120d status --porcelain=v1 -b
echo
echo "== rk3588 series patch presence =="
ls /opt/unixnu-out/feeds/UniXNU-rk3588-common-75c7120d/patches/xnu/ | sort
echo
echo "== old rk3588 cache (other key) =="
git -C /opt/unixnu-out/feeds/UniXNU-rk3588-common-f99abd1c log -1 --format='%H %ci %s' 2>/dev/null || echo NONE
