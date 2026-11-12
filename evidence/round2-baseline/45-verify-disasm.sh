#!/bin/bash
# Disassembly evidence for the markers strings(1) cannot see, in the object and in the linked kernel.
set -u
K=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho
O=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837
T=/mnt/d/rk-mac/.tmp/r2-verify

echo "== object: branch targets in _opi5plus_ubt_for_thread =="
grep -nE '(^|[[:space:]])bl[[:space:]]' "$T/ubt_for_thread.s" | head -40

echo "== object: lines mentioning lock/map helpers =="
grep -nE 'lck_rw|vm_map|task->map|_opi5plus_ubt_hex' "$T/ubt_for_thread.s" | head -20

echo "== object: the header literals around the '] text' walk =="
sed -n '215,270p' "$T/ubt_for_thread.s"

echo "== linked kernel: disassemble _opi5plus_ubt_for_thread =="
llvm-objdump -d --disassemble-symbols=_opi5plus_ubt_for_thread "$K" > "$T/kernel_ubt.s" 2>&1
echo "lines: $(wc -l < "$T/kernel_ubt.s")"
grep -cE 'mov[[:space:]]+w0, #0x30|mov[[:space:]]+w0, #0x78' "$T/kernel_ubt.s"
grep -nE 'mov[[:space:]]+w0, #(0x5d|0x74|0x65|0x78|0x30|0x20)' "$T/kernel_ubt.s" | head -24
echo "-- calls in the linked function --"
grep -nE '(^|[[:space:]])bl[[:space:]]' "$T/kernel_ubt.s" | head -20

echo "== linked kernel: is lck_rw_try_lock_shared reachable from the function =="
grep -nE 'lck_rw|lock_shared' "$T/kernel_ubt.s" | head -10
