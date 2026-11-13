# Round 18 result — the wedge is SystemOS demand-paging starvation on the SD window lock

Date: 2026-11-13 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One unattended XNU boot (`logs/boot-round18.log`, project working tree, **not committed**), merged
with the round-21 implementation (the mutex-holder reverse lookup, the lock inventory, the launchd
disabled state and the deferred unlock). The holder forensics in S1 are re-derived **directly from
the capture** in this session; the working artifact `logs/r21-holders-freeze.txt` carried unfilled
line-number placeholders and a smaller subset, and the numbers below supersede it. Caliber note
(same as the round-16/17 pages): line numbers are the **LF-only count** (bare-CR fragments merge
into the preceding line); the LF-only total is 10,965 lines, the CR-inclusive
(`File.ReadAllLines`) count is 12,397 — the round ledger's numbers were CR-caliber, and every one
of them was re-anchored to its LF line for this page.

## What the round did, and why

Round 17 left the freeze as a high-confidence reading (an uninterruptible CoreUI asset page-in)
and three unknowns: who held the mutex the waiter sat behind, was it a deadlock or a queue, and
whether the launchd disabled state could retire the block-exec argument. This round's
implementation answered all three in one boot:

1. **The mutex holder becomes nameable (patch 0004).** A thread whose block hint is 1
   (kThreadWaitKernelMutex) is asserted by `lck_mtx_lock_wait()` on the mutex's own state word —
   `thread->wait_event` is `LCK_MTX_EVENT(lck)` = the address of `lck_mtx.data`, so the
   `lck_mtx_t` sits at `wait_event - 8` and the low 28 bits of the word hold the owner's compact
   thread id. The sample now appends `| mtx <addr> state <word> owner <proc> tid <tid> st <st>
   bh <bh> ev <ev> upc <pc> ulr <lr>`, resolving the id through `ctid_get_thread_unsafe()` — a
   plain table read. Strictly read-only: no lock taken, no word written, nothing woken; a stale
   word prints `owner ?`, a zero event prints `mtx ?`.
2. **The SD locks publish themselves (package/NativeSd).** Once per window, the controller and
   each block window print `opi5plus: sd: lock <name> iolock=<p> mtx=<p>` — IOLock is lck_mtx_t
   in this kernel, so the printed pointer is exactly the address the sampler's reverse lookup
   produces, and the holder lines can be matched to a named window.
3. **launchd stops spawning bluetoothd (patch 0002).** `opi5plus_launchd_disabled()` writes
   `/private/var/db/com.apple.xpc.launchd/disabled.migrated` (empty) and `disabled.plist`
   (`com.apple.bluetoothd → true`, both root 0644) **every boot**, gated by `sd-data-launchd`;
   the ensure_dirs table gains the directory. The every-boot placement is forced by the seed's
   own design: with `sd-data-persist=1` the first-boot seed is marker-skipped from the second
   boot on (:1970 `seeded by an earlier boot ... nothing copied`), so anything seed-only would be
   first-boot-only — the round-2 lesson applied. `opi5plus_seed_write()` now takes its
   vfs_context as an argument so the kernel-context writer outside the seed walk can reuse it.
   **`opi5plus-block-exec=bluetoothd` leaves board.toml** — 31 bytes back (52 → 51 arguments,
   988 → 957 B); the 0006 mechanism stays in the kernel, inactive without the argument.
4. **The unlock gesture waits for reality (package/Opi5plusHid, patch 0006).** 0006's spawn top
   now exports `native_spawn_seen_backboardd` (set on basename match whatever the trace switch
   says; `copyinstr` moved above the switch so both the flag and the refused-spawn line share
   it). The injector gestures only after backboardd has spawned **and** the display has accepted
   a swap, then 30 s of quiet; 600 s without both falls back to the old driver/open gate.
5. **Budget surgery** fell out of item 3: no new arguments were needed, and the command line
   shrank to **51 tokens / 957 B**.

### Changes

