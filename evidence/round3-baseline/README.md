# Round 3 baseline - the input injector is built

Date: 2026-11-11, after round 2 (`evidence/round2-result.md`). This page is the build identity of round 3 and
the two build-environment facts that cost time getting there. No board run had happened when this page was
written, so nothing here is device evidence.

> Update 2026-11-12: the payload described here did boot. `evidence/round3-result.md` is the board measurement, and
> `porting.toml`'s `input` area is `device-verified` on it. The rest of this page is kept as written so the two
> documents can be read as a sequence.

## Result

* Kernel `0359b168244252f4d992d573f24fc8a82f370bd9f485fd9d0cf991f13e2d1d82`, 51,695,616 bytes. The size is
  round 2's size exactly because the image is page-aligned; the content is not. Round 2's kernel was
  `45fbfab3cae0c66dcf73b85ba08a82ae7a580b0c4e6beabab1d81f76dc0a3893`.
* Payload `artifacts/m5/payload-round3.img` (project working tree, not committed), 100,663,296 bytes, sha256
  `407ac58aa22700a5833f709bea900d8c042e715f735f399f3449228cefd75f13`. The argument list that packed it is
  recorded in `tools/wsl/tmp-payload-r3.sh`.
* Command line 993 bytes / 54 arguments, carrying `opi5-hid-input=2`. That leaves 7 bytes of the 1000-byte
  ceiling, which is why the injector is selected by the value of an argument that already existed instead of
  by a new argument name.
* `xnu-patches.list` is unchanged at
  `3c0af5097e65aae95729e59358b1d6f83c4e2b0146d66b98f4fc53ac7fe2af0b`, byte for byte from round 2: this round
  changed the board package only and no XNU patch.
* Build: 170 s incremental (22:53:30 to 22:56:20 in the log's own timestamps), and the five error counts are
  all zero: kernel.log, archive-*.log, component-*.log, prelink.log and kernel-bootstrap.log.
* Log: `logs/build-round3.log` in the project working tree (not committed). The earlier failed attempt is
  `logs/build-round3-attempt1.log`. Neither log is copied into this file.

## The command line, read back from the payload

The `args` value was read out of `EFI/UniXNU/unixnu.toml` inside `payload-round3.img` by walking the FAT32
directory tree directly (no mtools, and not `tools/wsl/911-verify-payload.py`). The result:

* 54 arguments, 993 bytes when joined with single spaces.
* `opi5-hid-input=2` present; `opi5-hid-input=1` absent.
* first three `-v serial=3 debug=0x10014e`; last five `rk3588-vop-direct=1 rk3588-vop-esmart=2
  rk3588-vop-vp=2 rk3588-vop-fence=1 offline-legacy-paint=0`.
* the volume's `Payloads/kernel.macho` is 51,695,616 bytes and hashes to
  `0359b168244252f4d992d573f24fc8a82f370bd9f485fd9d0cf991f13e2d1d82`, the round-3 kernel, so the payload does
  carry the kernel this round built.

## What this round changed

* `package/Opi5plusHid/NativeHidInput.cpp` - the synthetic gesture injector: `emitTouchReport()` sends report 3
  (one finger at a chosen position) and `emitButtonReport()` report 2 (Menu/Home), walked by a timer for as
  long as the boot lasts - 21 samples 20 ms apart from 80% to 20% of the screen's height, then 5 s quiet -
  with its own counters, a t+ heartbeat and a bounded wait for an `IOHIDEventDriver` client. Injector mode
  also forces the digitizer collection on, so report ID 3 exists in the descriptor it publishes.
* `package/Opi5plusHidProbe.cpp` - `opi5-hid-input` / `evb10-hid-input` now select how far the input path
  goes, through `native_hid_input_mode`: `1` is round 2's behaviour (the HID device only), `>=2` also starts
  the injector. No new argument name.
* `package/LegacyDisplayProbe.cpp` - the 30 s registry line now counts matched `IOHIDEventService` and
  `IOHIDEventDriver` instances; the `IOHIDSystem` field stays only so the line remains comparable with earlier
  boots, and the comment beside it records that the class does not exist in this kernel.

The record itself changed as well: the `IOHIDSystem` gate is retired everywhere it was still cited
(`README.md`, `configs/opi5plus_display_defconfig`, `evidence/HANDOFF.md`, `evidence/round2-result.md`,
`porting.toml`), the `XHCI: Initialize DWC3` / `XhcDriverBindingStart` lines are attributed to the firmware,
and the two early `evidence/m5` files the record cites (`M5-06`, `M5-08`) are now in the repository.

## Two build preconditions that cost time

1. **`startup.o.json` fails in two opposite ways.** A leftover compile record makes the bootstrap pass skip
   and `archive-03` fail, so round 2's rule is to move it aside before a build. Moving it aside is only safe
   when a XNU source actually changes: with no XNU edit clang writes no `-MJ` record, the bootstrap pass finds
   none, and `scripts/unixnu/kernel.py:170-182` re-raises that pass's error because the record is still not a
   file. Round 3 had no XNU change, so the first attempt failed exactly there (kernel unchanged,
   `45fbfab3...`). `touch`ing `tree/.cache/xnu-work/osfmk/kern/startup.c` (content unchanged) rebuilt one
   object, rewrote the record, and the second attempt ran the same bootstrap pass to its usual link failure
   and completed.
2. **The build must run as root in the WSL distro.** `/opt/unixnu-out` is root-owned, so both the kernel and
   the payload steps run through `wsl.exe -d unixnu -u root`.

## Not verified this round

No boot: the injector has never sent a report, the board was not touched, and no statement about input has a
device measurement. The first action of the next session is the one `porting.toml`'s `input` area names:
boot `artifacts/m5/payload-round3.img` and read the injector's own lines together with the
`IOHIDEventService` instance count.
