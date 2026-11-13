# Round 17 result — the freeze is an uninterruptible CoreUI asset page-in, and the taps never reached the desktop

Date: 2026-11-13 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
This round merges three threads: the **round-17 board test** (the spawn ledger, the app-launch tap
and the sampler slots — one unattended XNU boot, `logs/boot-round17.log`, project working tree,
**not committed**), the **R19 static analysis** (`.cache/r19-fate/REPORT.md`: the full fate of the
apps the user opened in rounds 14/15, from the existing logs only) and the **R20 local
symbolization** (`.cache/r20-dsc/` for the r14/r15 chains, `.cache/r20-sym17/report.md` for this
round's freeze window). Caliber note (same convention as the round-16 page): line numbers below
are the **LF-only count** (bare-CR fragments merge into the preceding line); the LF-only total is
12,190 lines, the CR-inclusive (`File.ReadAllLines`) count is 13,831.

## What the round did, and why

Round 16 removed the exit-78 red herring; the app-launch question remained: what does the app
process itself do, and why does the UI freeze. This round attacked it from three sides:

1. **Spawn tracking (patch 0006, `opi5plus-trace-spawn=1`).** Every posix_spawn(2) prints one
   entry line `opi5plus: spawn pid=<caller> path=<target>` and one return line
   `... ret=<error>` at the same posix_spawn top where block-exec refuses bluetoothd; a refused
   spawn prints path and return together. A per-boot soft cap of 512 printed ordinary spawns
   counts past that and tallies one line per 256 suppressed — launchd spawns hundreds, and the
   first hundreds stay verbatim without drowning the UART.
2. **The app-launch tap (`package/Opi5plusHid/NativeHidInput.cpp`).** After the first completed
   gesture the injector stops repeating the unlock cycle and taps the App Store icon (panel
   pixel 1591,439 — the centre of the user's photo position, transformed by the same
   panel-to-window path the gesture uses) once every 30 s, forever: the wedge reproduces
   unattended.
3. **Sampler slots:** `bt4=runningboardd` (launchd's bookkeeping daemon, 13 characters) and
   `bt6=CollectionsPoste` (exactly the sampler's 16-character strncmp width). `bt5=AppStore`
   stays out — the spawn trace names every spawn path anyway.
4. **Budget surgery** for the four new arguments: out went `sd-exec=1` (the execve-only trace is
   blind to posix_spawn, the round-16 lesson), `sd-syscall=1`, `sd-clock-div=0` and `sd-paint=0`
   (the last two proved equal to their defaults in R17); in came `opi5plus-trace-spawn=1`,
   `opi5-hid-input=2` (the injector back), `bt4` and `bt6`.
5. **disabled.plist was evaluated but not implemented** (R20, conditionally feasible — see
   "Next questions"): the round's code change list does not include it, pending the user's
   decision.

### Changes

| file | change |
| --- | --- |
| `patches/xnu/0006-block-exec.patch` | `opi5plus_trace_spawn_entry` / `opi5plus_trace_spawn_ret` at the posix_spawn top and its single `bad:` tail, the 512-print soft cap with per-256 tally, the boot-arg gate and the header paragraph (+97 lines) |
| `package/Opi5plusHid/NativeHidInput.cpp` | `injectTapMode`/`injectTapTick`: after the first completed gesture, one tap at panel (1591,439) every 30,000 ms (down, 80 ms lift, finger id 2), tap counters in the heartbeat line, header note (+55 lines) |
| `board.toml` | `opi5plus-trace-spawn=1` added with the diagnostics-group comment; `sd-exec`, `sd-syscall`, `sd-clock-div=0`, `sd-paint=0` commented out; `opi5-hid-input=2` restored (with the tap binding noted); `bt4=runningboardd`, `bt6=CollectionsPoste` added, `bt5=AppStore` left out; header count updated |
| `tools/wsl/pack-payload.sh` | header byte count 52/940 -> **52/988** |

## R0 — build identity

| item | value |
| --- | --- |
| kernel sha256 | `cf419cb101a2840b66f42f70de3bdc3c9b1657f5c57aa15d994a934d1e707ff5` (51,752,960 B; `logs/build-round17.log` last line, re-checked in the build tree this round) |
| payload | `artifacts/m5/payload-round17.img`, 96 MiB (100,663,296 B), sha256 `4111da9ead9e38388a47c5f200ea8ff15b041c987ced46563ebab69508fe6826` (re-hashed this round) |
| command line | **52 tokens / 988 B** — counted from the launchd echo (:2410): the four round-20 ins and outs net to +48 B over round 16's 940 |

(The round's `board.toml` header comment said "51 arguments"; the echo line counts 52 — the
comment was corrected to match the echo before commit.)

## S0 — the execution chain

Detached boot and capture as in the round-16 shape; the board ran XNU unattended, the closing
task's budget was spent as planned (SSH reboot for the push ×1, closing 0x1d ×1) and the board is
back in Linux. The capture opens with `iBoot didn't extract panic log from previous session
crash` (:1606) — the expected marker after a 0x1d panic reset, recorded so it is not read as a
new fault.

## S1 — the spawn ledger

| item | reading | verdict |
| --- | --- | --- |
| printed entries | **723**: 512 ordinary entries (the soft cap, exact) + 211 refused single-line entries | ✓ |
| refused | all **211 are bluetoothd**, one line `spawn pid=N ret=1 path=/usr/sbin/bluetoothd (block-exec)` (:11928 last) | ✓ — the block-exec echo again, unchanged |
| suppressed | one tally line `spawn trace capped at 512, 256 more suppressed` (:9389) → **256 suppressed**, 768 ordinary spawn events in the boot | ✓ the cap worked as designed |
| unique ordinary paths | **136** (137 with bluetoothd) | ✓ |
| first entries | `spawn pid=1 path=/sbin/fsck` (:2411, merged into a launchd line — a bare-CR merge), `spawn pid=1 path=/System/Library/CoreServices/SpringBoard.app/SpringBoard` (:2656), `spawn pid=57 path=/usr/libexec/backboardd` (:2794) | ✓ |
| exit ring | **443 exit lines, 36 distinct names**: `xpcproxy` code-78 **×211** (1:1 with the 211 refusals — the round-16 verdict holds a third time), `fairplayd.A2` **×190** (a new heavy churner: first exit age 222 rv 0 (:3396), then a spawn→exit age-3 rv-0 loop), datami ×6, CloudKeychainPro ×3, the rest single-digit | ✓ recorded as measured |

(The round ledger circulated "exit 246"; the capture's own counts are the ones above — recorded
from the log, not from the ledger.)

## S2 — the tap closure did not establish: the gesture raced the desktop, and lost

The injector's design gate is "wait behind the driver" — it fires only when an
`IOHIDEventDriver` has the device open. This boot the gate passed **immediately and wrongly**:

| t | event | evidence |
| --- | --- | --- |
| t+0..3 | the kernel-side `IOHIDEventDriver` takes the open gate (`driver appeared ... open 1`) | :1899, before the first beat (:1979 = beat 5 s) |
| ~t+3 | the unlock gesture runs and completes against nothing that draws (`gesture 1: down/lift ... reports 23, fails 0`), then tapping starts | :1913, :1928, :1929 |
| t+36 | first heartbeat: `gestures 1 ... taps 1` | :2023 |
| t+595-600 | **backboardd spawns** (`spawn pid=57 path=/usr/libexec/backboardd` :2794 sits between beat t+595, `procs 3` (:2615), and beat t+600, `procs 125` (:2858)) — **~9.9 min after the gesture** | ✓ |
| t+36 → t+2683 | **90 taps** at panel (1591,439), one per 30 s (`tap 90` :11881) — all of them on the **lock screen**, because the unlock gesture had already been spent against the not-yet-present consumer | ✓ |
| whole boot | **zero AppStore spawns in the ledger** (0 matches for the path) | ✓ |

So the round's central question ("does tapping the icon spawn the app, and what does it do")
closed **mechanically** but not for the board: the injector's own open gate — the one the
gestures invented to avoid firing before anything listens — was consumed at t+0 by the
kernel-side digitizer driver, not by backboardd's client. The fix is small and recorded for the
next round: hang the unlock gesture **after backboardd** (the NativeHidInput comment expected
backboardd "around t+720"; it spawned at t+595-600, and the gate needs to name that, not "any
driver").

## S3 — swap 0 → 2 → 3, and the freeze read frame by frame

| t | swap counter | evidence |
| --- | --- | --- |
| t+30..1500 | 0 | :2016 onward |
| by t+1535 | **2** | :6050 |
| by t+1900 | **3** | :7868 |
| t+1900 → t+2700 | frozen at 3 — **≥800 s** on the display-beat record (the 3rd swap completed between the t+1895 and t+1900 beats; the ledger's "~780 s" anchors at t+1920) | :11922 (t+2700, still 3) |

Three swaps in 2,700 s, 0 copies, `gop 0/3`, and the taps had no visible compositor effect — the
screen stayed on the untouched lock screen's shape.

The freeze window (the t+2700 sampler block, slide **0x1594c000** on every sampler header in the
capture — 204 headers: SpringBoard 71, backboardd 62, runningboardd 71; WidgetRenderer_D and
CollectionsPoste never spawned this boot, so their slots stayed silent; `.cache/r20-sym17/report.md`,
38 threads / 116 unique addresses through `dsc.py`):

* **SpringBoard main thread tid 0x12a, st 0x9 = TH_WAIT|TH_UNINT (uninterruptible)** (:11993,
  chain :11994): the full 14-frame chain reads `UIKitCore _UIAssetManager imageNamed:` →
  `CUICatalog namedVectorGlyph...` → `CUIStructuredThemeStore lookupAssetForKey/renditionWithKey`
  → `_BOMTreeGetValueSize` → `_BOMStreamReadUInt16+0xb0` — **the main thread is synchronously
  parsing a vector-glyph rendition out of a .car asset and stops inside the BOM tree read** of the
  mmap'd catalog data [confirmed chain and state].
* **SpringBoard tid 0x19e7, st 0x9, the same event word 0xfffffff61a66f698** (:12009-:12010): the
  IconFoundation icon-bitmap decode path (`IFImage createCGImageWithBitmapData...` →
  IconServices → `SBHGetApplicationIconImage`). Two independent paths parked on **one event**
  [confirmed]; the reading "the VM page-in of the same mapped asset file" is the round's
  **[conjecture — high-confidence]**: a shared lock would have to be locked by two unrelated
  call paths and be uninterruptible in both, which a page event explains for free.
* SpringBoard's secondary threads are the known XPC/sync furniture, all **interruptible**
  (st 0x41): Telephony `TUCallCapabilitiesXPCClient` sync reply wait, KeyboardArbiter
  `_createSceneWithIdentifier` dispatch_sync, notification persistence `_readPersistentStateFromFile`
  dispatch_sync [confirmed chains].
* **backboardd and runningboardd: every thread healthy** — runloop mach_msg waits, condvar
  waits, sleep timers, workq idles; the only "busy" thread is backboardd 0x621 in a
  dispatch_group_wait [confirmed]. **No wedge on the render side this time.**

### What R20's local symbolization added about the older rounds (`.cache/r20-dsc/`)

* r14 SpringBoard tid **0x109b** = `DoNotDisturb -[DNDRemoteServiceConnection queryState...]+0x84`
  — a synchronous XPC wait, not a mystery PC; tid **0x1132** = the same KeyboardArbiter
  `_createSceneWithIdentifier:...+0x108` dispatch_sync seen again in round 17.
* CollectionsPoste's r14/r15 stop PCs = `___ulock_wait+0x8` / `__dlock_wait+0x34` — the SIGKILL'd
  extensions die **inside the same wedge-layer waits** as the wedged threads (R19's placement,
  now named).
* WidgetRenderer_D r15 tid **0x184e** = `UIKitCore _loadInitializationContext` inside
  dispatch_once; tid **0x1852** = `UIKitServices applicationInitializationContext...` synchronous
  XPC — app-initialization sync waits, the same family.
* The r15 "wild jump" PCs 0x1b6fe4844 / 0x1b68af448 are **real call edges, not garbage**:
  `MediaToolbox FigPictureTileCursorServiceGetClassID+0x0` and
  `VideoToolbox VTTileDecompressionSessionCreate+0x0`; the L1 instruction-translation fault is
  read as an on-demand paging miss plus the kill-moment artifact [conjecture], not a corrupted
  jump.
* A correction to the round-14 reading: backboardd's "wedged" tid was
  `QuartzCore CA::WindowServer::IOMFBServer thread_body+0x1ac` — **a runloop thread idling
  normally**, not a parked render thread [confirmed by the full chain].

## S4 — the three-round wedge unifies

The freezes now align on one shape [conjecture → high-confidence, the round's central result]:
**SpringBoard parses icon/asset data (CoreUI .car through the BOM tree) → the mmap'd page is not
resident → the page-in rides this port's storage path → the wait is TH_UNINT, so nothing
services it and the main thread hangs.** Round 15's icon hover, round 14's storm and this round's
post-tap freeze are all the same site, and this round adds the named 14-frame chain and the
two-threads-one-event observation that make the page-in reading the parsimonious one. The
user-visible "app opens and instantly quits" of round 14 was never the app dying — R19 showed
AppStore[540]/Preferences[530]/MobileSlideShow[544] spawned successfully, stayed alive with zero
Metal and zero IOMFB clients and never exited; the white flash is the composition layer freezing
while the app waits behind the same parse. What remains to *prove* it: a kernel-side stack
capture at the uninterruptible wait (the `ev` channel), which names the page-in site directly.

## S5 — parity

| counter | reading | verdict |
| --- | --- | --- |
| beats | 5 s to 2700 s, `failures 0` in every beat, `stage 32 (L12 late done)` | ✓ |
| final beat | t+2700: `requests 212311, failures 0, procs 171, vfree 208022, vcmode 8, lsr 0` (:11934) | ✓ |
| fence | all 43 beat fence lines `0 of N swaps ... 0 flips waited, 0 gave up`, including the frozen window | ✓ (`mali-sched-wait=2` holds) |
| real panic | 0 (the `panic` matches are the iBoot previous-session marker and the `DumpPanic` daemon's name) | ✓ |
| spawn/exit churn | the ledger above; `fairplayd.A2`'s 190-exit loop is new and noted for the next round | — |

## S6 — wrap-up

The capture ran to its end (last beat t+2700); the board was then recovered with the round's
second and last budgeted interaction (0x1d, reset #1 of the round) and is back in Linux; the
push used the round's one SSH reboot. Budget: SSH ×1, 0x1d ×1, both spent.

## Next questions

1. **Prove the page-in site:** capture the kernel-side stack at the uninterruptible wait (the
   `ev` channel of the sampler) — it names the page-fault handler and the backing-file path
   directly. Small patch 0004/0002 addition, next round's first move.
2. **disabled.plist (user decision pending):** R20 evaluates it as **conditionally feasible** —
   `label = com.apple.bluetoothd` (the OS volume's plist is proven `RunAtLoad`+`KeepAlive`), the
   mechanism is locked to our pinned launchd (unicorn emulation + byte-identical), the timing
   works (0002's seed runs before launchd), upstream precedent exists (sirius/asteroids), and the
   minimal form is a seed addition (dir `/private/var/db/com.apple.xpc.launchd` +
   `disabled.migrated` empty file + `disabled.plist` XML `com.apple.bluetoothd → true`, 0644
   root:wheel), after which `opi5plus-block-exec=bluetoothd` (−31 B) can go. This round
   implemented nothing of it.
3. **The gesture timing fix:** hang the unlock gesture after backboardd (name backboardd, not
   "any driver", in the open-gate check) — otherwise every unattended tap round repeats this
   round's lock-screen dead end.
4. **(carry)** the white-block region sampling and the `fairplayd.A2` 190-exit loop.

## Verbatim log excerpts

Line numbers are the LF-only caliber (see the header note).

* The launchd command-line echo (:2410; 52 tokens / 988 B, middle elided):

```
com.apple.xpc.launchd|1969-12-31 16:10:08.886611 <Notice>: boot-args = -v serial=1 debug=0x10014e ... sd-ps=300 sd-exit=1 sd-watchdog=15 ... opi5-hid=1 opi5-hid-input=2 opi5-usb=5 mali=8 mali-metal=1 mali-sched-wait=2 opi5plus-bt=SpringBoard opi5plus-bt2=backboardd opi5plus-bt3=WidgetRenderer_D opi5plus-bt4=runningboardd opi5plus-bt6=CollectionsPoste opi5plus-bt-secs=30 opi5plus-block-exec=bluetoothd opi5plus-trace-spawn=1 panic_on_crash_disabled=1
```

* The spawn ledger's shape (:2411 bare-CR merge, :2656, :9389, :11928):

```
com.apple.xpc.launchd|1969-12-31 16:10:09.030707 (system) <Notice>: entering ondskip attaching fsw to lo0 using legacy TX modelopi5plus: spawn pid=1 path=/sbin/fsck
opi5plus: spawn pid=1 path=/System/Library/CoreServices/SpringBoard.app/SpringBoard
opi5plus: spawn trace capped at 512, 256 more suppressed
opi5plus: spawn pid=612 ret=1 path=/usr/sbin/bluetoothd (block-exec)
```

* The tap timeline (:1899, :1928-:1929, :2023 merged form, :11881):

```
opi5: hid: inject: driver appeared 0xfffffff3677b2bc0 open 1
opi5: hid: inject: gesture 1: lift at 16383,6553, reports 23, fails 0, last result 0
opi5: hid: inject: gesture 1 complete, entering app-launch tapping: one tap at panel (1591,439) every 30000 ms
opi5: hid: inject: t+36 s: gestures 1 reports 23 fails 0 taps 1 open 1 driver 0xfffffff3677b2bc0
opi5: hid: inject: tap 90: down/lift done at 20372,9996 (panel 1591,439), lift 0
```

* backboardd's late spawn between the procs jump (:2615, :2794, :2858):

```
opi5plus beat 595 s, requests 17618, blocks 6853632, failures 0, procs 3, stage 32 (L12 late done), ...
opi5plus: spawn pid=57 path=/usr/libexec/backboardd
opi5plus beat 600 s, requests 18924, blocks 6909440, failures 0, procs 125, stage 32 (L12 late done), ...
```

* The freeze window's central pair — the main thread's status+chain (:11993-:11994) and the icon
  decoder (:12010), both st 0x9, both `ev 0xfffffff61a66f698`:

```
opi5: ubt SpringBoard tid 0x12a st 0x9 pri 47/47 cpu 3 wait -1 ev 0xfffffff61a66f698 cont 0 bh 0x1 wq 0xfffffff534d04dd0 | user pc 0x1b8ab1d6c lr 0x1b8ab198c sp 0x16f00cda0 fp 0x16f00cdc0 x0 0x30105f7e0 x16 0x4b
opi5: ubt tid 0x12a pc 0x1b8ab1d6c lr 0x1b8ab198c sp 0x16f00cda0 fp 0x16f00cdc0 | up 0x1b8ab198c 0x1b8aa589c 0x1b8aa5734 0x1b8aa5ccc 0x1b8aaf468 0x1b8aa7a0c 0x1b8aa5f28 0x1b8ab5310 0x1b8ab51f0 0x1b8ab8ddc 0x1b8ab3e88 0x1b8aadb10 0x19f9567f0 0x19f956730 | fps 0x16f00cdc0 0x16f00ce20 0x16f00ce60 0x16f00cea0 0x16f00cef0 0x16f00cf30 0x16f00cf70 0x16f00d110 0x16f00d190 0x16f00d200 0x16f00d2a0 0x16f00d300 0x16f00d5b0 0x16f00d690
opi5: ubt tid 0x19e7 pc 0x1e8839b00 lr 0x1e8839af8 sp 0x16f3b9330 fp 0x16f3b9380 | up 0x1e8839a1c 0x1e88398c0 0x1c2bc4204 0x1c2bc51cc 0x1c2bc335c 0x1c2bc313c 0x1e4d969c8 0x1e54175c0 0x1e4d94ab0 0x1e4d94820 0x1e4d94694 0x1a4f01c9c 0x1a4f02cc0 0x1a4ede8f8 | fps 0x16f3b9380 0x16f3b9400 0x16f3b9440 0x16f3b9500 0x16f3b95c0 0x16f3b9630 0x16f3b9690 0x16f3b9720 0x16f3b9760 0x16f3b97c0 0x16f3b9800 0x16f3b9890 0x16f3b98b0 0x16f3b98d0
```

(The chain's symbols, from `.cache/r20-sym17/report.md`: f0 `CoreUI _BOMStreamReadUInt16+0xb0` up
through `CUICatalog namedVectorGlyph...` to `UIKitCore _UIAssetManager imageNamed:`, and
`IconFoundation IFImage createCGImageWithBitmapData...` up through IconServices to
`SpringBoardHome SBHGetApplicationIconImage`.)

* The swap freeze's three anchors (:6050, :7868, :11922) and the closing fence line (:11926):

```
opi5: display t+1535s: swap ok 2 rej 0 (reason 0) copy 2 (status 0, 14745600 B, 0 copies) retired 2 gen 2/2 frames 0 gop 0/2 timeouts 0 prepare-fail 0, ...
opi5: display t+1900s: swap ok 3 rej 0 (reason 0) copy 3 (status 0, 14745600 B, 0 copies) retired 3 gen 3/3 frames 0 gop 0/3 timeouts 0 prepare-fail 0, ...
opi5: display t+2700s: swap ok 3 rej 0 (reason 0) copy 3 (status 0, 14745600 B, 0 copies) retired 3 gen 3/3 frames 0 gop 0/3 timeouts 0 prepare-fail 0, ...
opi5: display t+2700s: fence: 0 of 3 swaps arrived with GPU work not done; 0 flips waited (mean 0 us, longest 0 us, 0 gave up)
```

* The main thread's freeze-window neighbour — the icon decoder's status line (:12009):

```
opi5: ubt SpringBoard tid 0x19e7 st 0x9 pri 46/37 cpu 0 wait -1 ev 0xfffffff61a66f698 cont 0 bh 0x1 wq 0xfffffff534d04dd0 | user pc 0x1e8839b00 lr 0x1e8839af8 sp 0x16f3b9330 fp 0x16f3b9380 x0 0x10161d000 x16 0x1a4efcc7c
```

* The new fairplayd churn (:3396-:3397):

```
opi5plus: exit fairplayd.A2[80] rv 0x0 sig 0 code 0 age 222
opi5plus: exit fairplayd.A2[116] rv 0x0 sig 0 code 0 age 3
```
