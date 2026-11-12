#!/bin/bash
# Round 2 pre-board build: defconfig + build kernel, transcript to build-round2.log.
# Two build-environment preconditions, both inherited from round 1 (no source change):
#  * prepare-xnu-source.py restores 26 omitted Apple headers over HTTPS; direct TLS is reset by the
#    local network, the proxy git uses answers 200, so the proxy is exported for that step only.
#  * unixnu runs the archives before the final XNU compile; a startup.o.json compile record left by the
#    previous build makes the bootstrap pass skip, and archive-03 then fails on the missing object.
set -u
export UNIXNU_OUT=/opt/unixnu-out
export UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553
# proxy from the host repo-root .env (never committed)
. /mnt/d/rk-mac/.env
: "${PROXY:?set PROXY in the repo-root .env}"
export http_proxy="$PROXY"
export https_proxy="$PROXY"
cd /mnt/d/rk-mac/repos/UniXNU || exit 90
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
E=/mnt/d/rk-mac/repos/UniXNU-opi5plus/evidence/round2-baseline
REC=$W/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json
GEN=$W/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/Makefile

echo "== before =="
date -Is
sha256sum "$W/kernel/kernel.macho"
sha256sum "$W/kernel/kernel.json"
sha256sum "$W/xnu-patches.list"
sha256sum "$W/.config"

mkdir -p "$E/stash"
if [ -f "$REC" ]; then
  mv -f "$REC" "$E/stash/startup.o.json.bak-$(date +%H%M%S)"
  echo "record moved rc=$?"
else
  echo "no startup.o.json record"
fi
if [ -f "$GEN" ] && grep -q 'SRCROOT)/osfmk' "$GEN"; then
  rm -f "$GEN"
  echo "generated bsd Makefile with a stale osfmk include removed rc=$?"
else
  echo "generated bsd Makefile leaves no stale osfmk include (or is absent)"
fi

echo "== defconfig =="
./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
echo "defconfig exit=$?"

echo "== build kernel =="
date -Is
./unixnu build kernel -D ../UniXNU-opi5plus
echo "build exit=$?"
date -Is

echo "== after =="
sha256sum "$W/kernel/kernel.macho"
sha256sum "$W/kernel/kernel.json"
sha256sum "$W/xnu-patches.list"
sha256sum "$W/xnu-inputs.json"

echo "== error hits (regex :[0-9]+:[0-9]+: error:) =="
echo "kernel.log:            $(grep -cE ':[0-9]+:[0-9]+: error:' "$W"/logs/kernel.log 2>/dev/null)"
echo "archive-*.log (sum):   $(cat "$W"/logs/archive-*.log 2>/dev/null | grep -cE ':[0-9]+:[0-9]+: error:')"
echo "component-*.log (sum): $(cat "$W"/logs/component-*.log 2>/dev/null | grep -cE ':[0-9]+:[0-9]+: error:')"
echo "prelink.log:           $(grep -cE ':[0-9]+:[0-9]+: error:' "$W"/logs/prelink.log 2>/dev/null)"
echo "kernel-bootstrap.log:  $(grep -cE ':[0-9]+:[0-9]+: error:' "$W"/logs/kernel-bootstrap.log 2>/dev/null)"

echo "== kernel sha256 line of this run =="
grep -hoE 'kernel: \S+ sha256 [0-9a-f]{64}' "$E"/build-round2.log || echo "NONE"
