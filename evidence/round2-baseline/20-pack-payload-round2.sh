#!/bin/bash
# Round-2 payload: tools/wsl/tmp-payload-stub.sh verbatim except the destination paths (the scratch
# log and the final copy), so payload-round1.img and payload-stub.img cannot be overwritten.
# The kernel embedded is the one the round-2 build just wrote to $OUT/kernel/kernel.macho.
set -u
export UNIXNU_OUT=/opt/unixnu-out
export UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553
DEV=/mnt/d/rk-mac/repos/UniXNU-opi5plus
OUT=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
UNIXNU=/mnt/d/rk-mac/repos/UniXNU/unixnu
ADDON=/mnt/d/rk-mac/.tmp/native-plus-stub.addon
DEST=/mnt/d/rk-mac/artifacts/m5/payload-round2.img
cd /mnt/d/rk-mac/repos/UniXNU || exit 1
[ -f "$ADDON" ] || { echo "error: no addon at $ADDON (run make-exec-stub-addon.py)"; exit 1; }
$UNIXNU payload -D $DEV --out $OUT --kernel $OUT/kernel/kernel.macho \
  --owner-trust-cache "$DEV/.cache/gpu-trust.bundle" --gpu-addon "$ADDON" \
  --data-ramdisk /mnt/d/rk-mac/artifacts/m3/data-seed-front2g.img \
  --kernel-arg sd=4 --kernel-arg sd-root-trust=1 --kernel-arg sd-data-ram=1 \
  --kernel-arg sd-data-persist=1 --kernel-arg sd-ps=300 --kernel-arg sd-exit=1 \
  --kernel-arg sd-syscall=1 --kernel-arg sd-auto-reboot=3600 --kernel-arg sd-trace-io=0 \
  --kernel-arg sd-clock-div=0 --kernel-arg sd-watchdog=15 --kernel-arg sd-paint=0 \
  --kernel-arg panic_on_crash_disabled=1 \
  --kernel-arg offline-legacy-register=1 --kernel-arg offline-legacy-phase=4 \
  --kernel-arg offline-legacy-gop=1 --kernel-arg offline-legacy-unit-scale=2 \
  --kernel-arg offline-legacy-pm=1 --kernel-arg offline-legacy-validate=1 \
  --kernel-arg offline-legacy-transfer=1 --kernel-arg offline-legacy-cpu-present=1 \
  --kernel-arg offline-legacy-default-surface=0 --kernel-arg offline-legacy-adopt-surface=1 \
  --kernel-arg opi5plus-bt=SpringBoard --kernel-arg opi5plus-bt2=backboardd \
  --kernel-arg opi5plus-bt3=liveactivitiesd --kernel-arg opi5plus-bt4=chronod \
  --kernel-arg opi5plus-bt5=contextstored --kernel-arg opi5plus-bt6=biomed \
  --kernel-arg opi5plus-bt7=callservicesd --kernel-arg opi5plus-bt8=runningboardd \
  --kernel-arg opi5plus-bt-secs=30 \
  --kernel-arg sheng-no-lc=1 \
  --kernel-arg sheng-vsync=60 --kernel-arg opi5-hid=1 --kernel-arg opi5-hid-input=1 \
  --kernel-arg rk3588-vop-direct=1 --kernel-arg rk3588-vop-esmart=2 --kernel-arg rk3588-vop-vp=2 \
  --kernel-arg rk3588-vop-fence=1 --kernel-arg offline-legacy-paint=0 \
  > /tmp/payload-round2.log 2>&1
echo "rc=$?"
tail -4 /tmp/payload-round2.log
P=$OUT/opi5plus/ipados-17.7.11-21H461/opi5plus/payload/payload.img
cp "$P" "$DEST"
ls -l "$DEST"
sha256sum "$DEST"
echo "== initrd args line =="
mtype -i "$DEST" ::/EFI/UniXNU/unixnu.toml 2>/dev/null | grep -E 'args' | head -1 | cut -c1-400
