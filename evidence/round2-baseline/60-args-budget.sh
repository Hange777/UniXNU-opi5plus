#!/bin/bash
# Why the round-2 payload cannot be written: command-line budget accounting.
set -u
A=/mnt/d/rk-mac/artifacts/m5
K=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho
T=/mnt/d/rk-mac/.tmp/r2-verify
mkdir -p "$T"

echo "== round-1 payload command line =="
mcopy -i "$A/payload-round1.img" -o ::/EFI/UniXNU/unixnu.toml "$T/round1.toml" 2>&1
grep -oE 'args[[:space:]]*=.*' "$T/round1.toml" | head -1 > "$T/round1.args"
echo "round1 line bytes (with 'args = \"\"' wrapper): $(wc -c < "$T/round1.args")"
python3 - "$T/round1.args" <<'PY'
import sys
line = open(sys.argv[1]).read().strip()
line = line.split('"',2)[1]
print('round1 command line: %d bytes' % len(line))
print(line)
PY

echo "== three new bt slots =="
for s in 'opi5plus-bt5=contextstored' 'opi5plus-bt6=biomed' 'opi5plus-bt7=callservicesd' 'opi5plus-bt8=runningboardd'; do
  printf '  %-32s %d bytes (+1 space)\n' "$s" "${#s}"
done

echo "== is each argument name parsed by this kernel (literal present in the binary) =="
strings -a "$K" > "$T/k2.strings"
for name in sd sd-root-trust sd-data-ram sd-data-persist sd-ps sd-exit sd-syscall sd-auto-reboot \
            sd-trace-io sd-clock-div sd-watchdog sd-paint panic_on_crash_disabled \
            offline-legacy-register offline-legacy-phase offline-legacy-gop offline-legacy-unit-scale \
            offline-legacy-pm offline-legacy-validate offline-legacy-transfer offline-legacy-cpu-present \
            offline-legacy-default-surface offline-legacy-adopt-surface offline-legacy-paint \
            sheng-vsync opi5-hid opi5-hid-input rk3588-vop-direct rk3588-vop-esmart rk3588-vop-vp \
            rk3588-vop-fence sheng-no-lc; do
  n=$(grep -Fxc -- "$name" "$T/k2.strings" 2>/dev/null || true)
  printf '  %-32s %s\n' "$name" "${n:-0}"
done