| file | change |
| --- | --- |
| `patches/xnu/0004-thread-sample.patch` | `opi5plus_mtx_owner()` (+76 lines): the bh==1 reverse lookup above, appended to the waiter's line before its user state |
| `package/NativeSd/NativeSd.cpp` | the per-window lock inventory (gate + each block window; the RAM window's "no lock" line in `configureRam`) |
| `patches/xnu/0002-mount-data-volume.patch` | `opi5plus_launchd_disabled()` (+ the ensure_dirs entry, the `opi5plus_seed_write` context refactor, the every-boot call before the MobileGestalt wipe) |
| `package/Opi5plusHid/NativeHidInput.cpp` | the deferred unlock gate (both conditions + 30 s quiet + 600 s fallback), the first-tap log line (+65 lines) |
| `patches/xnu/0006-block-exec.patch` | `native_spawn_seen_backboardd` export, `copyinstr` moved to the top (+32 lines) |
| `board.toml` | `opi5plus-block-exec=bluetoothd` removed with the history comment; header count 51/957 |
| `tools/wsl/331-patch-xnu-data-mount.py` (root repo) | the generator of the 0002 changes above |

## R0 — build identity

| item | value |
| --- | --- |
| kernel sha256 | `f9e14341de8eb46d3026f805fdb320847e95841d963c2a88fef927205f65373a` (51,752,960 B; `logs/build-round18.log` last line, re-checked in the build tree this round) |
| payload | `artifacts/m5/payload-round18.img`, 96 MiB (100,663,296 B), sha256 `3940e422faaf8c4b283e31a8e39269f37a269565b009a92a9c8707a7d22f593b` (re-hashed this round) |
| command line | **51 tokens / 957 B** — counted from the launchd echo (:2396); `block-exec` absent from the line |

## S0 — the execution chain

Detached boot and capture as in rounds 16/17; the round's budget (SSH ×1 for the push, closing
0x1d ×1) was spent and the board is back in Linux. The capture opens with the by-now expected
`iBoot didn't extract panic log from previous session crash` (:1608) — the 0x1d panic reset's
marker, not a new fault.

## S1 — the lock-holder forensics (the round's main delivery)

The inventory (:1718, :1731, :1734, :1739):

```
gate     iolock=0xfffffff5f21f6070 mtx=0xfffffff5f21f6070
SystemOS iolock=0xfffffff5f21f6f50 mtx=0xfffffff5f21f6f50
OS       iolock=0xfffffff5f21f66c0 mtx=0xfffffff5f21f66c0
Data     iolock=0xfffffff5f21e99f0 mtx=0xfffffff5f21e99f0
```

(The RAM window's own "no lock" line did not appear in this capture; the four medium windows
did. Recorded as observed.)

**The full bh==1 population of the capture, re-derived this session: 25 ubt lines** — 24 with a
mtx segment, one without. Distribution:

| mutex | lines | reading |
| --- | --- | --- |
| **gate** 0xfffffff5f21f6070 | 1 | holder = `xpcproxy tid 0x146 st 0x4` (:2846, t+600 block) — the spawn-side reader, holding the gate briefly |
| **SystemOS** 0xfffffff5f21f6f50 | 23 | the card-read window: **every uninterruptible waiter in the boot is queued here** |
| OS / Data | **0 / 0** | the OS and Data windows never appear under a bh==1 waiter |

**The SystemOS lock rotates through 19 distinct owner processes across the 23 samples** — healthd,
mediaremoted, biomed, routined, timed, remoted, lsd, mobileassetd, backboardd (tid 0x173 as owner,
:3942), audioaccessoryd, studentd, calaccessd ×2, mediaserverd, remindd, financed, identityservices,
chronod ×2, searchd — **every one of them user-mode (owner st 0x4), none holding across a sampling
interval**. There is no stuck holder: this is **queue saturation, not a deadlock** [confirmed].

Two **nested waits** were caught in the act — a holder of the SystemOS lock that is itself
uninterruptibly waiting on the gate:

* :3077 (t+660 block): owner `mediaremoted tid 0x18c st 0x9 bh 0x1 ev 0xfffffff5f21f6078` —
  holds SystemOS, waits on the gate;
* :6081 (t+1800 block): owner `mediaserverd tid 0x288 st 0x9 bh 0x1 ev 0xfffffff5f21f6078` — the
  same shape.

