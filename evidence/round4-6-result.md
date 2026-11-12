# Rounds 4-6 result — bluetoothd blocked with zero code, the SD counters, and the serial reset key

Date: 2026-11-12 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One boot per round, t0 = the kernel's first line; wall-clock time is t0 + t. Every boot log cited here lives
in `logs/` of the **project working tree, not committed** (`logs/**` is gitignored).

## What the three rounds did

| round | change | one-line outcome |
| --- | --- | --- |
| 4 | `opi5plus-block-exec=bluetoothd` on the command line; `sd-auto-reboot=3600` and `offline-legacy-paint=0` dropped; the SD PIO counters printed on the 30 s beat line | bluetoothd never runs again (268 exits -> 0) with zero driver code; the counters price the PIO read path at 12,484 bursts per 64 KiB chunk |
| 5 | `SdHost.h` read path: two phases per burst (fill to a watermark, then drain) | bursts/chunk 12,484 -> 121 (103x fewer STATUS polls) and the chunk time does not move: the polling shape is not the constraint |
| 6 | `NativeSd.cpp` serial reset thread (Ctrl-], 0x1d, -> panic -> PSCI reset); `serial=3` -> `serial=1`; new tool `tools/wsl/serial-reset.py` | a byte typed at the host ends a round: about 19 s later the board is back in Linux; the serial input channel is closed for good |

## R0 — build identity

| round | kernel sha256 (51,695,616 B) | payload | command line |
| --- | --- | --- | --- |
| 4 | `7a19fec7dcec309d23cd7b2be35aab51aded902a6a5afe1a228283669e39f529` (`logs/build-round4.log:89`) | `artifacts/m5/payload-round4.img`, 100,663,296 B, sha256 `eb15f3bf2f40aa27652b0c83302d32bd0d8e4fd65395cb12aba38dbd5bd90ca7` (`logs/pack-r4.log`) | 981 B / 53 arguments |
| 5 | `ebc9e399d4389d08f49d5ad92f806c13d56fd5901cb6a3133d1f769dba17027e` (`logs/build-round5.log:78`) | `artifacts/m5/payload-round5.img`, sha256 `a2827f37246d25cbebf36c22c8f943565e47d8d068e1fa660918a2f2d3a86654` | identical to round 4 (header change only) |
| 6 | `60f90e509ea775c2353861bfa3928356cc20c9722b16c9c3b12a00864bb56220` (`logs/build-round6.log:89`) | `artifacts/m5/payload-round6.img`, sha256 `a32be9cd0df5e722104d5aab011f22534acb91e1756ff93d2204dd94f0978a62` | identical except `serial=1` |

The round-4 command line differs from round 3's 993 bytes / 54 arguments in exactly three ways: `sd-auto-reboot=3600`
removed (the watchdog stays armed through `sd-watchdog=15`; the automatic reset is redundant now that the reset key
exists), `offline-legacy-paint=0` removed (its parser in `package/LegacyDisplayProbe.cpp:717` reads 0 when the
argument is absent, `paintMode=2` is the off state either way, so deleting it changes no behaviour), and
`opi5plus-block-exec=bluetoothd` added (31 bytes with its space). The read-back line agrees:
`logs/boot-round4.log:2234` carries `sheng-no-lc=1 opi5plus-block-exec=bluetoothd` and no `sd-auto-reboot`.

## S1 — block-exec: bluetoothd is gone from the process table (round 4)

Criterion: with `opi5plus-block-exec=bluetoothd` on the command line, the kernel refuses the daemon's exec and
the `exit bluetoothd[` corpse line disappears.

```
logs/boot-round4.log:2439  opi5plus: exec of /usr/sbin/bluetoothd blocked (opi5plus-block-exec=bluetoothd)
```

The refusal line repeats through the whole capture (every launchd respawn attempt is refused in one line). The
count side by side, `grep 'exit bluetoothd\['` over each round's capture:

| round | `exit bluetoothd[` lines |
| --- | --- |
| 3 | 268 |
| 4 | **0** |

