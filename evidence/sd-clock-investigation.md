# The SD clock investigation — rounds 7-10, from two falsified models to the 26 MHz base clock

Date: 2026-11-12 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One boot per round, t0 = the kernel's first line. Every boot log cited here lives in `logs/` of the
**project working tree, not committed** (`logs/**` is gitignored). This page closes the SD-throughput line:
[`evidence/round4-6-result.md`](round4-6-result.md) S2 established the baseline it starts from (12,484 bursts
per 64 KiB chunk, 5.67 ms per chunk, ~11.3 MB/s).

## R0 — build identity

| round | kernel sha256 | payload (`artifacts/m5/`) | the one change |
| --- | --- | --- | --- |
| 7 | `60f90e50…` — round 6's kernel, unchanged | `payload-round7.img`, sha256 `3c6cbb976d5449858575551d3dc5ac1304876a779a1e8a9a2755f0dd0a8db194` | `sd-clock-div=1` on the command line |
| 8 | `d31fbabf68e3f0a7999613c0016528e5c883cccff27c9c35165c1e533d4cbe3e` (`logs/build-round8.log:89`) | `payload-round8.img`, sha256 `c2cd6eba7bc5be0106cfb8d8c65db10877b83e35631233ddde2ea996de8a64b7` | `kPioReadBatchWords` 128 -> 32 in `package/SdHost/SdHost.h` |
| 9 | `f241e3a9889b2444c5ebb067c0a0b0bd0614f7834b2b4fd4055ec35d75d24e70` (`logs/build-round9.log:109`) | `payload-round9.img`, sha256 `ac98191fe2094e7118241822b6f92def8f95d59973141cd361ace7b302cb98f3` | fill/drain phase counters + CLKDIV/CLKSRC/CLKENA read-back |
| 10 | `4b2e270bf6dfa6bdaf9f9bd670001fd41a018372491f3d9e67e3a80644dd2743` (`logs/build-round10.log:109`) | `payload-round10.img`, sha256 `7d9f0e4d686feddc4479f658c4c45458b400d1b8d23543467ea0758ca8731322` | `kPioReadBatchWords` 32 -> 128 (clean up the 4 us poll granularity for the timing) |

The instrument is the same beat line throughout: `sd pio bursts`, `chunk us`, `chunks` (round 4), plus
`fill us`, `drain us` and the one-time `opi5plus: sd clock: CLKDIV=…` line (round 9). The counters are defined
in `package/NativeSd/NativeSd.cpp` and the clock read-back in `package/SdHost/SdHost.h`
(`setClockDivider`); the beat line prints them (`package/LegacyDisplayProbe.cpp`).

## S1 — the CIU divider is linearly effective (round 7)

Round 7 set `sd-clock-div=1` — the controller's own CLKDIV register, which divides the clock the CIU runs the
card at. Result: **the chunk time doubled, throughput halved.**

```
logs/boot-round7.log:1698   NativeSd: sd-clock-div=1 -> ok
logs/boot-round7.log:1967   opi5: display t+30s: … sd pio bursts 354858, chunk us 29969424, chunks 2736
```

29969424 us / 2736 chunks = **10,957 us per chunk** against 5,671 us at div=0 — 1.93x, i.e. exactly what
halving the card clock does. Throughput fell 11.5 -> **5.99 MB/s**.

This is the positive result the round is for, and it is easy to misread later, so it is stated plainly: **the
CIU clock divider works, linearly.** Setting CLKDIV=1 halves the rate; whatever sets the div=0 rate is
therefore a clock the divider sits on top of, not something CLKDIV bypasses. (A later round's analysis briefly
misread this as "the divider has no effect"; the measurement above is the correction.)

## S2 — batch size is not the constraint (round 8, negative result)

Round 5's two-phase loop drained 128 words per burst. Round 8 dropped `kPioReadBatchWords` to 32, hoping the
shorter drain would overlap the fill and hide the CPU's MMIO time.

```
logs/boot-round8.log:1967   opi5: display t+30s: … sd pio bursts 2319965, chunk us 29303130, chunks 5166
```

