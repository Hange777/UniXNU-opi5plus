#!/bin/bash
# Exact numbers for the README: argument count, line length, size of each drop candidate.
set -u
T=/mnt/d/rk-mac/.tmp/r2-verify
python3 - "$T/round1.args" <<'PY'
import sys
raw = open(sys.argv[1]).read().strip()
line = raw.split('"', 2)[1]
args = line.split()
print('round-1 argument count:', len(args))
print('round-1 line length:', len(line))
new = ['opi5plus-bt5=contextstored', 'opi5plus-bt6=biomed', 'opi5plus-bt7=callservicesd', 'opi5plus-bt8=runningboardd']
added = sum(len(a) + 1 for a in new)
print('added by the four slots:', added)
print('round-2 line length:', len(line) + added)
print('overflow over 1000:', len(line) + added - 1000)
print('round-2 argument count:', len(args) + 4)
for a in ['offline-legacy-default-surface=0', 'offline-legacy-unit-scale=2', 'offline-legacy-adopt-surface=1',
          'sd-paint=0', 'sd-clock-div=0', 'sheng-vsync=60', 'opi5-hid-input=1', 'sd-watchdog=15']:
    print('  drop %-34s %3d bytes' % (a, len(a) + 1))
for combo in (['offline-legacy-default-surface=0', 'sd-paint=0'],
              ['offline-legacy-default-surface=0', 'sd-clock-div=0'],
              ['offline-legacy-default-surface=0', 'sheng-vsync=60']):
    cut = sum(len(a) + 1 for a in combo)
    print('  combo %-60s cut %3d -> %d' % (' + '.join(combo), cut, len(line) + added - cut))
PY
