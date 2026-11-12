# Round 12 result — the mali relay is signed with the owning process, the sampler moves to WidgetRenderer_D, and the white-block hypothesis is revised

Date: 2026-11-12 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One boot; t0 = 18:48:45, the kernel's first line. Every line cited here lives in
`logs/boot-round12.log` of the **project working tree, not committed** (`logs/**` is gitignored).

## What the round did, and why

The fifth of the user's five corrections: **locate the white blocks** with three instruments, all
landed in one boot:

1. **The sampler retargeted.** `bt3` moved from `liveactivitiesd` to **`WidgetRenderer_D`** — the
   p_comm the "opi5plus: ps" table and the mali sandbox-grant line print for the white
   rounded-square renderer (full name WidgetRenderer_Default, truncated to 16 characters by
   MAXCOMLEN, exactly what the sampler's `strncmp(..., 16)` compares); `bt5` (contextstored) and
   `bt6` (biomed) are dropped to leave the command line headroom. `bt`/`bt2`/`bt4` keep
   SpringBoard/backboardd/chronod.
2. **The validateSwap rejection path got its own print.** A rejected swap's surface geometry
   (width/height/format/rowBytes), the request's size and the framebuffer user client's pid, logged
   at most once per minute — a rejecting surface can recur at swap rate, so the rate limiter says how
   many rejections it swallowed since.
3. **The mali user-message relay is signed.** Every `mali: user:` line now carries the owning
   process's name and pid (`mali: user: %s[%d]: %s`), so the metal2vulkan degrade decisions and the
   Vulkan failures — among them the round-2-era "A8Unorm ... 332x200 ... ERROR_INITIALIZATION_FAILED"
   line — are attributable.

### Changes

| file | change |
| --- | --- |
| `board.toml` | sampler slots reblooded: `opi5plus-bt3=WidgetRenderer_D` (`:374`), `bt5`/`bt6` entries deleted, comment rewritten (`:365-373`); `opi5plus-bt-secs=30` untouched (`:376`) |
| `package/Opi5plusDisplay/NativeLegacyDisplay.cpp` | the rejection print: `native_legacy_client_pid` extern (`:40`, the definition has been in the library since commit a72f3c4), rate-limited block at `:1072-1090` — surface `width x height format rowBytes`, request size and client pid (`:1079`, `:1083`) |
| `package/Mali/Mali.cpp` | **in the nested git repository `package/Mali`** (its own .git, HEAD `7167b67` before this round; the device repo's `.gitignore` carries `/package/Mali/`, so the pointer never enters the device repo): `ownerPid` member (`:2054`), captured at `initWithTask` (`:2158`), and the MALI_LOG relay printf signs every line — `printf("mali: user: %s[%d]: %s\n", ownerName, ownerPid, i.text)` (`:2540`) |

The command line is 939 B / 51 arguments (round 11: 985 B / 53) — the slot comment rewrite and the
two dropped `bt5`/`bt6` entries bought the headroom.

## R0 — build identity

| item | value |
| --- | --- |
| kernel sha256 | `5f101d81d1581567c015a4af81e9119b28d059e948dc88d433b6506cb7007354` (51,699,712 B); the build passed once, BUILD_RC = 0, the bootstrap pass's `ld: symbol(s) not found` is error form 1, the known baseline |
| payload | `artifacts/m5/payload-round12.img`, 96 MiB, sha256 `6eeb9888090e9b51bcec4e1862d2a1fbcee4df4aa4da713c571ecd024a59cdff` |
| compare-kernel vs `d6861991` | NOT EQUIVALENT (expected — the sources changed this round) |
| build harness | first run of the `.env`-reading harness scripts: both `tools/wsl/` scripts took `UNIXNU_UNIQUE_CHIP_ID`/`RK_MAC`/`GIT_PROXY` from the gitignored `/mnt/d/rk-mac/.env` and the build ran with **zero script modifications** |

## S1 — the command line, verified on the board

Criterion: the new sampler arguments survive the FAT round-trip and launchd echoes exactly them.

* The launchd echo (`:2364`, CR-inclusive count — see the caliber note at the bottom) carries
  `opi5plus-bt=SpringBoard opi5plus-bt2=backboardd opi5plus-bt3=WidgetRenderer_D
  opi5plus-bt4=chronod opi5plus-bt-secs=30` — and no `bt5`/`bt6` — measured **939 B / 51 tokens**,
  byte for byte what `board.toml` resolves to (FAT read-back was the first half of the double
  verification).

## S2 — the sampler slot took effect (bt3 = WidgetRenderer_D)

* First `WidgetRenderer_D` sample at **t+1590 s** (`:8390`, `opi5: ubt WidgetRenderer_D at 1590 s:
  cache base 0x180000000 first 0x1d1f0000 slide 0x1d1f0000`) — the same instant the renderer opens
  its mali connection (`:8606`, below). The first sample carries **four thread stacks**
  (`:8388`-`:8395`, tids 0x1748/0x174f/0x1757/0x17ae), and the slot re-samples every 30 s
  (`bt-secs=30`) through t+1800 and beyond.
* `chronod` samples beside it from the same beat (`:8396`), as in rounds 4-11.
* SpringBoard/backboardd keep their slots (the r4-r11 behavior, unchanged).

## S3 — the pid attribution table, and the round-2-era hypothesis

The signed relay is the round's decisive instrument. Final counts over the whole capture:

| process | relay lines | what it is |
| --- | --- | --- |
| `backboardd[57]` | **3,643** | the compositor's Metal client — plugin load, all 74 degrade decisions, the Vulkan failures, every status tick |
| `SpringBoard[27]` | 96 | its own client (status ticks, `calls`/`mem` rows) |
| `CollectionsPoste[528]` | 16 | its own client |
| `WidgetRenderer_D[491]` | **1** | only the bundle-loaded line (`:8607`, `mali: user: WidgetRenderer_D[491]: [WidgetRenderer_Default] metal: bundle loaded in WidgetRenderer_Default (pid 491)`) |

* The connection line is unambiguous: `mali: connection opened by WidgetRenderer_D` (`:8606`),
  bundle loaded at board time t+1590 s, between the t+1590 and t+1620 beats.
* **Every one of the 3,756 `mali: user:` lines carries a `[pid]`** — zero unattributed relay lines
  (verified by LF-line count over the finished capture; the working count during the round's wrap-up
  was 3,735 — the capture kept appending backboardd status ticks after the count, 3,622 -> 3,643 in
  that column, the other three rows unchanged).
* **The degrade/`ERROR_INITIALIZATION_FAILED` population is 100% `backboardd[57]`**: 74 degrade
  lines plus the standalone Vulkan error line (msg 33, `panvk_image.c:459`, `VK_ERROR_INITIALIZATION_FAILED`)
  = 75, and 75/75 name `backboardd[57]`. WidgetRenderer_D contributed exactly one relay line and it
  is not a degrade.
* **332x200 / 332x352: zero occurrences in the whole capture** — the round-2-era signature line
  ("A8Unorm texture of 332x200 over rows 352 ... ERROR_INITIALIZATION_FAILED") did **not** reproduce
  this boot. The kinds observed are the 23 round-11 kinds (same structure); `shadowed-texture`
  prints 31 times vs round 11's 35, nothing new appeared.

## S4 — validateSwap rejections: instrument in place, never triggered

The rejection print (`:1079`/`:1083`) is live in the payload, but the run **never rejected a swap**:
`rej 0` on all 100 beat lines and zero `swap rejected` lines anywhere in the capture. The rate
limiter's `skipped` counter therefore never printed either. Recorded as untriggered, not as pass —
the surface-geometry-and-pid evidence the print was built for stays available for the next boot in
which a rejection happens.

## S5 — `lsr` and the "never read" marker: unobservable this boot

The round-11 fix (`last_tick == 0` prints `-` instead of 0) is in this kernel, but the branch has no
triggering condition on this run: **`lsr -` appears nowhere** (0 occurrences). The SD card reads
were dense from the first beat on, so `last_tick` was never 0 at a beat — the first beat already
reads `lsr 0` (`:1953`), which after the fix unambiguously means **"the last read finished less than
a second ago"**. No ambiguity remains in the data; the marker itself is unexercised. The late
trajectory is the same +5-per-5-s-beat price card reads by age as round 11 published: 2, 7, 12, 17,
22 over the last beats (`:16525`-`:16699`; the working count froze at 12, `:16667`, the capture ran on).

## S6 — parity against round 11

| counter | round 12 | round 11 | verdict |
| --- | --- | --- | --- |
| swap ok (final) | **104**, rej 0 (reason 0) | 158, rej 0 | both 0 refused; the total differs and is recorded as-is: r11's capture window had the unlock animation producing more frames (`:16518` t+2580 = final 104; first `swap ok 104` at t+1660, `:9346`) |
| copies per swap | `0 copies` on every swap line (`:3618` first, t+750) | 0 copies | ✓ |
| gop presents/refusals | `gop 0/104` on every beat | `gop 0/158` | ✓ (letterbox never presents) |
| copy fallbacks | 0 on all 100 `vop direct` lines | 0 | ✓ |
| `vop direct` latch | mean **10,654 us**, longest 16,976, 0 over 50 ms (`:16519`) | mean 13,965 / longest 17,108 | ✓ |
| flips / mapped / remaps | 103 / 3 / 3 (`:16519`) | 157 / 3 / 3 | ✓ (remaps 3 both rounds) |
| `back to boot` | 1, cumulative from the unlock on | 1 | ✓ (designed blank-swap path, r11 S6) |
| injector | **469 gestures / 10,777 reports / fails 0** at t+2610 (`:16723`); 463/10,649 at the working count's freeze, fails 0 both readings | 388 gestures / 8,924 reports / 0 fails | ✓ (gestures keep coming, `open 1` throughout) |
| panics | 0 real (all 23 raw `panic`-pattern matches are the benign classes r11 classified: the iBoot extraction note, the `offline-legacy-default-surface` comment lines, the boot-args echo carrying `panic_on_crash_disabled`) | 0 | ✓ |
| process count peak | **224** | 224 | ✓ |
| `vsync` | not counted separately this round; swaps complete as before | sent 0 | — |

Timeline: lock screen to t+1560 (swap static at 3, `:8100`), unlock animation from t+1590
(swap 11 at `:8352`), the counter climbs 14 -> 89 -> 97 -> 103 -> 104 by t+1660 (`:8884`-`:9346`)
and sits at 104 to the end of the capture (`:16518`). The mali connection of WidgetRenderer_D opens
inside that window (t+1590 s, `:8606`).

## S7 — wrap-up

The run ended early: with the swap counter saturated and every parity number on the table, the
capture was stopped (taskkill of the detached reader, PID 48924) and the board was reset with
0x1d via `tools/wsl/serial-reset.py` (reset quota 2/2). Linux was back 28 s later
(`/proc/uptime` 17.71 s). The payload was pushed to the board with:

```
cmd.exe /c py -3 tools\boot-uefi.py --port COM3 --baud 1500000 --log logs\boot-round12.log
  --tail-secs 2700 --local-payload artifacts\m5\payload-round12.img
```

`--local-payload` walks an rpush over SSH internally; the SPI NOR is never written.

## The white-block conclusion, and the next step

* **The round-2-era hypothesis "A8Unorm 332x200 = the white block" is retired for good.** This boot
  is the direct test: the renderer opened its GPU connection and loaded its bundle
  (`:8606`-`:8607`), the relay is fully signed, and the 332x200/rows-352 signature appears **zero
  times** while the white blocks were on the panel. Negative result, confirmed.
* The degrade/failure path is 100% `backboardd[57]` — nothing in the failure population belongs to
  WidgetRenderer_D.
* What the evidence now points at **(conjecture, not confirmed)**: the white blocks are a
  WidgetRenderer surface-*content* problem — a surface is created and handed to the compositor, but
  its content is never rendered into it, and the compositor composites the allocation's white
  fallback. The one relay line WidgetRenderer_D produced (bundle loaded, `:8607`) shows the process
  alive in the driver but silent afterwards.
* Next-step suggestion (user decision): give the WidgetRenderer_D side its own observation points —
  surface-commit/content observability (a sampler slot is already in place; the driver side could
  log the surfaces it binds) before scheduling another board round.

## The pending human-eye item

The operator was not at the panel during this run. The three observations round 11 confirmed by eye
remain unconfirmed this round: **(a) the white blocks are still there, (b) no banding/stripes, (c)
the animation is consistent**. The working hypothesis carries round 11's human-eye verdict
(stripes gone under direct scanout) forward; the supporting instrument parity above is full-width
aligned. **All three are due on the next board boot.**

## Verbatim log excerpts

The capture lives only in the project working tree. `logs/boot-round12.log`, 16,726 LF lines.
Line numbers are 1-based counting only LF (`\n`), i.e. what ripgrep prints; the file's breaks are
CRLF and the stray CR characters from serial interleaving make a CR-inclusive count drift ahead of
these numbers — this page's three CR-inclusive citations from the round's working count are marked
inline (`:2364` boot-args echo = LF 2237, `:4143` first-batch swap = LF 3618, `:18173` final
`vop direct` = LF 16519). Every other number on this page is the LF count.

* The launchd command-line echo (CR-inclusive 2364 = LF 2237):

```
com.apple.xpc.launchd|1969-12-31 16:05:33.098120 <Notice>: boot-args = -v serial=1 debug=0x10014e ... opi5plus-bt=SpringBoard opi5plus-bt2=backboardd opi5plus-bt3=WidgetRenderer_D opi5plus-bt4=chronod opi5plus-bt-secs=30 panic_on_crash_disabled=1
```

(the full line is 939 B / 51 tokens; the middle is elided here only for width — the FAT read-back
and the echo agree byte for byte)

* The first swap of the run (LF 3618, t+750; CR-inclusive 4143):

```
opi5: display t+750s: swap ok 2 rej 0 (reason 0) copy 2 (status 0, 14745600 B, 0 copies) retired 2 gen 2/2 frames 0 gop 0/2 timeouts 0 prepare-fail 0, sd pio bursts 19929757, chunk us ...
```

* The renderer's first sample and its connection (LF 8390, 8606, 8607):

```
opi5: ubt WidgetRenderer_D at 1590 s: cache base 0x180000000 first 0x1d1f0000 slide 0x1d1f0000
mali: connection opened by WidgetRenderer_D
mali: user: WidgetRenderer_D[491]: [WidgetRenderer_Default] metal: bundle loaded in WidgetRenderer_Default (pid 491)
```

* The final `vop direct` line the S6 table cites (LF 16519; CR-inclusive 18173 — the number the
  working count cites):

```
opi5: display t+2580s: vop direct (iommu 1, status 00000019 00000019): flips 103, surfaces mapped 3 (map failures 0, let go 0, remaps 3, zap timeouts 0), latch mean 10654 us longest 16976 us (0 over 50 ms), copy fallbacks 0, back to boot 1
```

* The final swap line (LF 16518):

```
opi5: display t+2580s: swap ok 104 rej 0 (reason 0) copy 104 (status 0, 14745600 B, 0 copies) retired 104 gen 104/104 frames 1 gop 0/104 timeouts 0 prepare-fail 0, sd pio bursts 75, chunk us ...
```

* The final injector heartbeat (LF 16723, t+2610 — after the wrap-up decision, before the reset):

```
opi5: hid: inject: t+2610 s: gestures 469 reports 10777 fails 0 open 1 driver 0xfffffff36c0bc700
```
