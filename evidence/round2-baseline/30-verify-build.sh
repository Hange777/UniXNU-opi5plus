#!/bin/bash
# Round-2 post-build log inspection: what compiled, what warned, what the bootstrap pass did.
set -u
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
E=/mnt/d/rk-mac/repos/UniXNU-opi5plus/evidence/round2-baseline

echo "== kernel.log =="
echo "clang -c lines:   $(grep -cE 'clang -c ' "$W"/logs/kernel.log)"
echo "warnings:         $(grep -cE ':[0-9]+:[0-9]+: warning:' "$W"/logs/kernel.log)"
echo "errors:           $(grep -cE ':[0-9]+:[0-9]+: error:' "$W"/logs/kernel.log)"
echo "bootstrap link failure lines:"
grep -nE 'symbol\(s\) not found|ld: *.*not found' "$W"/logs/kernel.log | head -20

echo "== per-log error counts =="
for f in "$W"/logs/*.log; do
  n=$(grep -cE ':[0-9]+:[0-9]+: error:' "$f" 2>/dev/null)
  if [ "$n" != "0" ]; then echo "$(basename "$f"): $n"; fi
done

echo "== archive-15 / display compile =="
ls -1 "$W"/logs/ | grep -E 'archive-15|component' | head -20
grep -cE 'NativeLegacyDisplay' "$W"/logs/archive-15-build-opi5plus-display.log 2>/dev/null

echo "== build log: bootstrap + link outcomes =="
grep -nE 'symbol.*not found|ld: |Bootstrap|bootstrap|Error [0-9]|failed' "$E"/build-round2.log | head -30

echo "== kernel sha256 lines in this build log =="
grep -hoE 'kernel: [^ ]+ sha256 [0-9a-f]{64}' "$E"/build-round2.log

echo "== xnu-patches.list =="
wc -l < "$W"/xnu-patches.list
sha256sum "$W"/xnu-patches.list

echo "== xnu-inputs.json 0002/0003/0004 =="
python3 - <<'PY'
import json
p='/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/xnu-inputs.json'
d=json.load(open(p))
print('source', d.get('source'), 'commit', d.get('commit'), 'patches', len(d['patches']))
for e in d['patches']:
    name = e['path'].rsplit('/',1)[-1]
    if name.startswith(('0001','0002','0003','0004','0005','0006','0007')):
        print(name, e['sha256'])
PY

echo "== kernel file =="
ls -l "$W"/kernel/kernel.macho
sha256sum "$W"/kernel/kernel.macho
