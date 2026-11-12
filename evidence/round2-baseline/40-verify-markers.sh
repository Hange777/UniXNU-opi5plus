#!/bin/bash
# Round-2 marker verification on the new kernel: strings counts, raw byte counts, symbol table.
set -u
K=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho
LLVM=/opt/unixnu-linux/llvm/bin
E=/mnt/d/rk-mac/repos/UniXNU-opi5plus/evidence/round2-baseline

STR=/tmp/round2-kernel.strings
strings -a "$K" > "$STR"
echo "kernel: $K"
ls -l "$K"
sha256sum "$K"
echo "strings lines: $(wc -l < "$STR")"

check() {
  label=$1
  pat=$2
  s=$(grep -Fc -- "$pat" "$STR" 2>/dev/null)
  r=$(LC_ALL=C grep -aoc -- "$pat" "$K" 2>/dev/null)
  printf '%-34s strings=%-4s raw_lines=%s\n' "$label" "$s" "$r"
}

echo "== new markers =="
check 'opi5plus_text_base (symbol text)' 'opi5plus_text_base'
check '_opi5plus_text_base (symbol)' '_opi5plus_text_base'
check 'text 0x' 'text 0x'
check ' text ' ' text '
check 'biplanar surface accepted' 'biplanar surface accepted'
check 'derived stride' 'derived stride'
check 'biomed' 'biomed'
check 'callservicesd' 'callservicesd'
check 'runningboardd' 'runningboardd'
check 'contextstored' 'contextstored'
echo "== regression markers =="
check 'opi5plus: stat ' 'opi5plus: stat '
check 'opi5plus: sysnum ' 'opi5plus: sysnum '
check 'bh %#x' 'bh %#x'
echo "== boot-arg names for the sampler =="
check 'opi5plus-bt5' 'opi5plus-bt5'
check 'opi5plus-bt8' 'opi5plus-bt8'
check 'opi5plus-bt-secs' 'opi5plus-bt-secs'

echo "== nm =="
if [ -x "$LLVM/llvm-nm" ]; then
  "$LLVM/llvm-nm" "$K" > /tmp/round2-kernel.nm 2>/dev/null
  echo "nm lines: $(wc -l < /tmp/round2-kernel.nm)"
  grep -E 'opi5plus_text_base|opi5plus_ubt_for_thread|opi5plus_task_sample|opi5plus_user_chain' /tmp/round2-kernel.nm
  echo "-- count of any opi5plus_text_base symbol: $(grep -c 'opi5plus_text_base' /tmp/round2-kernel.nm)"
else
  echo "no llvm-nm at $LLVM/llvm-nm"
  ls -1 "$LLVM" | grep -E 'llvm-(nm|objdump)' || true
fi
