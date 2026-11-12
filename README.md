# UniXNU Orange Pi 5 Plus

[UniXNU](https://unixnu.com) device repository for the [Orange Pi 5 Plus](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-5-Plus.html)
(Orange Pi 5 Plus, Rockchip RK3588, 8 GiB). It targets the iPadOS 17.7.11 (21H461) userland on a source-built
XNU, started from an [edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588) based firmware.

**Status: the userland reaches its home screen; input works by scripted injection and, since
round 14, by real USB keyboard/mouse through the OHCI mailbox.** Milestones M0 to M5 are done:
the iPadOS 17.7.11 userland boots from this repository's payload to `Early boot complete. Continuing system boot.`,
250+ processes run, `SpringBoard` stays alive, and at 2560x1440 the compositor's frames reach the panel through the
firmware's GOP frame buffer. Round 3 (2026-11-12) closed the input half of M6: the payload's `opi5-hid-input=2`
publishes the digitizer collection on an `IOHIDEventService`, `IOHIDEventDriver` opens it, and 4,830 injected
reports were accepted with 0 failures — which drove SpringBoard **from the lock screen into the home screen**, a
transition round 2 never made in 63 minutes (`porting.toml` marks `input` `device-verified`). Round 14
(2026-11-13) then closed the real-input half: the polled-OHCI driver (`package/Opi5plusUsb`, staged
as tier 3 = enumeration, tier 5 = poll + mailbox, parser `NativeUsb.cpp:821`) enumerated the Razer
Viper V2 Pro receiver on ohci1 in tier 3 and, in tier 5, delivered real keyboard/mouse reports to
`handleReport` — 6,154 handed with 0 errors while the synthetic injector coexisted; the device goes
in a USB 2.0 Type-A port because a hub or an xHCI port is refused (`NativeUsb.cpp:691`)
([`evidence/round14-result.md`](evidence/round14-result.md)). What is still missing after round 14
is hot-plug measurement, and interaction exposed a new problem: an application-launch layer defect
(all opened apps instantly quit with `exit xpcproxy ... code 78`, then the UI render threads wedge
at the same user PCs and the screen freezes with the compositor no longer committing frames). Earlier
rounds' two names cited as USB evidence do not hold: `IOHIDSystem` is not a class of this kernel at all (`evidence/m5/M5-08`), and
the `XHCI: Initialize DWC3` / `XhcDriverBindingStart` lines are the firmware's own `XhciDxe.efi` printing before
`DARWINBOOT: hand-off`, so they prove the firmware has USB, not that XNU does. At that point the picture still
carried horizontal banding inside the large glyphs, and the userland never arms a vsync (`sent=0` in every round
so far). Rounds 4-10
(2026-11-12) then closed two more lines: **`bluetoothd` is blocked at the exec gate with zero code**
(`opi5plus-block-exec=bluetoothd`, 268 `exit bluetoothd[` lines in round 3 -> 0 in round 4), and the **SD
throughput line is finished** — the PIO read path runs at ~11.5 MB/s, 89% of what the card's ~26 MHz 4-bit clock
offers; the fill/drain split puts 81.6% of a chunk in waiting for the card and 8.8% in the CPU; poll volume and
batch size were both falsified as the constraint on the board; the CIU divider is linearly effective; and the
next 2x is CRU clock-tree work (move the sdmmc source clock to 52 MHz), not driver work
([`evidence/sd-clock-investigation.md`](evidence/sd-clock-investigation.md),
[`evidence/round4-6-result.md`](evidence/round4-6-result.md)). The board also resets itself back to Linux now:
`tools/wsl/serial-reset.py` sends Ctrl-] over COM3 and NativeSd's UART hook panics on it — about 19 s later the
board is back in Linux (`serial=1` gives the hook the receive FIFO, closing serial input). Memory is not the next wall: the low
`vfree` readings of the long runs (6,180 pages ≈24 MB at t+3600 in round 2; 1,440 pages ≈5.6 MB at t+2825 in
round 3) are XNU's designed steady state, not a pathology — reclamation runs off the speculative/inactive
queues and free + speculative memory never dropped below ~0.92 GB in any long run; the VM compressor is
compiled in mode 8 (freezer semantics, hence `vcomp 0`, switchable with the boot argument `vm_compressor=2`,
zero kernel code), and jetsam correctly finds nothing to kill. The next step on this line is observation, not
a fix: extend the beat line with `vext/vpur/vinact/vact/vcact/vcmode/lsr` to build the jetsam/compressor
baseline. Round 11 (2026-11-12) did that and closed the display line's copy question in the same boot: the beat
line now carries the seven fields (`vcmode 8` / `vcact 0` is the compressor baseline a future
`vm_compressor=2` comparison boot measures against; `lsr` prices card-medium reads by age), and
**`opi5-gop-mirror=0` made direct scanout the working display path** — zero-copy (`gop 0/158`, every swap
`0 copies`, no `gop blit` anywhere), the 30 s readback saying `window on a compositor surface` every time it
prints, latch mean ~14 ms / longest 17.1 ms / 0 over 50 ms, and the board's first frame-rate measurement:
**50 fps** during the unlock animation. The banding question closed as a phenomenon with it — it is
avoided by direct scanout, and its producers are CPU writes into the buffer the VOP scans (the mirror
copy, the early console's writes and the early paints; the mapping-attribute audit eliminated the
write-combining and dirty-cache candidates for kernel writers, leaving the scanout racing
unsynchronized CPU writes as the remaining conjecture,
[`evidence/round11-result.md`](evidence/round11-result.md)) — while the **white blocks remain and are a
separate defect** (WidgetRenderer rendering an empty surface, the next display-line target). Round 12
(2026-11-12) then signed the mali relay with the owning process and retargeted the sampler at
`WidgetRenderer_D`: every relay line now carries a pid, the degrade/failure path is 100%
`backboardd[57]`, the 332x200 signature printed zero times — so the white-block hypothesis is revised
(conjecture) to a WidgetRenderer surface-content problem, with surface-commit observability as the next
step pending the user's decision ([`evidence/round12-result.md`](evidence/round12-result.md)). Round 13
(2026-11-12) then added the surface census (`opi5-surface-census=1`): the direct path's three surfaces
are all 2560x1440 ARGB with no plain-white layer among them — the lock-screen composites read 24/25
grid points black + 1 white (~1/25 of the panel, the white block's area) and the desktop reads mixed —
so the white content sits **inside the composited frame** (a widget-placeholder layer whose content is
blank; the App Store icon renders on top of the block), with parity at 175 swaps / 0 copies / latch
mean 11.8 ms ([`evidence/round13-result.md`](evidence/round13-result.md)). Round 14 (2026-11-13)
brought the S1/S2 USB code to the board in two tiers and closed the input line's real-device half:
tier 3 verified enumeration (1532:00a6 on ohci1, boot mouse + boot keyboard interfaces, report
protocol parsed), tier 5 verified the mailbox — real reports into `handleReport` with `errors=0`
throughout, the injector coexisting, parity at 2,596 swaps / 0 copies / latch mean 11.6 ms — and
the interaction windows exposed the application-launch layer problem (exit 78 storm + UI-thread
wedge, [`evidence/round14-result.md`](evidence/round14-result.md)).
`porting.toml`
marks firmware, cpu and memory `implemented`, and storage, display and input `device-verified`; round 3's measurement is
[`evidence/round3-result.md`](evidence/round3-result.md), round 2's baseline and root cause is
[`evidence/round2-result.md`](evidence/round2-result.md), round 11's direct-scanout verification is
[`evidence/round11-result.md`](evidence/round11-result.md), round 3's build identity is in
[`evidence/round3-baseline/README.md`](evidence/round3-baseline/README.md), and
[`evidence/HANDOFF.md`](evidence/HANDOFF.md) is the starting point for the next session.

| Repository | Role |
| --- | --- |
| `UniXNU-opi5plus` (this one) | Board configuration, device tree, board patches, board drivers under `package/`, packaging |
| [`UniXNU-rk3588-common`](https://github.com/Ken-u/UniXNU-rk3588-common) | Shared RK3588 SoC support: CPU, GIC, console, PSCI, eMMC personality (pinned in `feeds.toml`) |
| [`UniXNU-evb10`](https://github.com/Ken-u/UniXNU-evb10) | The RK3588 reference port this one is derived from |
| [`Rk3588Emmc`](https://github.com/Ken-u/Rk3588Emmc) | eMMC host and block device — Plan A for storage, if an eMMC module is fitted |
| [`Mali`](https://github.com/Ken-u/Mali) / [`MaliMetal`](https://github.com/Ken-u/MaliMetal) | Mali-G610 kernel driver and Metal stack (M7) |

## Differences from the EVB10 port

This is the whole point of the repository; the UniXNU machinery is identical.

| | RK3588 EVB10 | Orange Pi 5 Plus (this board) |
| --- | --- | --- |
| RAM | 4 GiB in one bank above 4 GiB | **8 GiB in three banks**: `0x0020_0000` (3.83 GiB), `0x1_0000_0000` (4 GiB), `0x2_f000_0000` (256 MiB), read from the board's own device tree. The ADT takes *one contiguous* range, so this port declares the 4 GiB one above 4 GiB and `dram_size = "0x100000000"`; the kernel's owned window is 2 GiB inside the firmware's low bank, which is why the display memory region is placed at `0x1_0000_0000`. |
| Storage | eMMC `mmc@fe2e0000` (`rockchip,rk3588-dwcmshc`), `Rk3588Emmc` driver | **No eMMC, no NVMe.** The only block device is a 30 GB SD card behind `mmc@fe2c0000` (DesignWare MSHC in SD mode). This port ships its own `NativeSd` driver in `package/`: it publishes the OS and SystemOS windows read-only at about 11 MiB/s — measured to be the card's own ceiling (a ~26 MHz CRU clock, 89% PIO efficiency; doubling needs clock-tree work, `evidence/sd-clock-investigation.md`) — and serves the 64 MiB Data seed, which is the writable `/private/var`. |
| Display | DSI panel + VOP2 scan-out (`evb10-vop*`, `Evb10Display`) | **HDMI0 only** (`hdmi@fde80000`), no DSI, no panel. The path is the firmware's GOP frame buffer with the *original* `IOMobileFramebufferLegacy` bound to a forked `GopFramebuffer`, plus `Rk3588VopDirect` for scan-out at 2560x1440. Two kernel passes: `opi5plus_defconfig` and `opi5plus_display_defconfig` (`CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y`). |
| Input | DSI touch panel (`Evb10Touch`, `evb10-touch*`) | **No touch panel.** M6 started with scripted HID injection (verified, round 3) and round 14 added real USB HID over the polled-OHCI mailbox (verified: real keyboard/mouse reports reach `handleReport`, `evidence/round14-result.md`); hot-plug and touch are unmeasured. |
| Console | UART2 `serial@feb50000`, 1.5 Mbaud | Same node, same rate. |
| GPU | Mali-G610 MP4 | Same GPU, same driver (M7). |
| Kernel configs | two passes (`evb10_defconfig`, `evb10_display_defconfig`) | the same two passes, for the same reason: the display archive is built against the first-pass kernel and force-loaded into the second. |

## Contents

| Path | Contents |
| --- | --- |
| `board.toml`, `feeds.toml` | Board configuration and the pinned SoC source |
| `porting.toml` | Porting record: 14 areas, four of them `implemented` and storage, display and input `device-verified`, with the findings gathered so far |
| `configs/opi5plus_defconfig`, `configs/opi5plus_display_defconfig` | The two kernel configurations |
| `dts/opi5plus.dts` | Board device tree on the shared `rk3588.dtsi` |
| `package/` | Board drivers: `NativeSd` (storage), `NativeLegacyDisplay` + `LegacyDisplayProbe` (display), `NativeHidInput` (input) |
| `patches/xnu/series` | Board XNU patch series (mount, exec/exit trace, console, probe thread) |
| `evidence/` | The display documents `porting.toml` references |
| `proprietary-files.txt` | Vendor inputs by SHA-256 |
| `AGENTS.md` | Rules for agents and for anyone copying code from the EVB10 |

## Build

Build with the [UniXNU CLI](https://unixnu.com/building) (Python 3.11 or newer) from a UniXNU checkout:

```sh
./unixnu defconfig opi5plus_defconfig -D ../UniXNU-opi5plus
./unixnu fetch -D ../UniXNU-opi5plus
./unixnu status --json -D ../UniXNU-opi5plus
./unixnu tree -D ../UniXNU-opi5plus
python3 scripts/porting-record.py check --device ../UniXNU-opi5plus
```

`./unixnu build payload -D ../UniXNU-opi5plus --unique-chip-id <hex>` needs a platform seed that is not
stored in any repository. Firmware, boot flow and the serial and reset tooling belong to a
`uefi-opi5plus` repository, which does not exist yet (M1); the board is booted through the vendor loader and
this repository's payload volume.

Two builds are involved, and the display one is built from the first:

```sh
./unixnu defconfig opi5plus_defconfig         -D ../UniXNU-opi5plus
./unixnu build kernel                          -D ../UniXNU-opi5plus
./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
./unixnu build kernel                          -D ../UniXNU-opi5plus
```

`unixnu build kernel` returns 0 even when the display archive fails to compile, so check
`logs/archive-15-build-opi5plus-display.log` for `error:` and the `kernel: <sha256>` line of the build before
trusting a payload. The trap is recorded in `evidence/m5/M5-18` §5 and in `porting.toml`.

## Verification

What was run, and where the output is kept, is in the project's evidence pack (`evidence/m2/summary.md`,
`evidence/m3/`, `evidence/m4/`, `evidence/m5/`): the five commands above, `dtc` on the device tree with the SoC
include path, and the searches behind the boot-argument list. The `dtc` step has no warning.

On the board, the state that matters today is the display and input one, and it is read from the serial log rather
than from the screen: the beat line reports swaps, copies, flips, GOP blits, block I/O and process count, the
display's own line reports `swap ok`, `copy`, `retired`, `frames`, `gop blit` and the vsync counters, and
`evidence/m5/M5-15` through `M5-18` record what each display run proved. [`evidence/round2-result.md`](evidence/round2-result.md)
is the round-2 boot in one page: the numbers against round 1, the root cause and fix, and the three things that
still do not work.
