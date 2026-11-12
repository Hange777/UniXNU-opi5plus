#!/bin/bash
# Read-only: every byte that differs between the round5 payload kernel and the current rebuild,
# plus the value at each site in both files, and the count.
set -u
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
OLD=/mnt/d/rk-mac/artifacts/m5/payload-stub.img
NEW="$W/kernel/kernel.macho"
echo "== differing byte count (round5 payload vs current kernel.macho) =="
cmp -l <(mtype -i "$OLD" ::/EFI/UniXNU/Payloads/kernel.macho) "$NEW" | wc -l
echo "== offsets =="
cmp -l <(mtype -i "$OLD" ::/EFI/UniXNU/Payloads/kernel.macho) "$NEW" | awk '{printf "%d ", $1} END {print ""}'
echo
echo "== 8-byte windows around each diff in the current file =="
for off in $(cmp -l <(mtype -i "$OLD" ::/EFI/UniXNU/Payloads/kernel.macho) "$NEW" | awk '{print $1-1}'); do
  python3 -c 'import struct,sys
o=int(sys.argv[1]); d=open(sys.argv[2],"rb").read()
print(o, d[o:o+8].hex(" "), struct.unpack_from("<I", d, o)[0])' "$off" "$NEW"
done
echo "== round5 value at the same sites (from payload) =="
mtype -i "$OLD" ::/EFI/UniXNU/Payloads/kernel.macho | python3 -c 'import sys,struct
d=sys.stdin.buffer.read(); offs=[int(x) for x in sys.argv[1:]]
for o in offs: print(o, d[o:o+8].hex(" "), struct.unpack_from("<I", d, o)[0])' $(cmp -l <(mtype -i "$OLD" ::/EFI/UniXNU/Payloads/kernel.macho) "$NEW" | awk '{print $1-1}')
