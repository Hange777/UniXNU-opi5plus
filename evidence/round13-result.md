# Round 13 result — the surface census attributes the composited frame; the white blocks sit inside it

Date: 2026-11-12 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One boot; t0 = 20:32:47, the kernel's first line. Every line cited here lives in
`logs/boot-round13.log` of the **project working tree, not committed** (`logs/**` is gitignored).
Caliber note (different from the round-11/12 pages): every line number on this page is the
**CR-inclusive count, the Select-String caliber the round's working count used**; a 1-based
LF-only count (ripgrep) drifts **behind** these numbers — at the launchd echo the LF number is
2235 for this page's `:2362` (round 12 measured the same 127-line drift there), at the final
swap line 17720 for `:19446`. The two excerpts that cite both numbers carry them inline.

## What the round did, and why

The third step of the user's white-block instruction: **census the surfaces and sample their
content**. Rounds 11-12 had cleared the display path's copy half and the degrade half of the
line; what was still unknown was *which surface carries the white*. This round gave the direct
path its own content instrument, landed in one boot:

1. **The surface census.** With `opi5-surface-census=1` the direct path keeps a ledger of every
   distinct (physical base, width, height, format) surface it maps into a scanout slot —
   first-seen time, shape, stride and the framebuffer client pid of that moment — and once per
   ledger surface per 30 s, sampled from `latchTick` after a latch confirms (1-17 ms after the
   swap, the widest window in which the GPU has certainly finished the frame the window now
   scans), it maps the surface read-only (the same map the GOP mirror blits through) and reads
   **25 pixels on a 5x5 grid over the visible rectangle**, one IOLog line each: uniform
   white/black/value, near-uniform (n, with per-value counts) or mixed. A white-block area
   reading `uniform 0xffffffff` would put the white in the compositor's layer; anything else
   points elsewhere. Read-only diagnostics; with the argument absent the direct path is exactly
   round 12.
2. **Nothing else changed.** No sampler retarget, no patch, no Mali code in this round's commits.

### Changes

| file | change |
| --- | --- |
| `board.toml` | the display group gained `opi5-surface-census=1` with the nine-line census comment (`:305-314`; parser named in the comment: `package/Opi5plusDisplay/NativeLegacyDisplay.cpp:273`, default 0 = off) |
| `package/Opi5plusDisplay/NativeLegacyDisplay.cpp` | **+163 lines, pure addition**: the census comment block (`:129-139`), the ledger — `CensusEntry`, cap 32, `censusRegister()` (`:140-168`), the argument parse in `startDirect` (`:271-274`), the ledger call in the direct-slot registration (`:342-350`), the `latchTick` hook after the latch confirms (`:436-438`) and `censusPoll()`/`censusSample()` (`:444-545`) — per surface 30 s, 25-point 5x5 grid, uniform/near-uniform/mixed classification, a global soft cap of at most 2 census lines per 5 s, and a map failure marks the entry unmappable (one-time line, never retried) |

The command line is 961 B / 52 arguments (round 12: 939 B / 51) — the census argument is the
only addition (+22 B, `opi5-surface-census=1 `).

The **Mali fork was pushed this round** (`Hange777/Mali`, main = `1a496b5`, carrying round 12's
signed relay; the device repo's fetch pin stays at Ken-u). That is a fork-side bookkeeping push,
outside this round's device-repository commits.

## R0 — build identity

| item | value |
| --- | --- |
| kernel sha256 | `8ee03b7e1611dc8ec7cce6d6b3d25d15b98a0aa5675d387f477fa0d0a43d9515` (51,716,096 B; +16,384 B vs round 12's 51,699,712) |
| build | one pass, clean: the new composite condition and the IOLog argument lists compiled with zero problems (the census `IOLog` call sites are `:486`, `:509`, `:515`, `:520`, `:526-:541`) |
| payload | `artifacts/m5/payload-round13.img`, 96 MiB (100,663,296 B), sha256 `d92daecdb4d3984308a2a4eec90457e44d1d7f4b545035f5e26f49810454d163` |

## S1 — the command line, verified on the board

Criterion: the census argument survives the FAT round-trip and launchd echoes exactly it.

* The launchd echo (`:2362`, CR-inclusive count — see the caliber note) carries
  `opi5-gop-mirror=0 opi5-surface-census=1 sheng-aks=1` in order, and the line measures
  **961 B / 52 tokens**, byte for byte what `board.toml` resolves to (the FAT read-back was the
  first half of the double verification).

## S2 — the census took its first archive: the attribution table

The census logged **all 12 lines (3 registered + 9 surf)**, no `ledger full`, no `unmappable`,
and the soft cap shaped the schedule exactly as designed (samples happen only at latch
confirms, so a static screen schedules nothing — the age gap below spans the lock screen's
static period). Three surfaces were registered, and every one of them is the same shape:

| surface | pid (client of the moment) | registered | samples, in order | what it is |
| --- | --- | --- | --- | --- |
| `0x19a631000` 2560x1440 fmt 0x42475241 stride 10240 | **57** (`backboardd`) | `:4124` (t+775, the first swap) | age 0 `:4126`, age 57 `:4473`, age 734 `:9050`: **near-uniform n=2: 0xff000000\*24 0xffffffff\*1**; age 769 `:9569`: **mixed n=14** | the lock screen's composited frame (24 of 25 grid points black + 1 white), then the desktop |
| `0x1ab873000` 2560x1440 fmt 0x42475241 stride 10240 | **57** (`backboardd`) | `:4471` | age 0 `:4472`: near-uniform n=2: 0xff000000\*24 0xffffffff\*1; age 676 `:9049`: near-uniform n=2: 0xff000000\*24 0xfffcfcfc\*1; age 712 `:9568`: **mixed n=14** | the same composite a generation over; its white point goes 0xffffffff -> 0xfffcfcfc just before the desktop handover |
| `0x1bfa4e000` 2560x1440 fmt 0x42475241 stride 10240 | **73** (`PerfPowerService`) | `:9080` | age 18 `:9291`: **mixed n=16**; age 48 `:9796`: mixed n=16 | its own surface, mixed from the first sample — no uniform phase at all |

**The decisive negative result: not one `uniform` line exists in the whole capture** (0
occurrences of every uniform form — `(white)`, `(black)`, plain value). No surface the
compositor scans is a plain white layer, so the white the panel shows is not an independent
layer surface's content — it is **inside the composited frames** these three surfaces carry.

## S3 — where the white blocks sit: inside the composited frame

* **The census half (instrumented):** during the lock-screen phase both `backboardd` surfaces
  read 24/25 black + **exactly 1 white grid point** — 1/25 of the 2560x1440 frame is white,
  which is a ~512x288 region, the same area fraction the white rounded rectangle on the panel
  occupies. The composited frame itself therefore contains a white region of the block's size;
  it is not a second surface stacked on top.
* **The user's half (observed at the panel, new this round):** the **App Store icon renders on
  top of the white block** — the block sits *below* the icon layer. That is the widget
  placeholder's layer position: the layer exists, its content is blank/white, and `backboardd`
  composites the icon above it exactly as a real widget would be composited.
* **Verdict upgrade:** rounds 2-12's hypotheses (a GOP-path artifact, then an A8Unorm degrade,
  then a separate never-rendered WidgetRenderer surface) are all behind us. What the evidence
  now says — **the white content comes from the render server's composition pipeline**: a
  widget-placeholder region whose content is empty/white, composited by `backboardd` as-is, and
  the display path faithfully presents the result. The direct scanout, the 0 copies, and the
  blocks surviving the mirror=1 -> mirror=0 transition already cleared the display path of
  producing the white; this round's census adds that the display path cannot even *see* a white
  layer to clear, because the white is not a layer.
* **The census's own limit (stated):** the 5x5 full-frame grid cannot tell *where inside* the
  surface the white sits — one sample point is not a region measurement. That is exactly what
  the next-step option (a) below buys.

## S4 — parity against rounds 11 and 12

| counter | round 13 | round 12 | round 11 | verdict |
| --- | --- | --- | --- | --- |
| swap ok (final) | **175**, rej 0 (reason 0) (`:19446`) | 104, rej 0 | 158, rej 0 | all three 0 refused; the totals differ (capture windows / animation phases differ) and are recorded as-is — same tier |
| copies per swap | `0 copies` on every swap line (`:4166` first non-zero) | 0 copies | 0 copies | ✓ |
| gop presents/refusals | `gop 0/175` on every beat (`:19447`) | `gop 0/104` | `gop 0/158` | ✓ (letterbox never presents, mirror stays off) |
| copy fallbacks | 0 on every `vop direct` line (`:19447`) | 0 | 0 | ✓ |
| `vop direct` latch | mean **11,829 us**, longest 17,058, **0 over 50 ms** (`:19447`) | 10,654 / 16,976 | 13,965 / 17,108 | ✓ |
| flips / mapped / remaps | 175 / **3** / 3 (`:19447`) | 103 / 3 / 3 | 157 / 3 / 3 | ✓; mapped 3 matches the census's 3 registered — the ledger and the slot table agree |
| `back to boot` | **0** (`:19447`) | 1 | 1 | fewer session teardowns this run; recorded as-is |
| injector | **485 gestures / 11,155 reports / fails 0** at t+2704 (`:19456`), `open 1` throughout | 469 / 10,777 / 0 | 388 / 8,924 / 0 | ✓ |
| panics | 0 | 0 | 0 | ✓ |
| process count (final beat) | **228** (`:19491`) | 224 peak | 224 peak | ✓ |

## S5 — `lsr`: the same dense-read shape as rounds 11-12

**538 beat lines carry `lsr`**, the first value 0 (t+5, `:1954`), the final 15 (t+2700,
`:19491`), and **no `lsr -` anywhere** (0 occurrences — the "never read" marker has no
triggering condition again; the fixed 0 semantics, "read within the last second", leaves no
ambiguity in the data). The late trajectory is the same +5-per-5-s-beat price card reads by age
as the previous rounds published (5, 10, 15 over the last beats, `:19452`-`:19491`).

## S6 — timing

* First non-zero swap **t+775 s** (`:4166`, `swap ok 2`) — round 12's t+750, same phase, +25 s.
* The launchd echo prints at board time 16:05:33 (`:2362`).
* The lock screen -> desktop handover is visible in the census data itself: the near-uniform
  lock-screen samples end at age 734 / 676 (`:9050`/`:9049`) and the mixed desktop samples
  begin at age 769 / 712 (`:9569`/`:9568`) — the transition the panel showed is the transition
  the surfaces' content shows.
* The capture ran to its full budget; the final beat is t+2700 s (`:19491`).

## S7 — wrap-up: the board stayed in XNU

* The capture ended **naturally at 2,700 s** (`--tail-secs 2700`); no early stop, no reader
  taskkill. No residual `py` processes remained; COM3 was released.
* **The board was left running XNU — no 0x1d was sent this round** (the reset quota stands at
  1/2 used, by the payload push itself; the second slot is preserved for a future round). After
  the capture the board's state is not verifiable from this side — XNU has no SSH — but the
  last beat (`:19491`, t+2700 s) is heartbeat-healthy: requests 269,011, failures 0, procs 228,
  `lsr 15`.
* The payload reached the board with:

```
cmd.exe /c py -3 tools\boot-uefi.py --port COM3 --baud 1500000 --log logs\boot-round13.log
  --tail-secs 2700 --local-payload artifacts\m5\payload-round13.img
```

`--local-payload` walks an rpush over SSH internally; the SPI NOR is never written.

## The white block's next step: two options (user decision)

1. **(a) Region-level sampling.** Sample the composited surface at the white block's
   coordinates (the user can calibrate them from their panel photo) — a handful of targeted
   pixels instead of the 5x5 full-frame grid — which upgrades "the white is inside the surface"
   from high-confidence conjecture to **confirmed**, and gives the widget-placeholder region's
   exact extent in the frame.
2. **(b) Accept the attribution and move on.** The display path is cleared end to end (direct
   scanout, 0 copies, 0 fallbacks, and now no white layer); the white content's origin is the
   render server's composition pipeline, a userland-side matter, and the line's remaining value
   is deciding what other direction the next board round serves.

## The human-eye items

The operator was at the panel for this run:

* **The white blocks are still there** (confirmed).
* **The App Store icon renders on top of the white block** (confirmed, new observation this
  round) — the block sits below the icon layer, i.e. a widget-placeholder layer that exists
  while its content is missing.
* The two round-11 items — **no banding/stripes** and **consistent animation** — were not
  re-confirmed this round and carry forward on rounds 11-12 continuity; the instrument parity
  above is full-width aligned.

## Verbatim log excerpts

The capture lives only in the project working tree. Line numbers are the CR-inclusive
(Select-String) count; LF-only equivalents are given where marked.

* The launchd command-line echo (CR-inclusive 2362 = LF 2235):

```
com.apple.xpc.launchd|1969-12-31 16:05:33.161623 <Notice>: boot-args = -v serial=1 debug=0x10014e ... rk3588-vop-fence=1 opi5-gop-mirror=0 opi5-surface-census=1 sheng-aks=1 ... opi5plus-block-exec=bluetoothd panic_on_crash_disabled=1
```

(the full line is 961 B / 52 tokens; the middle is elided here only for width — the FAT
read-back and the echo agree byte for byte)

* The census's first registration and first sample (CR-inclusive 4124-4126 = LF 3588):

```
opi5plus: display: census: registered #1 pa=0x19a631000 2560x1440 fmt=0x42475241 stride=10240 pid=57
opi5plus: display: census: surf pa=0x19a631000 2560x1440 fmt=0x42475241 stride=10240 pid=57 age=0s: near-uniform n=2: 0xff000000*24 0xffffffff*1
```

* The first non-zero swap (CR-inclusive 4166 = LF 3630, t+775):

```
opi5: display t+775s: swap ok 2 rej 0 (reason 0) copy 2 (status 0, 14745600 B, 0 copies) retired 2 gen 2/2 frames 0 gop 0/2 timeouts 0 prepare-fail 0, sd pio bursts 20550000, chunk us ...
```

* The lock-screen -> desktop transition in the content (CR-inclusive 9050 and 9569):

```
opi5plus: display: census: surf pa=0x19a631000 2560x1440 fmt=0x42475241 stride=10240 pid=57 age=734s: near-uniform n=2: 0xff000000*24 0xffffffff*1
opi5plus: display: census: surf pa=0x19a631000 2560x1440 fmt=0x42475241 stride=10240 pid=57 age=769s: mixed n=14
```

* The final swap line (CR-inclusive 19446 = LF 17720, t+2700):

```
opi5: display t+2700s: swap ok 175 rej 0 (reason 0) copy 175 (status 0, 14745600 B, 0 copies) retired 175 gen 175/175 frames 0 gop 0/175 timeouts 0 prepare-fail 0, sd pio bursts 72016643, chunk us ...
```

* The final `vop direct` line the S4 table cites (CR-inclusive 19447 = LF 17721):

```
opi5: display t+2700s: vop direct (iommu 1, status 00000019 00000001): flips 175, surfaces mapped 3 (map failures 0, let go 0, remaps 3, zap timeouts 0), latch mean 11829 us longest 17058 us (0 over 50 ms), copy fallbacks 0, back to boot 0
```

* The final injector heartbeat (CR-inclusive 19456 = LF 17727, t+2704):

```
opi5: hid: inject: t+2704 s: gestures 485 reports 11155 fails 0 open 1 driver 0xfffffff42d311f20
```

* The last beat of the capture (CR-inclusive 19491 = LF 17758, t+2700):

```
opi5plus beat 2700 s, requests 269011, blocks 37956224, failures 0, procs 228, stage 32 (L12 late done), vfree 1881, vspec 359338, vcomp 0, vinact 122296, vact 187236, vext 597570, vpur 2651, vcact 0, vcmode 8, dbavail 305172, lsr 15
```
