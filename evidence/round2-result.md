# Round 2 result — the Data volume's mobile directories carry their owner; the lock screen renders

Date: 2026-11-11 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
Scope: one boot of the round-2 kernel and payload, captured to 3,800 s. The run ends itself at t=3600 with

```
panic(cpu 0 caller 0xfffffff00776bdf4): opi5plus: sd-auto-reboot after 3600 s @kern_exec.c:10535
```

| item | value |
| --- | --- |
| kernel | `45fbfab3cae0c66dcf73b85ba08a82ae7a580b0c4e6beabab1d81f76dc0a3893`, 51,695,616 B |
| payload | `artifacts/m5/payload-round2.img`, 100,663,296 B, sha256 `80e4f2965501789a851974bd8366fd686c56aacc6bc443ceb2f3c36fc527d2e1` |
| command line | 993 bytes, 54 arguments (the limit is 1000) |
| log | `logs/boot-round2.log`, 3,522,343 B; it also carries the reset and the boot that follows the panic |

The command line was read back out of the running kernel's own `boot-args = …` line: 54 arguments, 993 bytes,
which is the payload-identity pair recorded in `evidence/round2-baseline/README.md` seen from the board.

## Round 1 against round 2

The two runs use the same board, the same card and the same display path; the difference is the round-2 kernel
and the fixes it carries.

| metric | round 1 | round 2 |
| --- | --- | --- |
| failure exits of the five sampled daemons | 1982, one every 10 s | `biomed` 0, `callservicesd` 0, `nsurlsessiond` 0; `biomesyncd` 2, both `code 0`; **`bluetoothd` 259 × `code 1`, every 10 s from t+696** |
| `swap ok` | 0 (about 150 000 refusals, all `reason 4`) | **54 accepted, 0 refused** |
| `gop blit` | 0 | **54 frames**, 14,745,600 B each (2560x1440x4), 88,634–95,723 µs a frame (about 155–167 MB/s), 796,262,400 B copied in total, 0 refused |
| display clients | 0 | up to **22 opened, 8 clients, 273 calls** |
| block-layer reads | 14.48 GiB, then frozen | **40,139,008 blocks (19.1 GiB)**, 286,338 requests, 0 failures; nearly all of it before t≈2400 |
| `vfree` | last value 206,206 pages | **last value 6,180 pages (about 24 MB)**; `vspec` 236,942, `vcomp` 0 throughout |
| `procs` peak | 165 | **254** |
| `dbavail` | 392,458 → last value | 392,450 at t+5 → **331,178** at t+3600 (61,272 blocks, about 239 MiB if a block is 4 KiB, written to the Data volume) |
| screen | nothing | **the lock screen** ("Wednesday, December 31 / 4:40"), confirmed by a photograph of the panel |

The freeze in round 1 and the 0/0 display line were both symptoms of one fault; with it fixed the same payload
produces frames and a UI. The second witness is the memory column: `vfree` falls from 568,193 pages at
t+600 to 6,180 at t+3600 as the userland's processes and their mappings accumulate, while `vcomp` never moves
off 0, so the 24 MB left is mapped, not leaked-compressed memory.

## The root cause, and the fix

`/private/var/mobile`, `/private/var/mobile/Library` and `/private/var/mobile/tmp` ended up owned by **root**:

1. the kernel's skeleton table in patch 0002 created those directories itself, as root, before anything else
   could;
2. `opi5plus_ensure_dir()` skipped a directory that already existed, so the seed never corrected the owner;
3. on a volume that already carried `/private/var/.opi5plus-seeded`, the seed returned early for the whole tree;
4. launchd's `fixup-mobile-tmp` job also skips a directory that exists.

mobile (uid 501) therefore could not `mkdir` inside its own temporary directory, got EACCES, and the daemons that
do this at startup exited 1 every 10 s. Patch 0002 now creates the mobile directories with their final owner and
applies the attributes on EEXIST, so the owner no longer depends on which of the four steps runs first.

The fix is visible in the log, from the `stat` probe:

```
opi5plus: stat /private/var/mobile        rc 0 uid 501 gid 501 mode 040755
opi5plus: stat /private/var/mobile/Library rc 0 uid 501 gid 501 mode 040755
opi5plus: stat /private/var/mobile/tmp    rc 0 uid 501 gid 501 mode 040755
opi5plus: stat /private/var/tmp           rc 0 uid 0   gid 0   mode 041777
opi5plus: stat /private/var               rc 0 uid 0   gid 0   mode 040755
```

## What else round 2 changed

* The display backend now accepts the compositor's two-plane `b3a8` surfaces and copies them out of plane 0's
  row (`width * 4` bytes), which is the row the pixels are in; the accessor reports the last plane's row. This is
  what turned "swap rejected" into 54 copied frames (`package/Opi5plusDisplay/NativeLegacyDisplay.cpp`).
* The thread sampler's exit line now names the image a frame belongs to (`opi5plus_text_base()`, the start of the
  lowest executable mapping) so a frame at `0x100xxxxxx` no longer needs the ASLR slide guessed
  (`patches/xnu/0004-thread-sample.patch`).

## Still unresolved

### 1. Input does not work at all

* USB enumerates nothing: no device is ever attached. A wireless mouse plugged in during the run changed
  nothing. The `XHCI: Initialize DWC3` / `XhcDriverBindingStart: XHCI started for controller @ …` lines are
  **not XNU output**: they are printed by the firmware's own `XhciDxe.efi`, before `DARWINBOOT: hand-off`, so
  they say the firmware has a USB host controller and nothing about a kernel USB stack. Round 3 established
  this on the boot log; until then these lines were read as XNU's.
