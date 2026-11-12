# Round 15 result — `mali-sched-wait=2` closes the fence gave-up half; the white blocks and the wedge are a second mechanism

Date: 2026-11-13 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One XNU boot this round (the fence-fix payload), captured detached for the full `--tail-secs 2700`
window; the board was left on XNU overnight by the user's instruction. The capture
(`logs/boot-round15.log`, project working tree, **not committed** — `logs/**` is gitignored) is the
round's instrument. Caliber note (same convention as the round-11/12/13/14 pages): every line
number on this page is the **CR-inclusive count, the Select-String caliber**; the final tail values
were taken while the capture was still running, at CR-inclusive 15903 (t+2205).

## What the round did, and why

Round 14 ended with two open defects behind the exit-78 storm: 2,503 of 2,551 swaps "arrived with
GPU work not done" (1,805 flips waited, longest 250,346 us, **2 gave up**, swap counter frozen) and
a UI-thread wedge that froze the panel under real interaction. The upstream board
(`UniXNU-orangepi5 @729ba25f`, `board.toml:155`) ships `mali-sched-wait=2` as the fix: a command
buffer working on an IOSurface reports "scheduled" only after the GPU has finished it, in every
process including the render server (user-space change only — `metal-plugin/NativeMaliMetalPlugin.mm`
reads the boot arg; `NativeBridgeMetalFrontend.mm` does the wait; `package/Mali` does not parse it,
so the kernel does not change). This round verified that fix on this board.

1. **Payload** (`artifacts/m5/payload-round15.img`, 96 MiB, sha256
   `a9c363c613f56c26df68b146087785fab66548ce9cd1ddd7543e06c8959ce0bc`) packs the **same kernel** as
   round 14 (`8463916c3426b570af4edfaa17105d1e06fb79ce001b998c045095c6dce048ea`, 51,736,576 B); the
   only content difference is the FAT command line, **54 arguments / 990 B of the 1000-byte limit**
   (round 14: 972 B / 53), the one addition being `mali-sched-wait=2 `. The launchd echo
   (CR-inclusive 2447) carries it: `... mali=8 mali-metal=1 mali-sched-wait=2 opi5plus-bt=SpringBoard ...`,
   with census/mirror/usb groups unchanged (`opi5-gop-mirror=0 opi5-surface-census=1 ... opi5-usb=5`).
2. **Nothing else changed.** One SSH software reboot (built into `tools/boot-uefi.py`); the 0x1d
   hard-reset quota stayed unused. t0 = 02:58:36 +08:00; the capture ran to t+2700 (ended 03:45:30).

### Changes

| file | change |
| --- | --- |
| `board.toml` | `mali-sched-wait=2` appended to `kernel_args` with the parser-location comment (:383-391); the header comment updated to "since round 16 — 54 arguments, 990 bytes" (:211) |
| `tools/wsl/pack-payload.sh` | header-comment byte count only (2 lines) |

## S1 — the fence layer is fixed [confirmed]

The fence population changed shape exactly as the upstream evidence predicts:

| metric | round 14 (before) | round 15 (after) | upstream fix evidence |
| --- | --- | --- | --- |
| swaps arrived with GPU work not done | 2,503 of 2,551 (~98%) | **0 of N, in all 66 fence lines** | d29 without it: 1,045/1,060 |
| flips waited | 1,805 | **0** | — |
| fence longest | 250,346 us | **0 us** | 14,078 us (d33/d34: 21/810) |
| gave up | 2 | **0** | 0 |

All 66 fence lines through t+2205 read `0 of N swaps arrived with GPU work not done; 0 flips waited
(mean 0 us, longest 0 us, 0 gave up)` — zero non-zero lines anywhere in the capture, including the
peak. The first fence line already carries the fix (t+730: `0 of 3 swaps`), where round 13's first
line at the same phase already read `1 of 2` and then degraded to its frozen 170/171.

## S2 — the interaction storm, and the wedge that ends it [confirmed]

The user's interaction (03:27 onward) triggered the same swap storm as round 14's:

* swaps: 6 (t+1610) -> 38 -> 119 -> 123 (t+1655-1670) -> 241 -> **393 -> 394** (t+1685-1690);
  a new `swaps handed over (the render server's rate)` line appeared with it: busiest second 60,
  mean gap 28,937 us, longest gap 729,178 us.
* through the whole storm the fence line stayed `0 of 372 swaps ... 0 gave up` — the round-14
  shape (storm -> gave-up -> swap freeze **with** fence backlog) did not reproduce.
* the storm's own terminal state differs from round 14: 394 swaps, `gen 394/394 retired 394/394`
  (every swap retired), 0 copies, `gop 0/394` (letterbox refuses, the direct path presents),
  `timeouts 0 prepare-fail 0`, latch mean 10,136 us / longest 17,050 us / 0 over 50 ms.
* **then the panel froze anyway** (user hovering the App Store icon at about t+1690): the swap
  counter froze at 394 from t+1690 through t+2205 (last read), the beat line's `requests` crawled
  to a stop (270,421 at t+1805 -> 270,542 at t+2205), the census and CollectionsPoste streams both
  went silent after t+1680, while the kernel kept beating every 5 s (watchdog fed, `procs 224`,
  `failures 0`, no real panic — the 16 `panic` matches in the log are the boot-arg echo and
  advisory strings) and the USB poll loop kept running clean.

So this round's verdict splits the freeze: **the fence gave-up path is closed** (fence 0% / gave-up
0 through the storm and after), and **a second wedge mechanism exists that reproduces without it** —
the screen freezes with the swap counter frozen and no fence backlog. Round 14's first suspect
(the mach_msg2 wedge thread, PCs `0x1daf40b58` / `0x1daf46bdc`) stands unchanged. The user's
overnight instruction leaves the board in exactly this state (kernel alive, panel frozen, harmless).

## S3 — CollectionsPoste: the SIGKILL loop is thinner, not gone [confirmed]

5 SIGKILLs total, concentrated in the storm's first 90 s (t+1590-1680): pids 503, 527, 534, 535,
538, ages 6-8 s — about one per 18 s in that window, against round 13's 5 in 60 s (12 s cycle) and
round 14's 7-12 s loop. Round 13's baseline first exit was at t+~1500; round 15's first came ~90 s
later and the spacing is ~1.5x wider. After t+1680 there are **no more exits through t+2205**
(525 s quiet) — but that silence coincides exactly with the wedge, so it is not evidence of a cure:
the whole render/compositor side stopped, the loop included. Crash signatures: four at
`esr 0x56000080` (pc `0x1ea48abdc` / `0x1ea484b58`, the same svc-plumbing site family), two at
`esr 0x82000007` inside CollectionsPoste's own text (pc `0x1b6fe4844`, `0x1b68af448` — new sites
this round). Attribution of the loop (and whether it shares the wedge mechanism) is open.

## S4 — white blocks: still there, and the census wording is corrected [confirmed by the user]

The user was at the panel: **the white blocks are still there.** The kernel-side readings of this
round are therefore recorded with a corrected interpretation:

* the census went `near-uniform n=2: 0xff000000*24 + 0xffffffff*1` (early, 15 lines) to
  **`mixed n=16/20/21`** on all three composited surfaces (pid 57 / 85 / 233, 2560x1440 BGRA) once
  the desktop was up — but `mixed` says the sampled 5x5 grid is heterogeneous, **not** that the
  white blocks are gone: the 5x5 full-frame grid cannot sample the block's region at all, and
  round 14's `near-uniform fff2f2f7*24` described a whole frame dominated by background, not the
  block's pixels. **A white-block verdict needs region-level sampling at the user's block
  coordinates — next round.** `fff2f2f7` appears 0 times in this capture, which again says nothing
  about the block's interior.
* the 30 s readback line read black (`px ff000000`) at its sample points for the whole run, across
  rotating windows (`f1000000` / `f2000000` / `f3000000`) — the sample point stays dark; same
  caliber limit as above.

## S5 — parity, USB, memory

* **USB coexists** [confirmed]: Razer receiver still in, all 221 poll lines `errors=0`, the real
  mouse drove the freeze-adjacent interaction: `mouse_seq 511 -> 10,524` (`hid_total 10,530`,
  `kbd_seq 6`), wheel 0. The poll loop stayed alive through the wedge.
* **parity at the last read** (t+2205): `requests 270,542, blocks 38,418,048, failures 0,
  procs 224, stage 32 (L12 late done)`, no real panic, 0 copies on every swap, `gop 0/394`,
  copy fallbacks 0, back to boot 0, `inject fails 0` (injector: last gesture lift `fails 0`).
* **vfree** fell 138,474 (t+1555) -> 3,201 (t+1820) -> 1,901 (t+2205) — the same designed floor the
  round-11 handoff records (~1,576 pages), reached under interaction, not exhausted; `lsr 0-20`
  oscillating as before.
* mali relay: 102 degrade/REFUSES lines (round 13 full-run: 87), all informational
  `metal2vulkan-degrade-*` notes from backboardd.
* **exit 78 storm unchanged** [confirmed]: 191 `exit xpcproxy ... code 78` lines — the known,
  independent cryptex/missing-binary layer; recorded as-is, not judged a regression.

## What the round retires, and what it adds

* **Retired:** the "fence gave-up -> swap freeze -> white blocks" chain as *the* freeze mechanism —
  its first half (gave-up) is verified absent here while the freeze still happens.
* **Confirmed:** the fence layer fix works on this board exactly as upstream's numbers predict
  (0% / 0 us / 0 gave-up through an interaction storm).
* **Open (next questions):**
  1. **Region-level census** for the white blocks (sample the user's block coordinates) — upgrades
     the white-block verdict from conjecture either way.
  2. **Symbolise the wedge PC** (`0x1daf40b58` / `0x1daf46bdc` from round 14; this round adds
     CollectionsPoste's own-text sites `0x1b6fe4844` / `0x1b68af448`) against the dyld shared cache
     (`dsc.py --slide`; dmg on hand, else patch 0004 with module+offset).
  3. **CollectionsPoste loop attribution** — same family as the wedge or independent?
  4. **exit-78 cryptex bottom-out** (the missing binaries' patch).

## The human-eye items

* White blocks: **still there** (user, confirmed).
* Freeze: reproduced at the App Store hover with no fence gave-up (user, confirmed).
* Collections/Photos-class apps: could not be meaningfully exercised this round — the wedge arrived
  with the first app hover; the launch layer (exit 78) still kills opened apps instantly.
* Mouse feel: real and continuous until the freeze (mouse_seq 10,524); the user's feel report
  beyond that is theirs to give.

## Verbatim log excerpts

The capture lives only in the project working tree. Line numbers are the CR-inclusive
(Select-String) count.

* The launchd command-line echo (CR-inclusive 2447, middle elided):

```
com.apple.xpc.launchd|1969-12-31 16:05:35.097403 <Notice>: boot-args = -v serial=1 debug=0x10014e ... opi5-gop-mirror=0 opi5-surface-census=1 sheng-aks=1 offline-key-events=1 opi5-hid=1 opi5-hid-input=2 opi5-usb=5 mali=8 mali-metal=1 mali-sched-wait=2 opi5plus-bt=SpringBoard ... panic_on_crash_disabled=1
```

* The fence line through the storm peak (CR-inclusive 11346, t+1690):

```
opi5: display t+1690s: fence: 0 of 372 swaps arrived with GPU work not done; 0 flips waited (mean 0 us, longest 0 us, 0 gave up)
```

* The storm's terminal swap / vop / fence triple (CR-inclusive 11344, t+1690; the `registry`
  line is a bare-CR fragment interleaved after `drain`):

```
opi5: display t+1690s: swap ok 394 rej 0 (reason 0) copy 394 (status 0, 14745600 B, 0 copies) retired 394 gen 394/394 frames 0 gop 0/394 timeouts 0 prepare-fail 0, sd pio bursts 71772532, chunk us 1947487363, chunks 284821, fill us 1412185304, drain us 16opi5: display t+1690s: vop direct (iommu 1, status 00000019 00000011): flips 394, surfaces mapped 3 (map failures 0, let go 0, remaps 3, zap timeouts 0), latch mean 10136 us longest 17050 us (0 over 50 ms), copy fallbacks 0, back to boot 0
```

* The wedge state: frozen swap counter and the still-alive kernel (CR-inclusive 14289 and 15903):

```
opi5: display t+2040s: swap ok 394 rej 0 (reason 0) copy 394 ...  (frozen at 394 since t+1690)
opi5plus beat 2205 s, requests 270542, blocks 38418048, failures 0, procs 224, stage 32 (L12 late done), vfree 1901, vspec 355683, vcomp 0, vinact 123643, vact 187338, vext 594796, vpur 2730, vcact 0, vcmode 8, dbavail 287356, lsr 0
```

* The last CollectionsPoste SIGKILL of the run (CR-inclusive 11015, t+~1680):

```
opi5plus: exit CollectionsPoste[538] rv 0x9 sig 9 code 0 age 6
```
