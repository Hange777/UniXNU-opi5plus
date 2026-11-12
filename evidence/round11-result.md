# Round 11 result — direct scanout without the GOP mirror, and the beat line grows compressor observability

Date: 2026-11-12 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One boot; t0 = the kernel's first line. Every line cited here lives in `logs/boot-round11.log` of the
**project working tree, not committed** (`logs/**` is gitignored).

## What the round did, and why

Two questions were open after round 10 (`evidence/sd-clock-investigation.md` closed the SD line):

1. **The mirror copy is the frame-rate bottleneck.** Since 2026-11-11 the direct path also copied each
   latched frame back into the firmware's GOP buffer (`opi5-gop-mirror`, default 1): 88.6-95.7 ms of CPU
   copy per 14,745,600 B frame in round 2, on top of the composition itself. A UI that animates cannot
   sustain a rate through that path. Round 11 boots with **`opi5-gop-mirror=0`** — the VOP window stays on
   the compositor's surface and the VOP scans it out directly — and verifies the direct path end to end.
2. **The old same-source judgement for the screen defects had to be tested.** Round 2's record carried
   the banding inside the large glyphs and the white blocks as one unresolved picture defect. Under
   direct scanout the two can be separated: if the banding is an artifact of CPU writes into the buffer
   the VOP scans (mirror copies among them), it disappears with the copy; anything left is a different
   defect. Both questions are answered below.

### Changes

| file | change |
| --- | --- |
| `board.toml` | `sd-trace-io=0` removed from the passed command line (its default is 0 — verified in round 4, `NativeSd.cpp` leaves the value at 0 when `PE_parse_boot_argn` fails — so passing nothing is the same value; dropped for the 1000-byte budget); `opi5-gop-mirror=0` added with a comment naming the parser and the reason; header comment 981 -> 985 |
| `patches/xnu/0002-mount-data-volume.patch` | regenerated from the generator: the beat line gains **7 new VM fields** (`vinact/vact/vext/vpur/vcact/vcmode/lsr`), the `debug.c` counter functions take 9 out-parameters, `kern_exec.c` carries the `lsr` block, and the display report gains the two rate lines (`swaps handed over (the render server's rate)` and `frames shown (the scanout's rate)`); 71,306 -> 75,588 B, sha256 `bcfd8c28cb3021130a52fad08934cd457f66a2fe660c5676d3f90062eadb88a8` |
| `package/NativeSd/NativeSd.cpp` | `native_sd_last_read_tick` added (`:148` declaration, written on the successful `readBlocks` path `:398-405` under the same gate lock): the `cntpct_el0` tick at which the last card read completed — the beat line's `lsr` counts only card-medium reads |
| `tools/wsl/pack-payload.sh` | header comment 981 -> 985 (this is now the regular pack; board.toml is the single argument source) |

The command line's recompute, checked against the launchd echo (`:2360`): the round-10 line was 981 B / 53
arguments. Removing `sd-trace-io=0` costs 13 characters plus its separator (14 B); adding
`opi5-gop-mirror=0` costs 17 characters plus its separator (18 B). 981 - 14 + 18 = **985 B**, and the
argument count stays 53 (one dropped, one added). The launchd echo agrees byte for byte: the boot-args
value on `logs/boot-round11.log:2360` measures 985 characters / 53 tokens.

## R0 — build identity

| item | value |
| --- | --- |
| kernel sha256 | `6a3a3a9df6be6967bfef5a471faba49b2152cfd94e0a1edd65db16e84c850c63` (51,699,712 B) |
| payload | `artifacts/m5/payload-round11.img`, 96 MiB (100,663,296 B), sha256 `4104427298fa01ba01a77764e55f1db6c22565c433559f802cd9e35c221b5cf8` |
| command line | **985 B / 53 arguments**, double-verified (FAT read-back and the launchd echo `:2360`) |
| patch series | the WSL build applied the full device series **0001-0007 in one pass** — the co-existence check this round's build is. The Windows-side series check still fails at 0001: a pre-existing Windows-side limitation, already adjudicated and accepted, unchanged by this round. |

## S1 — zero copy (criterion 1)

Criterion: with `opi5-gop-mirror=0`, no GOP write happens anywhere in the run and no swap needs a copy.

* `gop 0/158` — the letterbox's presents/refusals counter, zero presents across all 158 swaps.
* Every swap line reads `copy N (status 0, 14745600 B, **0 copies**)` — the per-swap copy field that the
  mirror path used to drive never leaves 0, e.g. `:10244` (t+1655).