* `IOHIDSystem` never appears — and on this kernel it never can. iPadOS 21H461 has no `IOHIDSystem` class: it is
  absent from the kernelcache's `IOHID*` class names, from the `site.` metaclass strings and from the mangled
  names, and no personality in any prelinked kext creates it; the single occurrence in the whole image is an
  `__os_log` format string (`evidence/m5/M5-08`, re-confirmed on the binary in round 3). So the 120 registry
  lines `class match IOMobileFramebuffer published, IOHIDSystem absent` (one every 30 s, t+30 to t+3600) are
  right and empty of information, and this metric must not be used as a gate again. The names that can appear
  are `IOHIDEventService` (kernel) and the user-space `IOHIDEventSystem`/`IOHIDEventSystemClient`.
* What *is* running is the EVB10 touch driver — `evb10: hid: input device 0xfffffff436e86980 init 1 attach 1
  start 1` — on a board that `AGENTS.md` records as having no touch controller. The same log also carries
  `NativeHidInput:0x… open by IOHIDEventDriver:0x…`, so an `IOHIDEventDriver` client did open the device: the
  device end is healthy. Round 3 read the round-2 command line and found that no report could ever have arrived
  — `sheng-touch-hid=1` (which builds the digitizer collection, and with it report ID 3) and
  `offline-hid-auto-home` (the only self-triggered report) were both absent, and the sink the reports would go
  to is an empty function on this board. That is the design as it then stood, not a fault in the link.

Next step: count the kernel's `IOHIDEventService` instances instead of asking for `IOHIDSystem`, and read the
xHCI port status registers at poll time rather than waiting for the boot to report an attach.

### 2. Horizontal banding inside the large glyphs

The lock screen's clock digits ("4:40", the large date) carry horizontal stripes; small text is clean. It is not
tearing: the screen is static and only 54 frames were ever copied. The texture-degrade warnings in the same run
name a shape this looks like — a shadow texture the Vulkan driver refuses as a linear image because of its row
pitch:

```
mali: user: msg 44: [metal2vulkan-degrade-shadowed-texture] metal2vulkan: a 2D A8Unorm texture of 332x200 over a
buffer at offset 64 with rows of 352 bytes: … the driver refuses a DRM_FORMAT_MOD_LINEAR image for host
allocations with rows 352 bytes apart (ERROR_INITIALIZATION_FAILED)
```

The GOP copy itself is correct: pitch 10240 bytes = 2560 × 4, 14,745,600 B a frame, 0 refused, and the frames are
byte-complete (`gop blit 54 frames … total 796262400 B`). That places the defect above the copy.

Next step: decide whether the banding follows the `332x200`/`rows 352` allocation (an alignment the degrade path
substitutes) or the surface's own row. The one-shot `biplanar surface accepted` line does **not** appear in this
capture, so the accepted frames were not two-plane `b3a8` surfaces at the moment of first acceptance; that is the
one piece of the surface path this log does not identify.

### 3. `vsync sent=0 arms=0`

Every vsync line in the capture — 62 of them, t+590 to t+3575 — reads `vsync hz=60 sent=0 arms=0 idle=0 late=0
start=1 refresh=1`. The counter never moves, so the refresh path this port publishes is never called by the
userland, even though 54 swaps complete. No line with a non-zero `sent` exists in the log.

Next step: find the caller that is supposed to arm a vsync (the swap path asks for one only when the client waits
for it), and check whether the compositor uses `swap_wait` at all with a static UI.

## The bluetoothd exception

`bluetoothd` is the one daemon that still exits 1 every 10 s (259 times from t+696, `rv 0x100 sig 0 code 1`). It is
not a directory-owner casualty: the round-1 static analysis found it to be the only process that prints its TMPDIR
failure and continues, and it kept looping after the owners were fixed. This board has no Bluetooth radio.

## Reproducibility

| input | value |
| --- | --- |
| kernel | `45fbfab3cae0c66dcf73b85ba08a82ae7a580b0c4e6beabab1d81f76dc0a3893` (51,695,616 B) |
| payload | `80e4f2965501789a851974bd8366fd686c56aacc6bc443ceb2f3c36fc527d2e1` (100,663,296 B) |
| command line | 993 B, 54 arguments |
| XNU source | `d8b80295118ef25ac3a784134bcf95cd8e88109f` (`unixnu.lock`) |
| rk3588 SoC source | `b5f5d949e9573b5e2232c2547abf2e3c8f7a05fe` (`feeds.toml`, `unixnu.lock`) |
| device patch series | `patches/xnu/0002-mount-data-volume.patch` `9fe0bb7a…`, `0003-gpu-addon.patch` `b4508068…`, `0004-thread-sample.patch` `ce806c0f…` — the three hashes the build recorded in `xnu-inputs.json` |
| log | `logs/boot-round2.log` |

The three patch hashes are the ones `evidence/round2-baseline/README.md` recorded from the build that produced
this kernel, so the kernel in the payload and the tree in this repository are the same code.

## What this record does not claim

* It does not claim the userland is usable: it reaches the lock screen and stops there, because there is no input.
* It does not claim the banding's cause. The texture-degrade warning is the best candidate in the log, not a
  measured cause.
* It does not claim any daemon other than the three named ones is healthy: `fairplayd.A2` (319) and `maild` (234)
  also exit and respawn in this run, both with `code 0`, so they are cycling rather than failing.
* It says nothing about frame rate: 54 frames over 3,600 s is a static UI, not a rate.
