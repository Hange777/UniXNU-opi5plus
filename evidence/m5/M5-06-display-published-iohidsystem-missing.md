# M5-06: the display is published and matchable; the missing piece is the rest of the userland graph

Date: 2026-11-07. Follows `M5-05`.

## 1. The boot is now a real iPadOS boot with the display archive linked

Payload `fdc948d6…` (display kernel `1a6e58c4…`, Data RAM disk, `opi5-hid=1`, HDMI attached) runs to the
full daemon set and keeps running (`logs/display19-run.txt`):

```
procs 94 ... beat 910 s, requests 137530, blocks 19945984, failures 0
ps 27(SpringBoard)  ps 26(runningboardd)  ps 85(IOMFB_FDR_Loader)  ps 45(keybagd) ...
```

The 90 s death of `M5-04` is gone; the earlier conclusion that the display archive breaks the boot is retired.

## 2. The instrument that answered it

`nameMatching()` is **IOName** matching (`iokit/Kernel/IOService.cpp`: `nameMatching` sets `IONameMatch`), so the
first registry probe in `M5-05` was asking the wrong question -- it reported "absent" for a service that was in
the registry. The class test is `serviceMatching()` (`IOClass`, which subclasses satisfy), and the reporter now
prints:

```
opi5: display t+90s: registry: class match IOMobileFramebuffer published, IOHIDSystem absent;
our IOName NativeLegacyDisplay IOClass ?, registrations 1, stage 10, busy 0
```

So:

| question | answer |
|---|---|
| did the driver register? | yes, once (`registrations 1`) |
| is the service findable the way userland looks for it? | yes (`class match IOMobileFramebuffer published`) |
| did any client open it? | no (`opened 0`, `busy 0`, `seen 0 clients`) |
| is the input system up? | **no** (`IOHIDSystem absent`) |

## 3. What that means

The framebuffer is not the problem any more: backboardd/SpringBoard could find it and never try. `backboardd`
is the display *and* input server and opens `IOHIDSystem` as part of its start-up; this port has never had an
`IOHIDSystem`, because `evb10_hid_input_start()` is still the inert stub that `M5-03` left:

```cpp
extern "C" void evb10_hid_input_start(IOService *) {}
```

The reference port gets one by building `kits/orangepi5/Evb10Hid/NativeHidInput.cpp` -- an owned `IOHIDDevice`
subclass (330 lines) that attaches to the `IOHIDResource` the family publishes, plus
`package/Evb10HidProbe.cpp` (144 lines) for the properties backboardd's digitizer filter reads. Its
`scripts/build-orangepi5-hid.py` qualifies it against the original `IOHIDFamily` (207-slot abstract
`IOHIDDevice`, byte-identical `__TEXT_EXEC` hash).

## 4. Second confounder found in the same run: the Data volume is too small

The 32 MiB Data RAM disk fills during boot, and from then on `disk3s1` spends the whole run in ENOSPC
(`apfs_fsync_internal: cluster_push() failed with 28`, `free_blocks` in the low hundreds, thousands of
transactions). `logd`, `cfprefsd`, `SpringBoard` cannot write caches or logs. That alone can hold a daemon
at "started but never finished".

Fix in this round: `artifacts/m3/data-seed-64mib.img` (64 MiB, `mkapfs -L OwnerData`), payload
`size_mib = 128` in `board.toml [payload]`, and `tools/boot-uefi.py` now takes the initrd end from the payload's
real size on the board (`fdt chosen 0x50000000 0x58000000` for 128 MiB) and allows 180 s for `load` commands.

The arithmetic was measured, not guessed:

| Data seed | payload | result |
|---|---|---|
| 32 MiB | 96 MiB | boots; ENOSPC storm from about t+200 s |
| 128 MiB | 192 MiB | `DARWINBOOT: boot refused: Out of Resources` (the firmware cannot stage it) |
| 64 MiB | 128 MiB | boots; clean until about t+600 s, then the same ENOSPC storm and `procs` falls 85 → 68 |

So the RAM Data volume only postpones the problem: the system writes more than 64 MiB under `/private/var`
during a full boot (SpringBoard and logd together). A RAM disk cannot be made large enough through the payload,
because DarwinBoot refuses anything past about 128 MiB. The durable fix is the SD-backed Data window that
`M4-10`'s `SHNGROOT` manifest already has room for (a third window next to OS and SystemOS), not a bigger seed.

A payload whose `fdt chosen` range is smaller than the FAT image is silently truncated: with 192 MiB declared as
96 MiB the kernel received `sd=1` and no trust cache, and panicked with `unexpected SIGKILL of launchd`. The
range is computed from the file on the board now.

## 5. State after the 64 MiB Data volume

`logs/display22-run.txt` (payload `885527a5…`, kernel `1a6e58c4…`): the boot is clean and quiet until about
t+600 s (`procs 85` by t+475 s), then `/private/var` fills and the ENOSPC storm returns. The registry line is
unchanged --

```
opi5: display t+480s: registry: class match IOMobileFramebuffer published, IOHIDSystem absent;
our IOName NativeLegacyDisplay IOClass ?, registrations 1, stage 10, busy 0
```

so the Data volume was a real confounder but not the reason nobody opens the framebuffer. `IOHIDSystem` is the
one remaining gate.

## 6. Next

1. Move `/private/var` off the RAM disk: add a Data window to the `SHNGROOT` manifest on the SD card (the OS and
   SystemOS windows are already there, and the card has 11 GiB of `userdata`), so the volume is large and
   persistent and no ENOSPC can stop the boot.
2. Port the HID input device so `IOHIDSystem` exists: `kits/orangepi5/Evb10Hid/NativeHidInput.cpp` (330 lines,
   `IOHIDDevice` subclass attached to the `IOHIDResource`), `package/Evb10HidProbe.cpp` (144 lines, the properties
   backboardd's digitizer filter reads, and the entry point this port currently stubs out), and
   `scripts/build-orangepi5-hid.py` re-qualified against **this** kernel's `IOHIDFamily` addresses (the reference
   script hard-codes the Orange Pi 5 ones).

