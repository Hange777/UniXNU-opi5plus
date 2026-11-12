#!/bin/bash
set -u
K=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho
T=/tmp/round2-verify
echo "== nm sample lines =="
head -5 "$T/new.nm"
echo "== nm lines containing opi5plus (any) =="
grep -c 'opi5plus' "$T/new.nm"
grep 'opi5plus' "$T/new.nm" | grep -E 'text_base|ubt_for_thread|ubt_hex|user_chain|task_sample|report_perms' || echo "no such symbols"
echo "== object files with the sampler =="
O=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837
grep -rl 'opi5plus_ubt_for_thread' "$O"/bsd "$O"/osfmk --include='*.o' 2>/dev/null | head -10
echo "== nm on those objects =="
for f in $(grep -rl 'opi5plus_text_base' "$O"/bsd "$O"/osfmk --include='*.o' 2>/dev/null | head -5); do
  echo "-- $f"
  llvm-nm "$f" | grep -E 'opi5plus_(text_base|ubt_for_thread|ubt_hex|ubt_puts)' || true
done