Round 3 counted 268 of them between t+696 and the end of its 3,800 s window (`logs/boot-round3.log`). Round 4 has
none: the daemon never starts, so there is nothing to exit. This is the round-2 finding ("bluetoothd keeps its own
loop because it is the one process that prints its TMPDIR failure and continues") closed at the exec gate instead
of the exit one, with zero lines of driver code — one boot argument, patch 0006.

The round-4 operator watched the board reach the desktop by itself, as round 3 did: the unlock path survives both
the serial changes of round 6 and the counter additions.

## S2 — the SD counters' first data (round 4)

Round 4 rode the counters onto the one report that prints every 30 s whatever the display does
(`package/LegacyDisplayProbe.cpp`, the `sd pio bursts / chunk us / chunks` tail). First sample:

```
logs/boot-round4.log:1969  opi5: display t+30s: … sd pio bursts 64593877, chunk us 29338459, chunks 5174
```

64593877 / 5174 = **12,484 bursts per chunk**; 29338459 us / 5174 = **5,671 us per 64 KiB chunk** (5.67 ms,
11.3 MB/s). A "burst" is one drain of whatever STATUS showed at poll time, so 12,484 polls for 16,384 words means
1.3 words per poll — the read loop was draining the FIFO the moment one or two words arrived. That number is the
baseline every later round is measured against.

`sd-auto-reboot, resetting` appears **0** times in the capture (the argument is gone), and the watchdog stayed
armed. The capture window was 1,000 s, against round 3's 3,800 s — the counters reach steady state in the first
30 s beat, so the shorter window is enough from round 4 on.

## S3 — two-phase PIO: fewer polls, same time (round 5, negative result)

Round 5 rewrote the read half of `SdHost::transferPio` as two phases per burst: phase 1 waits for the FIFO to
hold a full batch (`kPioReadBatchWords = 128`), phase 2 drains the batch without touching STATUS in between
(`package/SdHost/SdHost.h`, the read half of `transferPio`). One STATUS poll per batch instead of one per word.

```
logs/boot-round5.log:1968  opi5: display t+30s: … sd pio bursts 623919, chunk us 29300792, chunks 5150
```

623919 / 5150 = **121 bursts per chunk** — the 103x drop the change predicted — and 29300792 / 5150 = **5,687 us
per chunk**, the same 5.67 ms. Throughput did not move (11.3 MB/s both sides).

**Conclusion: the volume of STATUS polling is not the constraint.** Round 4's first model — "the chunk is
CPU-bound on MMIO because it drains 1.3 words per poll" — is falsified: removing 99% of the polls changes
nothing. Whatever sets the 5.67 ms does not scale with the poll count. Rounds 7-10 chase the real constraint in
[`evidence/sd-clock-investigation.md`](sd-clock-investigation.md); the falsification is recorded here because it
is what motivated them.

The tail of this capture is also the last round that ended the old way: lines 2302-2402 show the manual
power-cycle's UEFI start and the Linux boot down to its login prompt (`logs/boot-round5.log:2402`).

## S4 — the serial reset key (round 6)

`NativeSd::start()` now starts a kernel thread before the `sd=` early return, so the key works even on a boot
that publishes no device. Every 100 ms it drains the console's receive FIFO through the non-blocking
`serial_getc()` of `pexpert/arm/pe_kprintf.c` and panics on 0x1d (Ctrl-]), which this port's PSCI restart turns
into a board reset (`package/NativeSd/NativeSd.cpp`, `native_sd_serial_reset_thread`). This XNU has no
`tsleep()`, so the thread waits with `assert_wait_timeout(..., 100, kMillisecondScale)` + `thread_block()`.

Two facts make the hook safe and effective:

* **`serial=3` -> `serial=1`** (`board.toml`). With serial=3 the console starts `serial_keyboard_poll`, a 16 ms
  poller that drains the same FIFO, and the second reader only ever wins 0.3% of the bytes. serial=1 is
  SERIALMODE_OUTPUT alone (0x01): `serial_keyboard_init()` (`osfmk/console/serial_general.c:57`) returns without
  starting the poller, the reset thread becomes the only reader, and console output is unaffected.
  `logs/boot-round6.log:1560` and `logs/boot-round7.log:1560` both carry
  `DARWINBOOT: command line: -v serial=1 debug=0x10014e …`.
* The host sends the byte with the new tool `tools/wsl/serial-reset.py` (`py -3 tools/wsl/serial-reset.py`,
  default COM3 @ 1.5 Mbaud). The capture reader must release the port first, so **the reset itself is not in the
  boot capture**; the evidence below is the host-side and SSH readings taken right after.

Measured at the end of round 6, the first round the hook existed:

* after `py -3 tools/wsl/serial-reset.py`, the Linux kernel was up in about **19 s**;
* over SSH, `/proc/uptime` read 29.7 s — consistent with the kernel having started ~19 s after the key (the boot
  had begun before the reading);
* `journalctl` recorded a fresh boot, not a resume.

That is the round-end automation the later rounds run on: `py -3 tools/wsl/serial-reset.py` and the board is back
in Linux in about 20 s, where the deploy tool can push the next payload — no more power cable.

**The cost, accepted:** the serial input channel is permanently closed while `serial=1` stands. Nothing uses it
today (KDP is parked by `debug=0x10014e`, and there is no serial login). If a future round needs KDP or a serial
console login, it must go back to `serial=3` **and** add arbitration between `serial_keyboard_poll` and the reset
thread — the two readers cannot share the FIFO as they are.

## R1 — captures

| log | window | size |
| --- | --- | --- |
| `logs/boot-round4.log` | 1,000 s (kernel beats to t+505) | 193,257 B |
| `logs/boot-round5.log` | ends inside the manual power-cycle and the Linux boot it triggered | 142,995 B |
| `logs/boot-round6.log` | 1,000 s (kernel beats to t+115) | 124,103 B |

Project working tree, not committed; `logs/**` is gitignored.