And the round-17 mystery waiter is now inside this table: **SpringBoard's main thread tid 0x12a
appears 8 times** — once on the gate (t+600, while launchd's spawn side still ran) and seven
times on the SystemOS mutex across the frozen window (t+2070 → t+2640, owners remindd / financed /
identityservices / chronod ×2 / searchd / routined) — the asset-parsing thread of round 17 is
waiting, uninterruptibly, in the SD card-read queue. One sample (:9737, t+2490 block) carried the
waiter without a mtx segment — the reverse lookup did not produce an owner there; recorded as
observed, not smoothed over.

## S2 — the unified conclusion upgraded: bandwidth starvation [confirmed]

The freeze window is swap frozen at 3 from t+1810 (:6258) to t+2700 (:10670). Across it the SD
read path ran **continuously at its ceiling**:

| measure | t+1810 (beat :6296) | t+2700 (beat :10693) | delta | rate |
| --- | --- | --- | --- | --- |
| requests | 132,495 | 212,075 | **+79,580** | 89.4/s |
| blocks | 20,868,736 | 31,102,336 | **+10,233,600** | 11,498 blocks/s ≈ **5.61 MiB/s** |
| failures | 0 | 0 | 0 | — |

That rate equals the round-17 baseline (89.2/s, 5.62 MiB/s) and the terminal values at t+2700 sit
within ~0.1% of round 17's (212,311 / 31,135,744) — **the read trajectory reproduces across
boots** [confirmed]. Nothing is stuck; the card is simply saturated: the SystemOS cryptex
(~3.3 GB) is being read through this port's ~11.5 MB/s PIO path to satisfy demand paging, the
window lock is held for whole requests, and the queues behind it (page-ins, including
SpringBoard's asset pages) wait uninterruptibly. The wedge is **demand-paging bandwidth
starvation on the SD window lock**, not a deadlock and not an XPC wait [confirmed, upgraded from
round 17's page-in conjecture by naming the lock and the holders].

**The lever is bandwidth**: the SD line's own standing result — the PIO path runs at ~89% of the
card's ~26 MHz 4-bit clock, and the next 2× (~23 MB/s) is CRU clock-tree work
(`evidence/sd-clock-investigation.md`) — is now the primary fix candidate; a working-set warm-up /
prefetch for the cryptex is the alternate.

## S3 — the disabled.plist won completely [confirmed]

| check | reading |
| --- | --- |
| both files written, every boot | `disabled.migrated -> 0`, `disabled.plist -> 0` (:1965, :1966) |
| bluetoothd spawns | **0** (the whole spawn ledger has no bluetoothd entry) |
| bluetoothd refusals | **0** (block-exec is out of the command line and out of the log) |
| exit-78 storm | **211 → 1** — the single exit-78 is `exit xpcproxy[354] rv 0x4e00 sig 0 code 78 age 1` (:8732, between beats t+2330 and t+2335) |
| boot-args echo | no `block-exec` argument (:2396) |

The one exit-78 has **no named launchd line in this capture** — no `<Error>` line, no
`webbookmarksd` string anywhere [the round-ledger's attribution of this single corpse to the
r14 WebBookmarks missing-executable job is not reproducible from this log and is recorded as
unconfirmed]. What the ledger does name: two spawn returns of **2 (ENOENT-class)** —
`spawn pid=1 ret=2` (:2444, line-order paired with the :2434 entry, `HorizonSetup`) and
`spawn pid=8 ret=2` (:2465, `snapshottool` :2462). The storm's disappearance is exactly the
round-4 trade retired at its source: launchd never spawns the daemon, so there is no refusal and
no corpse.

## S4 — the expected timeline, as it actually ran

| t | event | evidence |
| --- | --- | --- |
| t+1 | unlock deferral starts (`backboardd 0 swap 0`) | :1905 |
| t+600 | **the 600-s fallback fired as designed** (`backboardd 1 swap 0`) — backboardd had spawned but the display had not accepted a swap | :2831 |
| t+600 | the gesture runs (against a lock screen that has not been drawn) | :2837-:2839 |
| t+606 | first tap (`first tap at t+606 s`) | :2995 |
| t+606 → end | 70 taps, `fails 0` in every heartbeat (last heartbeat t+2681 `taps 69`, then `tap 70` :10627) | ✓ |
| by t+1770 | first accepted swaps (`swap ok 2`) | :5873 |
| by t+1810 | third swap; frozen there to t+2700 | :6258, :10670 |

The fallback's own design tension showed up: it released on "600 s without both" — but the
condition it fell back to is the one round 17 proved wrong (a gate the kernel driver satisfies at
t+0), and the gesture then ran at t+600 with `swap 0`. The recorded fix for the next round: the
fallback should also require a swap (or simply keep waiting), not return to the old gate.

## S5 — the tap closure's next layer: the synthetic events had no consumer

* The display registry printed **`IOHIDSystem absent` on every registry line from t+30 to
  t+2700** (90 lines) — the synthetic touch events had **no IOHIDSystem-class consumer** to
  route them [confirmed]. backboardd is alive and healthy in every sample (S1), accepted three
  synthetic swaps at t+1770-1810, and froze with the rest.
* `appstored` appears only in the final process census (`ps 367(appstored)` :10799) and **never
  in the spawn ledger** [conjecture: spawned in the t+2400-2700 window, its spawn line swallowed
  by the 512-entry cap — 768 ordinary spawn events ran, 512 printed]. Its origin stays open.
* CollectionsPoste: 0 spawns (the unlock never completed, bt6 had no target) — same as round 17.

## S6 — parity

| counter | reading | verdict |
| --- | --- | --- |
| beats | 5 s to 2700 s, `failures 0` in every beat | ✓ |
| final beat | t+2700: `requests 212075, failures 0, procs 183, vfree 215525, vcmode 8, lsr 0` (:10693) | ✓ |
| fence | all 34 beat fence lines `0 of N swaps ... 0 flips waited, 0 gave up`, including the frozen window (:10674) | ✓ (`mali-sched-wait=2` holds) |
| real panic | 0 (the only `panic` match is the iBoot previous-session marker) | ✓ |
| spawn ledger | 512 printed entries (the cap, exact) + one tally (`capped at 512, 256 more suppressed` :10230) = **768 ordinary spawn events, 0 refused**, 148 unique paths | ✓ |
| exit ring | 235 lines / 36 names — `fairplayd.A2` ×190 (the round-17 churner persists unchanged), datami ×9, the one xpcproxy-78 of S3 | ✓ recorded |

## S7 — wrap-up

The capture ran to its end (last beat t+2700); the round's budgeted interactions (SSH ×1, 0x1d ×1)
were spent in the round session and the board is back in Linux — no board contact in this closing
session.

## Next questions

1. **The bandwidth lever:** evaluate the CRU clock-tree change (move the sdmmc source clock to
   52 MHz, the standing 2× ≈ 23 MB/s result) as the next board round; the working-set warm-up /
   prefetch for the cryptex is the alternate. This is the first fix candidate that attacks the
   named mechanism instead of a symptom.
2. **`IOHIDSystem absent`:** why the class never publishes on this port (round 3's digitizer
   collection rides `IOHIDEventService`/`IOHIDEventDriver`; `IOHIDSystem` is a macOS name,
   `evidence/m5/M5-08` — but backboardd's own synthetic-event consumption path needs naming
   before the taps can ever close).
3. **The gesture fallback:** make the 600-s fallback also require a swap instead of returning to
   the old driver/open gate (which the kernel driver satisfies at t+0).
4. **`appstored`:** where its spawn went (a per-name reservation in the spawn trace, or a
   second tally tier, would answer it).

## Verbatim log excerpts

Line numbers are the LF-only caliber (see the header note).

* The launchd command-line echo (:2396; 51 tokens / 957 B, middle elided; `block-exec` gone):

```
com.apple.xpc.launchd|1969-12-31 16:10:07.817078 <Notice>: boot-args = -v serial=1 debug=0x10014e ... sd-ps=300 sd-exit=1 sd-watchdog=15 ... opi5-hid=1 opi5-hid-input=2 opi5-usb=5 mali=8 mali-metal=1 mali-sched-wait=2 opi5plus-bt=SpringBoard opi5plus-bt2=backboardd opi5plus-bt3=WidgetRenderer_D opi5plus-bt4=runningboardd opi5plus-bt6=CollectionsPoste opi5plus-bt-secs=30 opi5plus-trace-spawn=1 panic_on_crash_disabled=1
```

* The lock inventory (:1718, :1731, :1734, :1739):

```
opi5plus: sd: lock gate iolock=0xfffffff5f21f6070 mtx=0xfffffff5f21f6070
opi5plus: sd: lock SystemOS iolock=0xfffffff5f21f6f50 mtx=0xfffffff5f21f6f50
opi5plus: sd: lock OS iolock=0xfffffff5f21f66c0 mtx=0xfffffff5f21f66c0
opi5plus: sd: lock Data iolock=0xfffffff5f21e99f0 mtx=0xfffffff5f21e99f0
```

* The disabled state written before launchd (:1965-:1966) and the seed marker it has to bypass
  (:1970):

```
opi5plus: launchd: /private/var/db/com.apple.xpc.launchd/disabled.migrated -> 0
opi5plus: launchd: /private/var/db/com.apple.xpc.launchd/disabled.plist -> 0
opi5plus: seed: persistent Data volume, seeded by an earlier boot (/private/var/.opi5plus-seeded exists): nothing copied
```

* The gate's spawn-side holder (:2846, t+600 block):

```
opi5: ubt SpringBoard tid 0x12a st 0x9 pri 47/47 cpu 0 wait -1 ev 0xfffffff5f21f6078 cont 0 bh 0x1 wq 0xfffffff5f21426d0 | mtx 0xfffffff5f21f6070 state 0x400dccb9 owner xpcproxy tid 0x146 st 0x4 bh 0 ev 0 upc 0x1dc370b44 ulr 0x1dc370b10 | user pc ...
```

* A rotating SystemOS holder with SpringBoard queued behind it (:7482, t+2070 block):

```
opi5: ubt SpringBoard tid 0x12a st 0x9 pri 47/47 cpu 2 wait -1 ev 0xfffffff5f21f6f58 cont 0 bh 0x1 wq 0xfffffff5f21426d0 | mtx 0xfffffff5f21f6f50 state 0x400dcc61 owner remindd tid 0x19f st 0x4 bh 0 ev 0 upc 0x1ba027df0 ulr 0x1ba027c14 | user pc 0x1ba027df0 ...
```

* The nested wait — mediaserverd holds SystemOS and waits on the gate (:6081, t+1800 block):

```
opi5: ubt backboardd tid 0x173 st 0x9 pri 46/37 cpu 3 wait -1 ev 0xfffffff5f21f6f58 cont 0 bh 0x1 wq 0xfffffff5f21426d0 | mtx 0xfffffff5f21f6f50 state 0x400dccfd owner mediaserverd tid 0x288 st 0x9 bh 0x1 ev 0xfffffff5f21f6078 upc ...
```

* The deferred-unlock gate working as designed (:1905, :2831, :2837, :2995):

```
opi5: hid: inject: unlock deferred (waiting backboardd/swap): backboardd 0 swap 0 at t+1 s
opi5: hid: inject: unlock deferred: fallback after 600 s without backboardd/swap (backboardd 1 swap 0), the driver/open gate applies
opi5: hid: inject: gesture 1: down at 16383,26213
opi5: hid: inject: first tap at t+606 s (panel 1591,439)
```

* The read ceiling through the frozen window (beats :6296 and :10693, elided):

```
opi5plus beat 1810 s, requests 132495, blocks 20868736, failures 0, procs 116, ...
opi5plus beat 2700 s, requests 212075, blocks 31102336, failures 0, procs 183, ...
```

* The freeze's three swap anchors (:5873, :6258, :10670) and the closing fence line (:10674):

```
opi5: display t+1770s: swap ok 2 rej 0 (reason 0) copy 2 (status 0, 14745600 B, 0 copies) retired 2 gen 2/2 frames 0 gop 0/2 timeouts 0 prepare-fail 0, ...
opi5: display t+1810s: swap ok 3 rej 0 (reason 0) copy 3 (status 0, 14745600 B, 0 copies) retired 3 gen 3/3 frames 0 gop 0/3 timeouts 0 prepare-fail 0, ...
opi5: display t+2700s: swap ok 3 rej 0 (reason 0) copy 3 (status 0, 14745600 B, 0 copies) retired 3 gen 3/3 frames 0 gop 0/3 timeouts 0 prepare-fail 0, ...
opi5: display t+2700s: fence: 0 of 3 swaps arrived with GPU work not done; 0 flips waited (mean 0 us, longest 0 us, 0 gave up)
```

* The one exit-78 of the boot, with its anchor beats (:8729-:8734):

```
opi5plus beat 2330 s, requests 179022, blocks 26856960, failures 0, procs 162, ...
opi5plus: exit xpcproxy[354] rv 0x4e00 sig 0 code 78 age 1opi5: usb: poll frames=8865 reports=0 errors=0 hid_total=0 ...
opi5plus beat 2335 s, requests 179480, blocks 26914944, failures 0, procs 162, ...
```
