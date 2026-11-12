#!/bin/bash
# Round-2 payload verification: hash, embedded kernel identity, and the boot arguments the round asks for.
set -u
A=/mnt/d/rk-mac/artifacts/m5
T=/mnt/d/rk-mac/.tmp/r2-verify
NEW=$A/payload-round2.img
mkdir -p "$T"

echo "== payload files =="
ls -l "$NEW" "$A/payload-round1.img" "$A/payload-stub.img"
sha256sum "$NEW" "$A/payload-round1.img" "$A/payload-stub.img"

echo "== embedded kernel =="
mcopy -i "$NEW" -o ::/EFI/UniXNU/Payloads/kernel.macho "$T/payload-round2.kernel.macho" 2>&1
ls -l "$T/payload-round2.kernel.macho"
sha256sum "$T/payload-round2.kernel.macho"

echo "== boot arguments (unixnu.toml in payload) =="
mcopy -i "$NEW" -o ::/EFI/UniXNU/unixnu.toml "$T/payload-round2.toml" 2>&1
grep -oE 'args[[:space:]]*=.*' "$T/payload-round2.toml" | head -1 > "$T/payload-round2.args"
wc -c < "$T/payload-round2.args"
cat "$T/payload-round2.args"

echo "== required arguments present =="
for want in 'opi5plus-bt5=contextstored' 'opi5plus-bt6=biomed' 'opi5plus-bt7=callservicesd' \
            'opi5plus-bt8=runningboardd' 'offline-legacy-paint=0' 'sd-auto-reboot=3600' 'sd-syscall=1'; do
  n=$(grep -Foc -- "$want" "$T/payload-round2.args" 2>/dev/null || true)
  printf '%-34s %s\n' "$want" "${n:-0}"
done

echo "== all opi5plus-bt slots =="
grep -oE 'opi5plus-bt[0-9]*=[A-Za-z0-9_]+' "$T/payload-round2.toml" | sort -u

echo "== 911-verify-payload.py =="
python3 /mnt/d/rk-mac/tools/wsl/911-verify-payload.py "$NEW" \
  --expect-kernel-sha 45fbfab3cae0c66dcf73b85ba08a82ae7a580b0c4e6beabab1d81f76dc0a3893 2>&1 | tail -20
