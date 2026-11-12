#!/bin/bash
# Symbol presence in the linked kernel and in the object it comes from; work files under .tmp (persistent).
set -u
K=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho
O=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837
T=/mnt/d/rk-mac/.tmp/r2-verify
mkdir -p "$T"

echo "== kernel.macho symbol table =="
llvm-nm "$K" > "$T/kernel.nm" 2>&1
echo "macho nm lines: $(wc -l < "$T/kernel.nm")"
echo "macho nm opi5plus hits: $(grep -c opi5plus "$T/kernel.nm")"
grep -E 'opi5plus_(text_base|ubt_for_thread|ubt_hex|task_sample)' "$T/kernel.nm" | head -10 || echo "(no such symbol in macho nm)"

echo "== kernel.macho strings (symbol names can survive as literals) =="
strings -a "$K" > "$T/kernel.strings"
for s in _opi5plus_text_base opi5plus_text_base _opi5plus_ubt_for_thread _opi5plus_ubt_hex; do
  printf '%-30s %s\n' "$s" "$(grep -Fc -- "$s" "$T/kernel.strings" 2>/dev/null || true)"
done

echo "== objects =="
for f in "$O"/osfmk/DEVELOPMENT/bsd_kern.o "$O"/bsd/DEVELOPMENT/kern_exec.o; do
  echo "-- $f"
  llvm-nm "$f" | grep -E 'opi5plus_(text_base|ubt_for_thread|ubt_hex|ubt_puts|ubt_putc|ubt_dec)' || echo "   (none)"
done

echo "== disassemble bsd_kern.o around opi5plus_ubt_for_thread =="
llvm-objdump -d --disassemble-symbols=_opi5plus_ubt_for_thread "$O"/osfmk/DEVELOPMENT/bsd_kern.o > "$T/ubt_for_thread.s" 2>&1
wc -l < "$T/ubt_for_thread.s"
grep -cE 'bl _lck_rw_try_lock_shared|bl _lck_rw_done' "$T/ubt_for_thread.s" || true
grep -nE 'bl .*(lck_rw|opi5plus_ubt_hex)' "$T/ubt_for_thread.s" | head -10 || true
echo "-- immediate character stores in the header of the line --"
grep -nE 'mov[^;]*w0, #(0x5d|0x30|0x78|0x74|0x65)' "$T/ubt_for_thread.s" | head -20 || true
