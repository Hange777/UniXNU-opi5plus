#!/bin/bash
# Read-only: show the bytes that differ between the round5 kernel (from its payload)
# and the round1 rebuild kernel currently on disk.
set -u
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
OLD=/mnt/d/rk-mac/artifacts/m5/payload-stub.img
NEW="$W/kernel/kernel.macho"
for off in 15025785 15144041 15145657 15168409 15170393 51676722; do
  echo "===== offset $off ====="
  echo "-- round5 (payload) --"
  mtype -i "$OLD" ::/EFI/UniXNU/Payloads/kernel.macho | python3 -c 'import sys;o=int(sys.argv[1]);d=sys.stdin.buffer.read();s=max(0,o-40);print(d[s:o+40].hex(" "));print(repr(d[s:o+40]))' "$off"
  echo "-- round1 (new) --"
  python3 -c 'import sys;o=int(sys.argv[1]);d=open(sys.argv[2],"rb").read();s=max(0,o-40);print(d[s:o+40].hex(" "));print(repr(d[s:o+40]))' "$off" "$NEW"
done
