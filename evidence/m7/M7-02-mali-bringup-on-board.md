# M7-02 — The Mali-G610 comes up on this board

Device result, 2026-11-09, with `mali=8` in the payload's kernel arguments and `libMali.a` linked into
the display kernel (`_mali_start` at `0xfffffff007a410cc`).

## What the driver printed, in order

    mali: Mali-G610 bring-up, level 8
    mali: GPU_ID 0xa8670005 = arch 10.8.6 product 7 version 0.0 status 5 (Mali-G610)
    mali: L2 features 0x7120306 core 0 tiler 0x809 mem 0x301 mmu 0x2830 (va 48 bits, pa 40 bits) as present 0xff csf id 0x40a0412
    mali: power domain 12 is on
    mali: clock probe: 4159451 GPU cycles in 21007 us = 198 MHz
    mali: CPU memory: fill 52176 MB/s, copy 22500 MB/s (32 MiB, cached)
    mali: buffer pool: 256 granules of 2 MiB (512 MiB) of 2 MiB aligned physical memory
    mali: firmware: header 0.2, 274432 bytes, entries end at 880
    mali: 7 sections loaded, 36864 bytes of page tables, root 0x12de00000
    mali: AS0 enabled
    mali: MCU booted: the global interface raised its interrupt (MCU status 1)
    mali: CSF interface v1.1.0, features 0, 8 groups (stride 0xa0)
    mali: global interface config + ping ACKNOWLEDGED (req 0x10e ack 0x10e)
    mali: service "MaliService" registered: userland can open it …
          sandbox grant: gpu-test,backboardd,…,SpringBoard,PosterBoard,WidgetRenderer_D,…

That is M0 through M8 of the driver's ladder reached in one run: power, clocks, the GPU_ID register,
the memory model, the firmware in AS0, the MCU, the global interface handshake and the IOKit service.

## Two facts that differ from the EVB10

- **The firmware is a different build.** `proprietary-files.txt` lists this board's copy
  (`ad91511a…`, taken from the BSP image the board itself runs) and it reports CSF header **0.2** with
  the entry list ending at **880**, where the EVB10's `8e7c821a…` build reports 0.3 and 960. The MCU
  accepts it and the interface negotiates, but the driver was developed against the other build, so
  compute and fragment jobs are still unproven on this file.
- **The driver's sandbox grant already names the clients.** `backboardd`, `SpringBoard`,
  `PosterBoard`, `WidgetRenderer_D` and the poster extensions are in the grant list, which is what
  the Metal stack's user space will need once the plug-in can be loaded.

## What it does not show

Nothing renders through the GPU in this run. `MaliService` is registered and openable, but the
user-space half (MaliMetal's `NativeMaliMetal.bundle`) is not in the payload and this port's kernel
has no path for it yet — see M7-01 for the built artifacts and what delivering them needs.

The same run's display probe stayed at `opened 0 … seen 0 clients` through `t+440 s` with 85
processes and no I/O failures; the display clients in earlier runs connected around `t+780 s`, so
this run does not cover that window.
