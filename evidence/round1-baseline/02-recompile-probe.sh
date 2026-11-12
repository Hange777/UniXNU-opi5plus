#!/bin/bash
# Read-only: did the rebuild recompile anything, and what does the log contain?
set -u
W=/opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display
cd "$W"
echo "== compile invocations in new kernel.log =="
grep -cE 'clang .* -c |cc -c ' logs/kernel.log || true
echo "== lines that are not make Entering/Leaving =="
grep -vE '^(make|gmake)\[' logs/kernel.log | head -40
echo
echo "== log byte size / lines =="
wc -c logs/kernel.log
wc -l logs/kernel.log
echo "== archive log sizes / mtimes =="
ls -l --time-style=+%Y-%m-%dT%H:%M logs/archive-*.log
echo "== object file mtimes (sample) =="
find tree/build/xnu/ios-obj -name '*.o' -newermt '2026-11-11 05:00' | head -20
echo "== count of .o newer than round5 =="
find tree/build/xnu/ios-obj -name '*.o' -newermt '2026-11-11 05:00' | wc -l
echo "== native kernel mtime =="
ls -l --time-style=full-iso tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf
