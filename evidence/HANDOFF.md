# Handoff - Orange Pi 5 Plus, UniXNU, 2026-11-13 (after round 16)

Read `evidence/sd-clock-investigation.md` (rounds 7-10, the SD line's conclusion), then `evidence/round4-6-result.md`
(rounds 4-6: block-exec, the SD counters, the serial reset key), then `evidence/round3-result.md` for the input
round these built on, and `evidence/round11-result.md` for the direct-scanout round. This page is only the rest.

## Where the port stands

The iPadOS 17.7.11 userland boots from this repository's payload, 250+ processes run, and at 2560x1440 the
compositor's frames reach the panel. **Input works, by scripted injection:** round 3 published the digitizer
collection on an `IOHIDEventService`, `IOHIDEventDriver` opened it, 4,830 injected reports were accepted with 0
failures, and the userland went from the lock screen **into the home screen** — round 2 sat on the lock screen for
63 minutes. Kernel `0359b168...` (51,695,616 B), payload `407ac58a...` (100,663,296 B), command line 993 B / 54
arguments, log `logs/boot-round3.log` (project working tree, in no repository).

Rounds 4-10 (2026-11-12) then measured and closed the two things that were cheap to close:

* **`bluetoothd` is blocked at the exec gate with zero code** (`opi5plus-block-exec=bluetoothd`, patch 0006): 268
  `exit bluetoothd[` lines in round 3, **0** in round 4 — the daemon never starts again (`evidence/round4-6-result.md` S1).
* **The SD throughput line is closed.** The PIO read path runs at ~11.5 MB/s, which is 89% of what the card's
  ~26 MHz 4-bit clock offers; the fill/drain split puts 81.6% of a chunk in waiting for the card and 8.8% in the
  CPU; both earlier models (poll volume, batch size) were falsified on the board; the CIU divider is linearly
  effective; and the next 2x is CRU clock-tree work (move the sdmmc source clock to 52 MHz), not driver work
  (`evidence/sd-clock-investigation.md`).

Round 11 (2026-11-12) then made direct scanout the working display path:

* **`opi5-gop-mirror=0` (now the board default in `board.toml`): zero-copy scanout, verified end to end.**
  `gop 0/158`, every swap `0 copies`, `copy fallbacks 0`, no `gop blit` line anywhere; the 30 s readback says
  `window on a compositor surface` every time it prints — the window never touches the boot buffer again
  (`evidence/round11-result.md` S1-S2). Latch mean 13,965 us / longest 17,108 us / 0 over 50 ms; 158 swaps, 0
  refused; and the first frame-rate measurement on the board: **50 fps** during the unlock animation
  (busiest second 61, mean gap 19,999 us). The mirror CPU copy this replaces measured 88.6-95.7 ms a frame.
* **The observation baseline is in.** The beat line now carries `vinact/vact/vext/vpur/vcact/vcmode/lsr`
  (patch 0002 regenerated): `vcmode 8` / `vcact 0` is the compressor baseline a future `vm_compressor=2`
  comparison boot measures against, `lsr` prices card-medium reads by age, and the `vfree` floor
  (~1,576 pages at t+2135) is again XNU's designed steady state, not a copy-path artifact.

Round 12 (2026-11-12) then closed the attribution half of the white-block line:

* **The mali relay is signed and the sampler watches the suspect.** Every `mali: user:` line carries
  the owning process's name and pid (`package/Mali/Mali.cpp`, in the nested repository `package/Mali`),
  and the sampler slot `bt3` moved to `WidgetRenderer_D` (`bt5`/`bt6` dropped; command line 939 B /
  51 arguments). On the board: all 3,756 relay lines carry a pid — backboardd[57] 3,643,
  SpringBoard[27] 96, CollectionsPoste[528] 16, WidgetRenderer_D[491] 1 (only its bundle-loaded
  line) — the 75 degrade/`ERROR_INITIALIZATION_FAILED` lines are 100% backboardd[57], the round-2-era
  332x200 signature printed **zero times**, and the white-block hypothesis is revised (conjecture) to
  a WidgetRenderer surface-content problem (`evidence/round12-result.md`). The validateSwap rejection
  print is in the payload and stayed untriggered (rej 0 throughout).

Round 13 (2026-11-12) then put the census on the surfaces and pushed the white-block attribution to the
composition pipeline:

* **The surface census took its first archive, and no scanned surface is a plain white layer.**
  `opi5-surface-census=1` gives the direct path a ledger (cap 32 surfaces) plus a 30 s per-surface
  content sample — 25 pixels on a 5x5 grid over the visible rectangle, read-only, after a latch
  confirms. All 12 census lines logged: three surfaces, all 2560x1440 ARGB stride 10240 —
  `0x19a631000`/`0x1ab873000` both `backboardd[57]` (lock screen: 24/25 grid points black + 1 white;
  desktop: mixed) and `0x1bfa4e000` `PerfPowerService[73]` (mixed from the first sample). **No
  `uniform` line exists anywhere** — so the white the panel shows is **inside the composited frame**
  (the widget-placeholder region whose content is blank; 1 white point in 25 covers ~1/25 of the
  panel, the white block's area fraction), and the user's new observation — the App Store icon
  renders **on top of** the white block — puts the block below the icon layer. The display path is
  now cleared end to end: direct scanout, 0 copies, 0 fallbacks, and no white layer for it to produce
  (`evidence/round13-result.md`). Parity: swap ok 175 / rej 0, latch mean 11,829 us, procs 228,
  injector fails 0; the capture ran its full 2,700 s and **the board stayed in XNU (no 0x1d; quota
  1/2)**.

Round 14 (2026-11-13) then closed the USB input line with the S1/S2 code (committed at `800f9e5`)
brought up in two tiers on the board:

* **Real keyboard and mouse reports reach `handleReport` through the OHCI mailbox.** Tier 3
  (`opi5-usb=3`) verified enumeration alone: the Razer Viper V2 Pro receiver (1532:00a6) enumerates
  on ohci1 (boot mouse iface0 + boot kbd iface1 + vendor iface2, report protocol `id -1, 8 bytes,
  buttons@0/3 x@32/16 y@48/16 wheel@24/8`), VBUS_EN driven high, two disjoint bounce pools. Tier 5
  (`opi5-usb=5`) verified the mailbox end to end: `HID polling started (keyboard 1, mouse 1)` at
  boot, `errors=0` in all 193 poll lines, 4 operator interaction sessions pushing 24 non-zero
  windows (largest 3,701 reports in one window), terminal counters `hid_total=32161 kbd_seq=8
  mouse_seq=32153 wheel=4`, and the consumer's final **6,154 reports handed to `handleReport`, last
  result 0x0, with motion 6,067** — while the synthetic injector coexisted (7,981 reports, fails 0).
  `hid_total` ≫ handed is the designed 8 ms aggregation tick (`NativeHidInput.cpp:529-536`). Parity
  with round 13: 2,596 swaps / 0 copies / latch mean 11,600 us / 0 over 50 ms / procs 224 / panic 0
  (`evidence/round14-result.md`). **A new problem was exposed by the interaction** and is item 5
  below.

Round 15 (2026-11-13) then verified the upstream fence fix `mali-sched-wait=2` on the board
(command line 990 B / 54 arguments; kernel unchanged at `8463916c...`, payload `a9c363c6...`,
`evidence/round15-result.md`):

* **The fence layer is fixed.** All 66 fence lines of the capture read `0 of N swaps arrived with
  GPU work not done; 0 flips waited (0 us, 0 gave up)` — including the interaction-storm peak
  (`0 of 372 swaps` at t+1690), against round 14's `2503 of 2551 ... longest 250,346 us, 2 gave up`.
  The storm itself ran to 394 swaps with every one retired, 0 copies, latch mean 10.1 ms.
* **The wedge reproduces without it (second mechanism).** The panel froze anyway at the user's
  App-Store hover: swap counter frozen at 394 from t+1690 through t+2205, `requests` crawling to a
  stop, no fence backlog, no gave-up, no real panic, the kernel beating and feeding the watchdog
  the whole time (the board was left on XNU overnight by the user's instruction). Round 14's
  mach_msg2 wedge-thread suspect (`0x1daf40b58` / `0x1daf46bdc`) stands (item 5).
* **The white blocks are still there** (user, confirmed) while the census reads `mixed n=16-21` on
  all three surfaces — the 5x5 full-frame grid cannot sample the block's region, so round 15's
  `mixed` readings and the 0 `fff2f2f7` occurrences say nothing about the block's interior; the
  verdict needs region-level sampling at the user's coordinates (item 2).
* **CollectionsPoste's SIGKILL loop is thinner, not gone:** 5 exits in the t+1590-1680 90 s window
  (~18 s spacing vs round 13's 12 s), two of them at new own-text crash sites
  (`0x1b6fe4844` / `0x1b68af448`, esr `0x82000007`); silent after t+1680 only because the wedge
  stopped the render side. The exit-78 storm is unchanged (191 lines).

Round 16 (2026-11-13) then settled the exit-78 storm, **and the round-14 reading of it is corrected**
(unattended forensics run: nobody at the panel, the board left in XNU overnight by the user's
instruction, kernel `b8113698...`, payload `12badb79...`, command line 940 B / 52 arguments —
`sd-exec=1` in, census/hid-input/bt4 out for headroom, `evidence/round16-result.md`):

* **The storm is the bluetoothd block-exec retry echo, not an application-spawn failure.** The
  capture holds 93 `exit xpcproxy[...] code 78` events and 93 `exec of /usr/sbin/bluetoothd
  blocked` events in a **strict refusal-then-corpse alternation, 0 orphans** (pids 81→373, ~1 per
  10 s from t+~315, the desktop daemon spawn wave; a later re-attach continues the pairing 3:3).
  launchd/xpcproxy spawn through **posix_spawn(2), not execve(2)** — patch 0006's comment recorded
  that long ago — so the new sd-exec execve hook correctly saw only three exec lines (launchd ×2,
  apfs_boot_util), the new xpcproxy watch and its nr-59 ENOENT exception stayed unexercised, and
  the exec-ENOENT reading is falsified for the observed data. The refusal (EPERM at the spawn
  entry) and the EX_CONFIG corpse are one event seen twice.
* **Consequences:** the exit-78 layer is **removed from the application-launch problem list**
  (it is the designed self-noise of the round-4 block-exec trade, interaction-independent); the
  disposal of the ~1-per-10-s noise is a user decision, recorded not changed. The kept new
  observations: a **thread flood** — `contextstored[56]` and `PerfPowerService[73]` both cross the
  32-thread high watermark — and named Data-volume permission failures (keybagd `nr 475 errno 17
  path /var/logs`; nsurlsessiond EEXIST/EPERM on its Library and tmp dirs). Round 14's
  webbookmarksd lead did not reproduce in this window. Everything else was a clean idle boot:
  fence 0% held again, no real panic, capture ended naturally at beat t+1240.

## The infrastructure this session added

* **Round-end automation: the board resets itself back to Linux.** `py -3 tools/wsl/serial-reset.py` sends 0x1d
  (Ctrl-]) at COM3; NativeSd's 100 ms UART thread panics on it, PSCI resets the board, and Linux is back in about
  **19 s** (round 6, verified over SSH: `/proc/uptime` 29.7 s; `journalctl` shows a fresh boot). No more power
  cable. This is why `board.toml` and the command line now carry `serial=1`: the INPUT bit is clear, so
  `serial_keyboard_poll` never starts and the reset thread is the FIFO's only reader. **The serial input channel
  is closed until a future reader and the hook agree on arbitration.**
* **Detached capture:** `Win32_Process.Create` (WmiPrvSE parent) starts `boot-uefi.py --tail-secs N`, so killing
  the agent does not kill the capture.
* **Build residue, every build:** move `startup.o.json` aside (into `logs/` — `evidence/**` is off limits),
  `touch osfmk/kern/startup.c`, delete the generated Makefile that contains `SRCROOT)/osfmk` (it grows back
  every build). Packaging must run in the foreground — `nohup` gets killed and leaves no log.
* **Incremental kernel builds are not byte-reproducible:** the same source built twice gave `d78a28df...` and
  `4b2e270b...` (both 51,699,712 B). Judge a kernel by the sha the payload records, never by "same source, same
  bytes" (`evidence/sd-clock-investigation.md` S6).

## Root causes closed so far

* Round 2: the `/private/var/mobile{,/Library,/tmp}` directories were owned by root, so every daemon that
  creates its tmpdir at startup exited 1 every 10 s; patch 0002 (commit `ca0cef3...`) creates them with their
  final owner (`evidence/round2-result.md`).
* Round 4: `bluetoothd`'s exit loop (the one process round 2's fix did not cover) is blocked at the exec gate by
  the `opi5plus-block-exec=bluetoothd` argument; 268 corpse lines -> 0, no driver code.

## What is still broken, and the next step for each

1. **Input - injected reports work, and since round 14 real USB keyboard/mouse reports reach the consumer too** (round 14 closed the "no USB device enumerates" half: the polled-OHCI mailbox feeds `handleReport`, `evidence/round14-result.md`). Round 3 settled the injection half: the digitizer
   collection is published on the event service, `IOHIDEventDriver` opens it, 4,830 reports were accepted with 0
   fails and `vsync`-free, and SpringBoard unlocked itself into the home screen (`evidence/round3-result.md`). What
   no round has done is attach a **USB** device: a wireless mouse hot-plugged during round 3 changed nothing, which
   is the expected result while there is no host stack (no USB configuration in `configs/`, no `IOUSBHostFamily` /
   `AppleUSBXHCI` in the prelink). Two round-2 measurements remain retired and must not be reused: the three
   `XhcDriverBindingStart` lines are UEFI `XhciDxe.efi` output printed before `DARWINBOOT: hand-off`, so they say
   the firmware has USB and nothing about XNU; and `IOHIDSystem` is not a class of this kernel at all
   (`evidence/m5/M5-08`), so its `absent` reading carries no information. Next: make the prelink feasibility
   decision for the USB host stack (17 kexts, closure 29) before scheduling it as work.
2. **White blocks — the banding is closed as a phenomenon, this one is separate.** Round 11 closed the
   banding inside the large glyphs: with `opi5-gop-mirror=0` it is gone, and the early-boot stripes
   (the Apple-logo phase), the mirror copy and the early paints share the producers — CPU writes into
   the buffer the VOP is concurrently scanning (the early console's own buffer is the scanout buffer,
   `logs/boot-round11.log:1936` base 0xE6740000 = Esmart2). The banding is **avoided by direct scanout
   (confirmed)**; the mapping-attribute audit (`evidence/round11-result.md` S7) found every kernel
   mapping that can write a scanout buffer non-cacheable (bootMapping maps Device nGnRnE via
   `kIOMapInhibitCache`, the frame buffer lies outside the ADT DRAM window, each kernel write is
   followed by `dsb sy`), so the write-combining and dirty-cache candidates are eliminated; what
   remains is conjecture — scanout racing unsynchronized CPU writes into the live buffer (the early
   console's writer is the firmware's DarwinBoot diagnostic console, whose UEFI mapping attributes are
   not visible here). The
   **white blocks (three white rounded rectangles) remain** and are
   independent of the banding — the round-11 run split them off. Round 12 then retired the
   round-2-era reading that tied them to the A8Unorm 332x200 degrade line: the signed mali relay
   shows the 332x200/rows-352 signature printed **zero times** in the whole capture while the blocks
   were on the panel, and the degrade/failure path is 100% backboardd[57] — nothing in it belongs to
   WidgetRenderer_D. The revised hypothesis (**conjecture**) is a surface-content problem: a
   WidgetRenderer surface is created and handed to the compositor but never rendered into, and the
   compositor composites the white fallback. Round 13's surface census then settled what the display
   path can and cannot be: **no census surface is a plain white layer** (no `uniform` line in the
   whole capture), the lock-screen composites read 24/25 grid points black + 1 white (~1/25 of the
   panel = the block's area fraction), and the white content is **inside the composited frame** — a
   widget-placeholder layer that exists while its content is blank, composited by backboardd as-is;
   the App Store icon renders on top of the block (user, confirmed). The display path is cleared end
   to end (direct scanout, 0 copies, 0 fallbacks, and no white layer for it to produce).
   **Next step, two options (user decision):** (a) region-level sampling at the white block's
   user-calibrated coordinates (a handful of targeted pixels instead of the 5x5 grid) to upgrade
   "the white is inside the surface" to confirmed, or (b) accept the attribution — the white
   content's origin is the render server's composition pipeline, a userland-side matter — and move
   to another direction.
   Round 15 sharpens this: the user confirmed **the white blocks are still on the panel** in the
   fence-fixed boot, and the census's `mixed n=16-21` readings coexist with them — the 5x5
   full-frame grid cannot sample the block's region at all (round 14's `near-uniform fff2f2f7*24`
   described a whole frame dominated by background, not the block's pixels), so only the region
   sampling of option (a) can settle it; `mali-sched-wait=2` did not touch the blocks.
   Resolved by source read: the single t+1595 `back to boot 1` is the designed blank-swap path —
   `kickOnce()`'s `surface == nullptr` branch paints the boot buffer black (`directToBoot()` ->
   `flip(bootIova)` -> `fill_pixels 0xff000000`) while the unlock session is torn down and rebuilt, and
   the next surface swap flips back; one occurrence because blank swaps only happen at session
   transitions; not a copy fallback (fallbacks 0 throughout); why IOMFB issues a blank swap at teardown
   is not answerable from our logs.
3. **`vsync sent=0 arms=0`** in every vsync line of both rounds, so swaps complete without the userland ever
   arming a vsync — and round 3 unlocked the device the same way. Next: find the caller that arms one, and
   whether the compositor waits at all for a static UI.
4. **Memory — the earlier "exhaustion" reading was wrong; what remains here is observation, not a fix.** The
   low `vfree` numbers are XNU's designed steady state, not a pathology: reclamation runs off the
   speculative/inactive queues, and free + speculative together never dropped below about 0.92 GB in any of
   the three long runs (round 1 ends at 2.03 GB; round 2 bottoms near 0.92 GB — `vfree 1111` at t+1710 with
   `vspec` ~242,000 pages, `logs/boot-round2.log:9860`; round 3 ends at 1.30 GB, its true `vfree` floor is
   1,131 pages at t+1860 during the read storm, `logs/boot-round3.log:12314`, while the 1,440 pages at
   t+2825 (`:21020`) is only the floor of the window after the reads stalled). Round 2 shows the reclaim
   loop live: between t+1775 and t+1785 the read storm takes `vspec` from 360,500 to 230,000 pages (~500 MB
   of file cache recycled), after which `vfree` bounces back to ~10,000 pages (the frozen read beats sit
   near `logs/boot-round2.log:26438`). The VM compressor is not missing either: this kernel prints
   `"vm_compressor_mode" is 8` (`logs/boot-round1.log:1613`) with the 4,608 MB pool mapped (`:1591`); mode 8
   has freezer semantics — `compressor_is_active` is FALSE (`osfmk/vm/vm_pageout.c:5050-5053`) so pageout
   does no active compression (`:3700-3712`), the kernel's only compression path is the freezer
   (`osfmk/vm/vm_object.c:7557-7567`), and it only freezes suspended processes of which the board has none —
   hence `vcomp 0`. Switching it on is a boot argument, `vm_compressor=2` (`vm_pageout.c:5040-5044`), zero
   kernel code. jetsam not killing anything is also correct: CONFIG_JETSAM is in
   (`MASTER.arm64.bcm2837:63`), the available-memory reading includes the file cache
   (`osfmk/vm/vm_page.h:1645-1652`), the pressure threshold is ~12% ≈490 MB (`kern_memorystatus.c:248`,
   `:252-253`), the file cache stays far above it, and the three round logs hold zero `jetsam` /
   `memorystatus` / SIGKILL lines. The SD read stalls are likewise independent of memory (85-90%
   confidence): round 1 stopped reading with ~209,000 free pages (820 MB) still free, in all three rounds
   the `vfree` collapse precedes the read stall by more than 1,000 s, block failures stay 0 throughout, and
   successful read bursts still land during the stall (round 3, t+2785, +47,744 blocks) — the userland goes
   quiet and nobody issues I/O any more. Next: extend the beat line with
   `vext/vpur/vinact/vact/vcact/vcmode/lsr` (patch 0002's beat thread, `:1553-1625`, and the counter
   functions in `osfmk/kern/debug.c`, `:1920-1930`) to build the jetsam/compressor baseline, then optionally
   run a `vm_compressor=2` comparison boot.
5. **Application launch layer - exposed by round 14's real input; round 16 removed its red herring.**
   With the mouse working (mode5's full chain), the user's app-launch attempts all opened and
   instantly quit, then the screen froze; unplugging USB changed nothing. The passive serial probe
   (`logs/probe-frozen.py`, read-only 15 s) proves the kernel and userland were alive (beats
   2720/2725/2730 healthy, launchd spawn/exit continuing) while **the compositor stopped committing
   frames (swap frozen at 2,596)** and **backboardd/WidgetRenderer_D threads were repeatedly
   sampled parked at the same two user PCs (0x1daf40b58 / 0x1daf46bdc)**; backboardd's
   `mali ... refuses a DRM_FORMAT_MOD_LINEAR image for host allocations` (14 lines, msgs 34-62) sat
   on the app-texture path as a second clue. **Round 16 falsified the third clue:** the
   `exit xpcproxy[...] code 78` storm is not an app-spawn failure — it pairs 1:1 (strict
   refusal-then-corpse alternation, 0 orphans) with the round-4 block-exec gate's bluetoothd
   refusals, because launchd/xpcproxy spawn through posix_spawn(2) which the execve-only hooks
   never see; the exit-78 layer is withdrawn from this list, and its ~1-per-10-s noise is a user
   decision (default: accept and record). What remains here is the real defect set: **a
   UI/render-thread wedge** (not a kernel death, not a watchdog scenario) plus the same-family
   white blocks (WidgetRenderer threads in the wedge queues). **Next step: symbolise the wedge PCs
   against the dyld shared cache (`dsc.py --slide`)** (`evidence/round14-result.md` S4).
   Round 15 splits the freeze further: with `mali-sched-wait=2` the fence gave-up path is verified
   absent (66/66 fence lines at 0% / 0 gave-up, through the storm peak) and the screen **still**
   froze at the App-Store hover with the swap counter frozen at 394 — a second wedge mechanism,
   independent of the fence backlog, with CollectionsPoste's SIGKILL loop running thinner (5 exits,
   ~18 s spacing, two at new own-text sites `0x1b6fe4844` / `0x1b68af448`) inside the same window
   (`evidence/round15-result.md` S2-S3).

## Key commands

Build, as root in the WSL distro `unixnu` (`evidence/round2-baseline/10-build-kernel.sh` is this pair plus the
before/after hashes and the error counts):

```sh
cd /mnt/d/rk-mac/repos/UniXNU
export http_proxy=http://$GW:7980 https_proxy=http://$GW:7980   # GW = ip route | awk '/^default/ {print $3; exit}'
UNIXNU_OUT=/opt/unixnu-out ./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
UNIXNU_OUT=/opt/unixnu-out UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553 ./unixnu build kernel -D ../UniXNU-opi5plus
```

(That is the historical manual form. The two `tools/wsl/` harness scripts now read these values from
the gitignored `/mnt/d/rk-mac/.env` — `UNIXNU_UNIQUE_CHIP_ID`, `RK_MAC`, `GIT_PROXY` — and refuse to
run without them, so no host-specific value is hardcoded in the scripts.)

Payload: since round 11 the regular pack is `tools/wsl/pack-payload.sh` — run as root in the WSL distro from
`/mnt/d/rk-mac/repos/UniXNU`, `bash /mnt/d/rk-mac/repos/UniXNU-opi5plus/tools/wsl/pack-payload.sh
<OUTPUT.img>` (writes `artifacts/m5/payload-round11.img` in round 11; no per-round stub and no
`--kernel-arg`, board.toml is the single argument source; the build step is not inside). The round stubs
`tmp-payload-r4.sh` through `-r10.sh` remain as the record of those rounds' argument lists.

Board: `powershell -File tools/deploy-payload.ps1 -SkipMaskrom ...` pushes the four payload files over the SD-first
Linux and then flashes SPI-first. Read its header before running it: a SPI-first board that panics is unreachable.
**Ending a round: `py -3 tools/wsl/serial-reset.py` sends 0x1d and the board is back in Linux in about 20 s**
(round 6 on; before that it was the power cable).
Serial capture: `powershell -File tools/serial-capture.ps1 -Port COM3 -Out D:\rk-mac\logs/<name>.log`, or
`py -3 tools/serial-cap.py --port COM3 --baud 1500000 --secs 3800 --log logs/<name>.log`, or the detached reader
`py -3 tools/boot-uefi.py --port COM3 --baud 1500000 --log logs/<name>.log`, which keeps writing after the agent
that started it is gone (rounds 3-10 were captured this way; rounds 4-10 were started detached through
`Win32_Process.Create`). Only one reader may hold COM3, so release the capture before `serial-reset.py` sends
the byte — which is why the reset itself never appears in the capture.

## Traps that already cost time

* The command line has a **1000-byte ceiling** (`scripts/unixnu/payload.py`), enforced before the volume is
  written. Round 4 sat at 981 bytes / 53 arguments (round 3: 993 / 54; the round-4 re-shuffle dropped
  `sd-auto-reboot=3600` and `offline-legacy-paint=0` and added `opi5plus-block-exec=bluetoothd`); since
  round 11 it is **985 bytes / 53 arguments** (`sd-trace-io=0` dropped for budget — its default is 0 —
  `opi5-gop-mirror=0` added), and since round 15 **990 bytes / 54 arguments** (`mali-sched-wait=2`
  appended; 10 bytes of headroom left); since round 16 it is **940 bytes / 52 arguments**
  (`sd-exec=1` added for the exec hook, `opi5-surface-census=1`, `opi5-hid-input=2` and
  `opi5plus-bt4=chronod` commented out for headroom — each with a restore line in `board.toml`),
  so adding an argument still means dropping one.
* `prepare-xnu-source.py` restores 26 Apple headers over HTTPS and direct TLS is reset on this network; export the
  proxy above or the build stops there.
* A leftover `startup.o.json` compile record makes the bootstrap pass skip and `archive-03` fail: move it aside
  before a build (`evidence/round2-baseline/stash/`). The bootstrap pass's own `ld: symbol(s) not found` is
  expected. But moving it aside is only safe when a XNU source actually changes: with no XNU edits clang writes
  no new `-MJ` record, the bootstrap pass finds none and fails. Round 3 hit exactly this and `touch`ed
  `osfmk/kern/startup.c` to force one object and rebuild the record; see `evidence/round3-baseline/README.md`.
* `unixnu build kernel` returns 0 even when the display archive fails; check
  `logs/archive-15-build-opi5plus-display.log` for `error:` and the build's `kernel: <sha256>` line before
  trusting a payload.
* Only one reader may hold the serial port; a capture and a boot helper cannot both own COM3.
* On a **persistent** Data volume the `.opi5plus-seeded` marker makes the whole seed return early, so a seed fix
  silently does nothing on the second boot - that is how the directory owner survived several rounds.
* The beat line's `gop X/Y` is the letterbox's presents/refusals, not a GOP write count; the GOP writes are
  `opi5-gop-mirror`'s `gop blit N frames`. Read the counters, not the screen: a static screen and a hung one look
  the same.
* The injector has a clean signature and it is worth checking instead of the screen: 23 reports per gesture
  (2 for the Home press/release + 21 for the swipe), `fails 0` and `last result 0x0` in every heartbeat, `open 1`
  throughout. A different ratio means the timer or the send path changed; a non-zero `fails` means the open gate
  was lost, and the injector says so in words (`waiting: no event driver`, `waiting: not open`, `giving up on the
  open gate`) rather than silently stopping.
* An unlock is not a vsync event on this port: round 3 went from the lock screen to the home screen with
  `vsync sent` still 0, so do not read a zero vsync counter as "the UI did not move".
