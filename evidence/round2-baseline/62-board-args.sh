#!/bin/bash
# The board.toml half of the command line: is each name a literal in the kernel too?
set -u
T=/mnt/d/rk-mac/.tmp/r2-verify
for name in debug serial sheng-smp armcpu-self-ipi armcpu-mmio-sgi cpumask sched-slowcpus \
            offline-apfs-load offline-mobile-graphics-load sd-data-seed sheng-aks offline-key-events \
            mali mali-metal; do
  n=$(grep -Fxc -- "$name" "$T/k2.strings" 2>/dev/null || true)
  printf '  %-32s %s\n' "$name" "${n:-0}"
done
