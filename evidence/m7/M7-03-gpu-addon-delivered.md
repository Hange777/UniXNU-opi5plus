# M7-03 — The Metal plug-in is delivered and the GPU accelerator registers

Device result, 2026-11-09. The kernel carries patch `0003-gpu-addon.patch`, the payload carries the
`NativeMaliMetal.addon` (11 708 216 B) at `/chosen/memory-map/GpuAddon` and an owner trust bundle with
the bundle's two cdhashes, and the boot arguments carry `mali=8 mali-metal=1`.

## What the serial line shows

    mali: Mali-G610 bring-up, level 8
    mali: GPU_ID 0xa8670005 = arch 10.8.6 product 7 version 0.0 status 5 (Mali-G610)
    mali: clocks now:    sel158 0x1f05 gate66 0x4 (gpll / 6 = 198000 kHz)
    mali: firmware: header 0.2, 274432 bytes, entries end at 880
    mali: 7 sections loaded, 36864 bytes of page tables, root 0x12e800000
    mali: AS0 enabled: transtab 0x12e800000 transcfg 0x420001c6 memattr 0x4c4c4c4c9c4c9f4c
    mali: MCU booted: the global interface raised its interrupt (MCU status 1, job irq raw 0x80000000)
    mali: CSF interface v1.1.0, features 0, 8 groups (stride 0xa0) …
    mali: global interface config + ping ACKNOWLEDGED (req 0x10e ack 0x10e …)
    mali: service "MaliService" registered: userland can open it (IOServiceGetMatchingService)
          sandbox grant: gpu-test,backboardd,iconservicesagen,Preferences,SpringBoard,PosterBoard,
          WidgetRenderer_D,AegirPoster,KaleidoscopePost,CollectionsPoste,PhotosPoster
    mali: metal: AGXAccelerator (an IOAcceleratorES) registered, plugin NativeMaliMetal class
          NativeMaliMetalDevice, sandbox grant backboardd,iconservicesagen,Preferences,SpringBoard,
          PosterBoard,WidgetRenderer_D,AegirPoster,KaleidoscopePost,CollectionsPoste,Pop…

then, from the boot hook:

    opi5plus: /private/var is the Data volume now
    opi5plus: gpu prefs: backboardd.plist (deep framebuffer and compression off) for mobile and root: 0 failure(s)
    opi5plus: addon: 3 file(s): 3 written, 0 already in place, 0 failed: 0
    opi5plus: addon: mounted over /System/Library/Extensions: stage 5 status 0
              (5 = our bundle visible next to the sealed ones)

and the external trust cache modules at their own line:

    attempting to load 3 external trust cache modules
    loaded external trust cache module: 0
    loaded external trust cache module: 1
    loaded external trust cache module: 2

## What each line proves

- **`mali-metal=1` reaches the IOKit plane.** The driver publishes an `AGXAccelerator` (an
  `IOAcceleratorES`) whose plugin class is `NativeMaliMetalDevice`. That is the object the stock Metal
  stack looks for; before this run the driver stopped at `MaliService`.
- **The add-on arrives and is unpacked.** `3 written, 0 failed` is the NGPUADD2 image's three files
  (`Info.plist`, `NativeMaliMetal`, `libvulkan_panfrost.dylib`) written into `/private/var/ngpu/addons`
  and inflated from their zlib streams by the kernel's own zlib.
- **The union mount works.** Stage 5 is the last of the mount's own checks: the mount succeeded, a
  sealed lower entry (`AGXMetalA10.bundle`) is still visible through the union, *and* our bundle's
  `Info.plist` resolves next to it. The sealed system volume is untouched.
- **The trust cache carries our hashes.** Three modules load: the owner module these two cdhashes were
  packed into, and the two original Apple modules unchanged.
- **The render server is told not to ask for 10-bit.** The `backboardd.plist` and global-domain
  properties are in place in both homes' preference domains, which matters because this board's scanout
  takes 8-bit BGRA only.

## What it does not show yet

Whether user space actually opens the accelerator and composes a frame. The log above ends in the
kernel's own boot hook; `backboardd` starts later, and the display probe's client report (which in
earlier runs became non-zero around `t+780 s`) is what will say whether the compositor connected. That
observation belongs to the next run, not to this record.

## How it was built

- `patches/xnu/0003-gpu-addon.patch`: one file (`bsd/kern/kern_exec.c`), two hunks — a call after the
  Data mount and the add-on block at the end of the device-series region. It is the add-on part of the
  EVB10's `evb10-userland-mounts` rewrite, not a vendor: that patch is a whole board's userland and its
  Data half calls `native_emmc_*`, which this board does not have.
- `scripts/build-gpu-trust-bundle.py`: wraps the original static/system trust bundle with one owner
  module (two cdhashes) and checks the result with the same structural rules DarwinBoot applies.
- `board.toml`: `[payload] size_mib` went 128 → 192 (kernel 49 + Data seed 64 + add-on 12 + ADT, trust
  cache and toml came to 124 MiB, too close to 128 for the payload step), and `mali-metal=1` joined
  `mali=8` in `kernel_args`.