2319965 / 5166 = **449 bursts per chunk** (against 121 at batch 128 — the shape changed exactly as coded), and
29303130 / 5166 = **5,672 us per chunk** — the same 5.67 ms. Throughput 11.55 vs 11.52 MB/s, noise.

**Conclusion: the batch size is not the constraint.** Round 8 falsified the second model ("the chunk is
CPU-bound on the 128-word drain; a smaller batch overlaps it"). Whatever sets the 5.67 ms does not scale with
the batch size either — after round 5 removed the poll count and round 8 removed the batch length, the constant
that is left is not a property of the CPU side at all.

## S3 — the line is rate-limited, not CPU-limited (round 9)

Round 9 split the two phases of the read loop onto the beat line (`native_sd_fill_us` = waiting for the FIFO to
fill against the card clock, `native_sd_drain_us` = draining it word by word against the CPU) and read back the
clock registers once, right after a clock update lands — the CLKDIV in the boot log is the firmware's dump from
before the driver took over and cannot answer "which card clock actually ran".

```
logs/boot-round9.log:1700   opi5plus: sd clock: CLKDIV=0 CLKSRC=0 CLKENA=1
logs/boot-round9.log:1969   opi5: display t+30s: … sd pio bursts 2497305, chunk us 29305594, chunks 5162, fill us 23827122, drain us 2639826
```

Two readings, both at batch 32:

* **fill is 81.6% of the chunk time, drain 8.8%.** The read path waits for the card, it does not fight the CPU.
  The "PIO costs too much CPU" model is dead: even the drain phase, the MMIO-bound one, is under a tenth of the
  chunk.
* **CLKDIV reads back 0.** The divider is off at div=0 — so round 7's halving came from turning it on, and the
  div=0 rate is whatever clock feeds the CIU with no division.

## S4 — the base clock is about 26 MHz (round 10)

At batch 32 the `kPioFillWaitUs = 4` polling granularity can add phantom fill time, so the two line-rate
hypotheses (base 52 MHz -> 26 MB/s, base 26 MHz -> 13 MB/s) were not separable: 26 MB/s predicts 19.7 us of
ideal fill per 128-word burst, 13 MB/s predicts 39.4 us, and 4 us poll steps pollute both. Round 10 returned to
batch 128 (granularity <= 4 us per burst) to make the measurement clean:

```
logs/boot-round10.log:1701  opi5plus: sd clock: CLKDIV=0 CLKSRC=0 CLKENA=1
logs/boot-round10.log:1970  opi5: display t+30s: … sd pio bursts 623322, chunk us 29304820, chunks 5153, fill us 25507942, drain us 2625637
```

Fill per burst converges to **41.4 us** (first beat 40.9 us, the later beats 41.2 us and up) — that lands on the
**26 MHz side: predicted 39.4 us, +5% off. The 52 MHz hypothesis predicted 19.7 us and is excluded by 2.1x.**

Per word: 41.4 us / 128 words = 323 ns of fill; counting the whole chunk per delivered word gives ~303 ns per
4-byte word, i.e. a **13.3 MB/s card delivery rate** — which is what 26 MHz x 4-bit bus width works out to
(13 MB/s). The controller is not losing words; it is being fed at the clock tree's rate.

## S5 — the conclusion

1. **The SD base clock is ~26 MHz, set by the CRU clock tree, not by anything the CIU can change.** CLKDIV
   divides on top of it (round 7 proved it works, round 9 proved it reads 0 at div=0, round 10 fixed the base
   rate).
2. **PIO efficiency is 89%** (11.5 MB/s delivered against the 13 MB/s the card clock offers). The software side
   — poll shape (round 5), batch size (round 8), phase split (round 9) — has no further room: every model that
   blamed the CPU was falsified, and the fill phase that dominates is the card's own rate.
3. **The next 2x is not a driver change.** Reaching ~26 MB/s means moving the sdmmc source clock in the CRU
   from 26 MHz to 52 MHz — clock-tree work on the SoC side (`UniXNU-rk3588-common`), then re-measuring with the
   same counters. Until then ~11.5 MB/s is this card's ceiling on this board, and nothing in
   `package/SdHost` / `package/NativeSd` will move it.

## S6 — the builds are not byte-reproducible (side finding)

While closing round 9/10, the same kernel source built twice produced different bytes:
`d78a28dfe07dca9a08793aee03315ed4ede6b5aab6185a44cf7cbee2953fcbca` and
`4b2e270bf6dfa6bdaf9f9bd670001fd41a018372491f3d9e67e3a80644dd2743`, both 51,699,712 B. The incremental build
records a build identity but not a reproducible one. **Judge a kernel by the sha the payload carries, not by
"it should be the same build"** — two rebuilds of one source are two kernels as far as evidence goes. The
before-hash line of a build log (`logs/build-round10.log:3`) is the previous kernel, not the one being built.

### S6a — what actually differs (2026-11-12 experiment)

Two consecutive incremental builds (`tools/wsl/build-kernel.sh 1|2`, three-residue cleanup each time, with
`SOURCE_DATE_EPOCH` pinned to the last device-repo commit) produced
`d6861991475156dc1d114b127d65ff7ba4c96bbc5f2a2ed2ec7615b6b334bd53` and
`8c0ae9c0d2fcff73128ca569211631e992c71a0c4ae473fad372707b8a0ad2d2`. Byte-level diff: 393 differing regions,
12,229 bytes, and **392 of the 393 regions are inside `__LINKEDIT`** — every one is the `n_value` of an
`N_OSO` stab (`n_type 0x66`, name = the linked object's full path) plus one 22-byte diff in `__PRELINK_INFO`:

1. **N_OSO mtimes (969 in one build).** ld64 stamps each linked object path into the symbol table with the
   object file's mtime as its value. Incremental rebuilds rewrite some `.o` files, so the stamps move with
   wall-clock time. `SOURCE_DATE_EPOCH` does not reach them (the timestamps also sit in the prelink *input*
   `mach.development.bcm2837.unstripped.noctf`, i.e. they come from the XNU link, not the prelink).
2. **`_PrelinkKCID` (1 × 16 B, base64).** `kits/XnuPrelink/emit_prelink.py:269` derives it as
   `sha256(kernel + original kernelcache)[:16]`, so it changes whenever any N_OSO byte changes.
3. **LC_UUID** — zeroed by the mask here; the identifier of the image.

A deterministic byte-reproducible build would need the `.o` mtimes clamped to a fixed epoch before the final
XNU link (then the KCID follows), which costs two more build runs to verify — out of budget for this
experiment, so it stays a recommendation, not a device-verified fact.

### S6b — the working judge: `tools/wsl/compare-kernel.py`

Semantic equivalence of two builds is directly decidable by masking exactly the classes above:

```
wsl.exe -d unixnu -u root -e bash -c \
  'python3 /mnt/d/rk-mac/repos/UniXNU-opi5plus/tools/wsl/compare-kernel.py \
   /opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho.1 \
   /opt/unixnu-out/opi5plus/ipados-17.7.11-21H461/opi5plus_display/kernel/kernel.macho.2'
```

On the two experiment builds it reports **SEMANTICALLY EQUIVALENT**
(normalized sha256 `4fe69551b1d9014f8047f7056db48895f71a6ed5788eec0a14924863dd3b3104`; masked: 1 LC_UUID,
969 N_OSO values, 1 `_PrelinkKCID`). A deliberately corrupted code byte makes it report NOT EQUIVALENT with
the offset listed (exit 1), so the judge separates metadata churn from real content. Builds are still run
with `tools/wsl/build-kernel.sh <run-id>`, which cleans the three known incremental-build residues first
(`startup.o.json` moved to `tree/logs/`, xnu-work `startup.c` touched, the regenerated bsd `Makefile`
deleted) and keeps `kernel.macho.<run-id>` for comparison; note its log always shows one `error:` line — the
bootstrap pass fails by design at the link (scripts/unixnu/kernel.py:176) and is not a build failure.

## R1 — captures

| log | size | first beat line |
| --- | --- | --- |
| `logs/boot-round7.log` | 121,741 B | `:1967` |
| `logs/boot-round8.log` | 125,448 B | `:1967` |
| `logs/boot-round9.log` | 128,661 B | `:1969` |
| `logs/boot-round10.log` | 123,837 B | `:1970` |

Project working tree, not committed; `logs/**` is gitignored.
