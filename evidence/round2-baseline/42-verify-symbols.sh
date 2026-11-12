#!/bin/bash
# Symbol-level verification of the new kernel, plus a strings comparison against the round-1 and
# round-5 payload kernels (the same image with the same Apple kernelcache, so a count that changes is
# attributable to this round; a count that stays is part of the prelinked Apple part).
set -u
K=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho
T=/tmp/round2-verify
mkdir -p "$T"

echo "== llvm-nm on the new kernel =="
llvm-nm "$K" > "$T/new.nm" 2>"$T/new.nm.err"
echo "nm lines: $(wc -l < "$T/new.nm")  stderr: $(head -2 "$T/new.nm.err" | tr '\n' ' ')"
for s in opi5plus_text_base opi5plus_ubt_for_thread opi5plus_user_chain opi5plus_task_sample opi5plus_ubt_hex opi5plus_report_perms; do
  printf '%-28s %s\n' "$s" "$(grep -c " $s\$" "$T/new.nm")"
done
echo "-- lines --"
grep -E ' (opi5plus_text_base|opi5plus_ubt_for_thread|opi5plus_ubt_hex)$' "$T/new.nm" || echo "none"

echo "== extract baseline kernels from the payloads =="
for p in /mnt/d/rk-mac/artifacts/m5/payload-round1.img /mnt/d/rk-mac/artifacts/m5/payload-stub.img; do
  name=$(basename "$p" .img)
  mcopy -i "$p" -o ::/EFI/UniXNU/Payloads/kernel.macho "$T/$name.kernel.macho" 2>"$T/$name.err" \
    && echo "$name extracted $(stat -c %s "$T/$name.kernel.macho") bytes sha256 $(sha256sum "$T/$name.kernel.macho" | cut -d' ' -f1)" \
    || echo "$name extraction failed: $(head -2 "$T/$name.err")"
done

echo "== strings comparison: new vs round1 vs round5 =="
strings -a "$K" > "$T/new.strings"
[ -f "$T/payload-round1.kernel.macho" ] && strings -a "$T/payload-round1.kernel.macho" > "$T/round1.strings"
[ -f "$T/payload-stub.kernel.macho" ] && strings -a "$T/payload-stub.kernel.macho" > "$T/round5.strings"
printf '%-32s %6s %6s %6s\n' marker new round1 round5
for m in 'opi5plus_text_base' 'text 0x' '] text ' ' text ' 'biplanar surface accepted' 'derived stride' \
         'biomed' 'callservicesd' 'runningboardd' 'contextstored' 'opi5plus: stat ' 'opi5plus: sysnum ' 'bh %#x'; do
  a=$(grep -Fc -- "$m" "$T/new.strings" 2>/dev/null || true)
  b=$(grep -Fc -- "$m" "$T/round1.strings" 2>/dev/null || true)
  c=$(grep -Fc -- "$m" "$T/round5.strings" 2>/dev/null || true)
  printf '%-32s %6s %6s %6s\n' "$m" "${a:-0}" "${b:-0}" "${c:-0}"
done
