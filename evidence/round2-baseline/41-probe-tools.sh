#!/bin/bash
set -u
echo "== llvm dir =="
ls -1 /opt/unixnu-linux/llvm/bin | head -60
echo "== which =="
for t in llvm-nm llvm-objdump llvm-otool mtype mcopy mmd strings python3 otool; do
  printf '%-14s %s\n' "$t" "$(command -v $t 2>/dev/null || echo -)"
done
echo "== PATH =="
echo "$PATH"
