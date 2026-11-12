#!/bin/bash
# Round 8 payload: the round-7 argument list with exactly one value changed.
#  * sd-clock-div=1 becomes sd-clock-div=0. That is the clock bypass at the board's 52 MHz maximum; round 7
#    measured the divider at 1 and could not go higher at 3.3 V, so the round-8 kernel is built for the
#    undivided clock and the payload has to say so. The argument count and every other character are
#    identical to round 7 (argc is unchanged at 53, 38 of them passed here).
# sd-ps=300 stays on purpose: patches/xnu/0002-mount-data-volume.patch:1564-1567 terminates the heartbeat
# thread when sd-ps and sd-auto-reboot are both 0, and that thread is the only caller of
# native_sd_watchdog_kick(), so dropping both would leave the RK3588 watchdog to reset the board every ~89 s.
# sd-auto-reboot and offline-legacy-paint stay absent for the reasons given in the round-4 script
# (tools/wsl/tmp-payload-r4.sh): the round-4 kernel no longer panics on a timer, and an absent
# offline-legacy-paint sets the same paint=0 the parser defaults to.
# Writes payload-round8.img only; every earlier image in artifacts/m5/ is a rollback point and is never
# touched.
set -u
export UNIXNU_OUT=/opt/unixnu-out
export UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553
DEV=/mnt/d/rk-mac/repos/UniXNU-opi5plus
OUT=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
UNIXNU=/mnt/d/rk-mac/repos/UniXNU/unixnu
ADDON=/mnt/d/rk-mac/.tmp/native-plus-stub.addon
cd /mnt/d/rk-mac/repos/UniXNU || exit 1
[ -f "$ADDON" ] || { echo "error: no addon at $ADDON (run make-exec-stub-addon.py)"; exit 1; }
$UNIXNU payload -D $DEV --out $OUT --kernel $OUT/kernel/kernel.macho \
  --owner-trust-cache "$DEV/.cache/gpu-trust.bundle" --gpu-addon "$ADDON" \
  --data-ramdisk /mnt/d/rk-mac/artifacts/m3/data-seed-front2g.img \
  --kernel-arg sd=4 --kernel-arg sd-root-trust=1 --kernel-arg sd-data-ram=1 \
  --kernel-arg sd-data-persist=1 --kernel-arg sd-ps=300 --kernel-arg sd-exit=1 \
  --kernel-arg sd-syscall=1 --kernel-arg sd-trace-io=0 \
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
  --kernel-arg opi5plus-bt-secs=30 \
  --kernel-arg sheng-no-lc=1 \
  --kernel-arg opi5plus-block-exec=bluetoothd \
  --kernel-arg sheng-vsync=60 --kernel-arg opi5-hid=1 --kernel-arg opi5-hid-input=2 \
  --kernel-arg rk3588-vop-direct=1 --kernel-arg rk3588-vop-esmart=2 --kernel-arg rk3588-vop-vp=2 \
  --kernel-arg rk3588-vop-fence=1 \
  > /tmp/payload-round8.log 2>&1
echo "rc=$?"
tail -4 /tmp/payload-round8.log
P=$OUT/opi5plus/ipados-17.7.11-21H461/opi5plus/payload/payload.img
cp "$P" /mnt/d/rk-mac/artifacts/m5/payload-round8.img
ls -l /mnt/d/rk-mac/artifacts/m5/payload-round8.img
sha256sum /mnt/d/rk-mac/artifacts/m5/payload-round8.img
echo "== initrd line =="
mtype -i /mnt/d/rk-mac/artifacts/m5/payload-round8.img ::/EFI/UniXNU/unixnu.toml 2>/dev/null | grep -E 'args' | head -1 | cut -c1-200