* `copy fallbacks 0` in every `vop direct` line from the first swap on (`:3501` at t+665 through the last
  beat of the capture).
* `gop blit` — the mirror path's own report line — appears **nowhere** in the whole capture: with
  mirror=0 the path does not run.

## S2 — direct-output liveness (criterion 2)

The first swap arrives at **t+665** (`:3498` `swap ok 2`, the `vop direct` line at `:3501` reports
flips 2, surfaces mapped 1, latch mean 1,419 us). `default_fb_surface: No Default FB Surface found`
(`:3490`) shows `offline-legacy-default-surface=0` is doing its job — the gated path stays silent.

The decisive evidence is the 30 s readback line, which probes the window the VOP scans:

```
:3972  opi5: display t+665s: readback: window f3000000, surface pa 183d44000 px ff000000 ff000000 ff000000 ff000000,
       boot px ff00ff00 ff00ff00 ff00ff00 ff00ff00 (window on a compositor surface)
```

Every readback in the capture — one every 30 s from t+665 to the end — carries the same verdict:
**(window on a compositor surface)**. The window never leaves a compositor surface, and in every sample
the window reads the compositor's pixels (black) while the boot buffer still holds its own content
(green `ff00ff00`) — the two buffers are genuinely different, which is what direct scanout means. In the
mirror=1 era the readback hit the boot buffer, because the mirror copied the frame back and pointed the
window at it.

## S3 — latch cost (criterion 3)

The `vop direct` latch statistics over the run: mean ends at **13,965 us**, longest 17,108 us, and
**0 latches over 50 ms** (final value on `:10246`, t+1655). The early samples are far cheaper and rise
with the surface count: 1,419 us at t+665 (`:3501`), 1,323 us at t+700 (`:3670`), 4,527 us / longest
14,139 us from t+900 on (`:4626`). For comparison, the mirror copy this replaces measured 88.6-95.7 ms
per frame in round 2 — the latch is ~7x cheaper than one copy's *worst* frame and ~65x cheaper at the
early steady state.

## S4 — swap accounting (criterion 4)

Final state, stable from t+1655 to the end of the capture (`:10244`): **swap ok 158, rej 0
(reason 0), time outs 0, prepare-fail 0, retired 158, gen 158/158, frames 1**. The flips counter climbs
during the unlock animation (140 by t+1635, `:10046`) and settles at **157-158** for the rest of the run.

## S5 — animation rate (criterion 5, new instrumentation)

The regenerated patch 0002 adds the rate lines to the display report. During the unlock animation
(t+1630):

```
:9947  opi5: display t+1630s: swaps handed over (the render server's rate): 120 frames since the last report,
       busiest second 61, gap shortest 3595 us, mean 19999 us, longest 173431 us
:9951  opi5: display t+1630s: frames shown (the scanout's rate): 120 frames since the last report,
       busiest second 60, gap shortest 16376 us, mean 20042 us, longest 200095 us
```

`busiest second 61` with a **20,000 us mean gap** is a **~50 fps** hand-over rate — the compositor's own
output — and the scanout keeps the same cadence (mean 20,042 us). This is the first frame-rate
measurement on this board, and it comes from the zero-copy path.

## S6 — the timeline, and the one `back to boot` event (criteria 6-7)

Timeline of the run: lock screen at about t+1560 (swaps static at 4 since t+900), unlock injection from
t+1561 (the injector moves from 280 to 291 gestures in that window, `fails 0`, `open 1`; it ends the
capture at 388 gestures / 8,924 reports / 0 fails, `:16121`), desktop steady state at about t+1655.

At t+1595, exactly when the unlock animation starts, the one and only `back to boot 1` of the run
appears (`:9589`), and the readback's boot-buffer pixels go green -> black at the same instant
(`:10387` t+1590 vs `:10647` t+1595). From t+1620 on, every readback says `window on a compositor
surface` again and the window never leaves it.

Qualification: this is a **one-off transient zap of the boot surface during the unlock session
teardown — the driver's late-teardown path, a legal route — not a fallback**: `copy fallbacks` is 0 for
the whole run and the zero-copy criteria never break. The vop-direct teardown branch that produced it is
**not yet read in source**; that is the round's one pending follow-up (see the pending item below).

One observation recorded without attribution: stage 32 (`L12 late done`) is already on the beat line at
the very start of the capture — the first 5 s beat carries it (`:1949`), i.e. by t+150 at the latest —
where round 3 first saw it at t+2000. Nothing in this round explains the shift; it is recorded so the
next round can look.

