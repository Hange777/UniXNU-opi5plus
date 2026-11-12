#!/bin/bash
# Read-only probe of the round5 build output, run before the round1 rebuild.
set -u
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
cd "$W"
echo "== round5 kernel.log error hits =="
grep -cE ':[0-9]+:[0-9]+: error:' logs/kernel.log
echo "== round5 archive-*.log error hits (sum) =="
cat logs/archive-*.log | grep -cE ':[0-9]+:[0-9]+: error:'
echo "== round5 component-*.log error hits (sum) =="
cat logs/component-*.log | grep -cE ':[0-9]+:[0-9]+: error:'
echo "== kernel sha lines in round5 kernel.log =="
grep -nE 'kernel: [0-9a-f]{64}' logs/kernel.log | tail -5
echo "== round5 xnu-patches.list sha256 =="
sha256sum xnu-patches.list
echo "== round5 kernel.macho sha256 =="
sha256sum kernel/kernel.macho
echo "== kernel.macho (tree) sha256 =="
sha256sum tree/build/xnu/kernel.macho 2>/dev/null || echo "no tree kernel.macho"
echo "== payload kernel sha256 (mtype) =="
mtype -i /mnt/d/rk-mac/artifacts/m5/payload-stub.img ::/EFI/UniXNU/Payloads/kernel.macho 2>/dev/null | sha256sum || echo "payload stub missing"
