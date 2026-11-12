#!/bin/bash
# Round1 close-out: snapshot the patch list and compute every number the report needs.
set -u
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
E=/mnt/d/rk-mac/repos/UniXNU-opi5plus/evidence/round1-baseline
cd "$W"

cp -p xnu-patches.list "$E/xnu-patches.list"

echo "== xnu-patches.list sha256 =="
sha256sum "$E/xnu-patches.list" "$E/xnu-patches.round5.list"
echo "== diff round5 vs round1 patch list =="
if diff -u "$E/xnu-patches.round5.list" "$E/xnu-patches.list"; then echo "IDENTICAL"; fi
echo "== patch entries =="
grep -c . "$E/xnu-patches.list"

echo "== error hits after rebuild =="
echo "kernel.log:            $(grep -cE ':[0-9]+:[0-9]+: error:' logs/kernel.log)"
echo "archive-*.log (sum):   $(cat logs/archive-*.log | grep -cE ':[0-9]+:[0-9]+: error:')"
echo "component-*.log (sum): $(cat logs/component-*.log | grep -cE ':[0-9]+:[0-9]+: error:')"
echo "prelink.log:           $(grep -cE ':[0-9]+:[0-9]+: error:' logs/prelink.log)"
echo "kernel-bootstrap.log:  $(grep -cE ':[0-9]+:[0-9]+: error:' logs/kernel-bootstrap.log)"

echo "== kernel sha256 lines from the three builds =="
grep -hoE 'kernel: \S+ sha256 [0-9a-f]{64}' "$E/build-round1.log" "$E/build-round1-run2.log" || true
echo "round5 (recorded before rebuild): 4782b2210d50783ec614525ee41b8265df73fa83784dffcd93e021a54601331e"

echo "== current kernel.macho sha256 =="
sha256sum kernel/kernel.macho
echo "== current kernel/kernel.json identity =="
head -3 kernel/kernel.json

echo "== xnu-inputs.json (patch/source signature) =="
ls -l --time-style=full-iso xnu-inputs.json
python3 - <<'PY'
import json
d = json.load(open('/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/xnu-inputs.json'))
print('source', d['source'])
print('commit', d['commit'])
print('patches', len(d['patches']))
print('first', d['patches'][0]['path'], d['patches'][0]['sha256'][:16])
PY
stat -c 'xnu-inputs.json mtime: %y' xnu-inputs.json

echo "== toolchain =="
/opt/unixnu-linux/llvm/bin/clang --version | head -2
echo "== soc feed cache working tree =="
git -C /opt/unixnu-out/feeds/UniXNU-rk3588-common-75c7120d status --porcelain=v1 -b