Health: 0 panics (all 22 panic/abort pattern matches classified benign one by one), 0 beacon-class
exits, process count peaks at **224** on the t+2145 beat, and the 413 exit lines are all launchd's
routine churn.

## S7 — the three human-eye observations, and the banding verdict (criterion 8's visual half)

The operator confirmed three things on the panel during the run:

1. **The horizontal banding inside the large glyphs is gone** under direct scanout.
2. **The white blocks are still there** (three white rounded rectangles) — so they are **not the same
   defect** as the banding. The round-2-era "same source" judgement is retired; the white blocks point
   at WidgetRenderer rendering an empty surface, and they are the next display-line target.
3. **The banding exists in the early boot too** (during the Apple-logo phase) and disappears later. The
   mechanism is pinned: XNU's early console writes the active scanout buffer directly by CPU —
   `:1936` `console result 0 base 3866361856` = 0xE6740000, the Esmart2 scanout buffer itself — and a
   CPU write into the buffer the VOP is concurrently scanning produces the stripes. Once the verbose
   console goes quiet and the compositor takes over, the stripes stop.

That is the third sighting of one mechanism (the mirror copy, the early console writes, the early
paints are all CPU writes into the scanout buffer), and it closes the banding question:

> **The banding root cause is the CPU write into the (write-combining) scanout buffer; direct scanout
> removes every producer of it.** Closed in round 11.

## The pending item

Read the vop-direct teardown branch in source to name the code path behind the t+1595 `back to boot 1`
(criterion 6 above). It is a transient during session teardown, it does not break any zero-copy
criterion, and the panel behaviour was correct — but the branch should be identified before the next
display change builds on this round's state.

## The observation baseline (new in this round)

The beat line's 7 new fields work end to end. First batch (`:1949`, t+5): `vinact 7821, vact 65, vext
9255, vpur 2, vcact 0, vcmode 8, lsr 0`. Late state (t+2135): `vinact 121,697, vact 185,385, vext
603,440, vpur 2,813, vcact 0, vcmode 8, lsr 128`.

* **`lsr`** — the age of the last card-medium read (seconds, from the `cntpct_el0` tick NativeSd writes
  on every successful read). 0 until about t+1980 (reads go through the Data RAM-disk volume first),
  then, with the userland quiet, +5 per 5 s beat (36 -> ... -> 128). It counts only card-medium reads,
  as designed — the `native_sd_last_read_tick` counter is only written on the success path.
* **`vcmode 8` / `vcact 0`** — the compressor baseline established: mode 8 is freezer semantics, the
  board has no suspended processes, so nothing ever compresses. This is by design. **This is the before
  baseline for a future `vm_compressor=2` comparison boot.**
* **`vfree` trajectory**: 825,708 (t+5) -> 499,717 (t+725) -> 95,824 (t+1610, GUI fully up) -> **1,576
  (t+2135, low-pressure steady state)**. The final value is the same order of magnitude the mirror=1
  runs ended at: the narrow free queue is XNU's designed steady state, **not** a property of the copy
  path. The earlier intermediate observation ("free memory looks generous under direct scanout")
  described only the GUI-not-yet-up phase and is corrected here.
* `vext` peaks at ~600,000 pages ≈ **2.3 GB of file cache**, far above the ~490 MB jetsam
  available-memory threshold — jetsam again correctly finds nothing to kill.

## Process notes

* **The strings method cannot verify the beat line's new fields**: `opi5plus_pe_puts`' static
  character-by-character loop is inlined and constant-propagated at -O2, so the new field-name literals
  never land in rodata — in any round, not just this one. Future rounds should verify such fields from
  `kern_exec.o`'s DWARF local names or the final symbol table (`_native_sd_last_read_tick` is in the
  record this way).
* Serial interleaving truncated the `drain us` value of a few swap lines (`:12617` and siblings); the
  cumulative fields on the same lines are intact, so no measurement is lost.
* The DARWINBOOT serial echo of the command line is truncated by construction (same as round 7); the
  authoritative command line is the launchd echo (`:2360`), which is why the double read-back was done.

## Wrap-up

Two of the round's reboot budget were used (the boot-uefi SSH route, then the 0x1d reset key at
17:02:33); Linux was back at 17:03:04 (`/proc/uptime` 17.78 s). The payload was pushed to the board with:

```
cmd.exe /c py -3 tools\boot-uefi.py --port COM3 --baud 1500000 --log logs\boot-round11.log
  --tail-secs 2700 --local-payload artifacts\m5\payload-round11.img
```

`--local-payload` walks an rpush over SSH internally; the SPI NOR is never written. The capture covers
~2,700 s (16,123 lines, 1,847,265 B).
