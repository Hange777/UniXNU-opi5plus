# Handoff - Orange Pi 5 Plus, UniXNU, 2026-11-12 (after round 10)

Read `evidence/sd-clock-investigation.md` (rounds 7-10, the SD line's conclusion), then `evidence/round4-6-result.md`
(rounds 4-6: block-exec, the SD counters, the serial reset key), then `evidence/round3-result.md` for the input
round these built on. This page is only the rest.

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

1. **Input - injected reports work, a real pointer does not.** Round 3 settled the injection half: the digitizer
   collection is published on the event service, `IOHIDEventDriver` opens it, 4,830 reports were accepted with 0
   fails and `vsync`-free, and SpringBoard unlocked itself into the home screen (`evidence/round3-result.md`). What
   no round has done is attach a **USB** device: a wireless mouse hot-plugged during round 3 changed nothing, which
   is the expected result while there is no host stack (no USB configuration in `configs/`, no `IOUSBHostFamily` /
   `AppleUSBXHCI` in the prelink). Two round-2 measurements remain retired and must not be reused: the three
   `XhcDriverBindingStart` lines are UEFI `XhciDxe.efi` output printed before `DARWINBOOT: hand-off`, so they say
   the firmware has USB and nothing about XNU; and `IOHIDSystem` is not a class of this kernel at all
   (`evidence/m5/M5-08`), so its `absent` reading carries no information. Next: make the prelink feasibility
   decision for the USB host stack (17 kexts, closure 29) before scheduling it as work.
2. **Banding inside the large glyphs.** Clean small text, striped clock digits, static screen, so not tearing.
   Best candidate from round 2: `metal2vulkan-degrade-shadowed-texture` ("a 2D A8Unorm texture of 332x200 ...
   rows of 352 bytes ... refuses a DRM_FORMAT_MOD_LINEAR image (ERROR_INITIALIZATION_FAILED)"). The GOP copy is
   byte-complete (pitch 10240 = 2560x4). Round 3 did not revisit it, and the home screen has no large glyph to
   compare. Next: find the shape the digits use; the one-shot `biplanar surface accepted` line never printed.
3. **`vsync sent=0 arms=0`** in every vsync line of both rounds, so swaps complete without the userland ever
   arming a vsync — and round 3 unlocked the device the same way. Next: find the caller that arms one, and
   whether the compositor waits at all for a static UI.
4. **Memory exhaustion over a long run is the next real problem.** The rounds 4-10 windows are short (100-505 s
   of kernel time), but the long runs end with the free list gone: the round-2 run ended at 6,180 free pages
   (about 24 MB) at t+3600, and round 3's full 3,600 s run is worse — the free list sits in the low thousands
   of pages after t+2000 and bottoms at **1,440 pages (about 5.6 MB) at t+2825** (`logs/boot-round3.log:21020`,
   `vfree 1440`), and the SD read side stalls at the same time: blocks sit at 38,641,792 for the 40 s from
   t+2740 to t+2780 (`logs/boot-round3.log:20305` first frozen beat, `vfree 3523`; `:20584` at t+2780,
   `vfree 3363`) and then only crawl afterwards. "The reads stop because nobody is left to ask" holds in both
   runs: 254 processes fitted and nothing can serve a client that asks for a large buffer (`porting.toml` area
   `memory`, limits). Any round that wants to run for hours — or to add frame-rate or GPU-pool work on top —
   has to answer where the memory goes before anything else.

## Key commands

Build, as root in the WSL distro `unixnu` (`evidence/round2-baseline/10-build-kernel.sh` is this pair plus the
before/after hashes and the error counts):

```sh
cd /mnt/d/rk-mac/repos/UniXNU
export http_proxy=http://$GW:7980 https_proxy=http://$GW:7980   # GW = ip route | awk '/^default/ {print $3; exit}'
UNIXNU_OUT=/opt/unixnu-out ./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
UNIXNU_OUT=/opt/unixnu-out UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553 ./unixnu build kernel -D ../UniXNU-opi5plus
```

Payload: `wsl -d unixnu -u root -e bash /mnt/d/rk-mac/tools/wsl/tmp-payload-r10.sh` (writes
`artifacts/m5/payload-round10.img`; one stub per round, `tmp-payload-r4.sh` through `-r10.sh`, each is the record
of that round's argument list and the only place the round's experiment argument is set — round 10's is
byte-identical to round 9's except the output paths).

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
  written. Round 4 sits at 981 bytes / 53 arguments (round 3: 993 / 54; the round-4 re-shuffle dropped
  `sd-auto-reboot=3600` and `offline-legacy-paint=0` and added `opi5plus-block-exec=bluetoothd`), so adding an
  argument still means dropping one.
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
