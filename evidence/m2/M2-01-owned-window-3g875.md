# M2-01 — The owned window: 2 GiB to 3.875 GiB, measured

Device result. The board ran two payloads that differ only in `[payload.adt] owned_memory_size`; the
firmware is the same build in both runs.

## What changed

`board.toml` declares `dram_size = "0x100000000"` (4 GiB) and `owned_memory_size = "0xF8000000"`
(3.875 GiB). The first attempt asked DarwinBoot for the whole 4 GiB and the boot stopped in
`DarwinBoot` with `NO WINDOW … BIG RUN …`; a 2 GiB request booted but took the *low* bank
(`pa=0x5807C000`), which is the 2 GiB the SoC's own firmware descriptors leave usable. The reason the
large request failed was not the ADT: `PcdImageLargeAddressLoad` made the firmware relocate its own
image into the high bank, fragmenting it. With that PCD set to FALSE the high 4 GiB survives the
handoff as one contiguous run, and 3.875 GiB leaves the low bank to the firmware, U-Boot and the
firmware's own allocations.

## The two runs

| | owned = 0x80000000 (2 GiB) | owned = 0xF8000000 (3.875 GiB) |
| --- | --- | --- |
| `pa=` (first-adapted run) | `0x5807C000` (low bank) | `0x10007C000` (high bank) |
| `vm_page_bootstrap` | 483496 free pages | **967425 free pages** |
| compressor | 2687M | **4608M** |
| 4 KiB pages implied | 1.84 GiB | 3.69 GiB |

967425 pages of 4 KiB is 3.69 GiB of free pages inside a 3.875 GiB window, which is the arithmetic
the window predicts once the kernel's own text, data and the boot allocations are taken out.

The 3.875 GiB run then lived to `t+600 s` with `procs 85`, 12.6 million block requests and
`failures 0` before it panicked in launchd (a separate matter: the panic names `pc
0xfffffff00496fdf4`, an address below the kernel text base, and the log prints no full backtrace).

## Why not the full 4 GiB

The upper bank is exactly 4096 MiB. Asking for all of it leaves nothing for the firmware's own
runtime allocations after the handoff, which is what the failed first attempt showed; 3.875 GiB keeps
128 MiB of headroom and still puts the compressor above 4.5 GiB, which is where the swap and the
shared-region cache stop being a constraint.
