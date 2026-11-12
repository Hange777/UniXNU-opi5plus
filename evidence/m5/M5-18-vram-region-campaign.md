# M5-18 — The `/vram` campaign: publishing the node, and why IOSurface still has no region

**Status:** the display archive now publishes a `/vram` node and can fill IOSurfaceRoot's region table
directly, but the region count is still zero on the board: `IORegistryEntry::fromPath("/vram")` answers 0 in
both the service and the device-tree planes even with the node attached to the registry root. The last
untried step is the direct table fill plus the service's own installer, which is implemented and built but not
yet read back from the board.

**Runs:** `display42`…`display50`, kernels `9f04adcc`, `b94723ec`, `fc918d93`, `a6629f71`, `6c218c24`.
**Prerequisite reading:** [M5-17](M5-17-display-memory-region-required.md) (what the region is and why a
region-less surface resets the board).

## 1. What was added

| piece | where | what it does |
| --- | --- | --- |
| `Opi5plusVram` | `package/Opi5plusDisplay/NativeLegacyDisplay.cpp` | an `IOService` subclass named `vram`, carrying `reg` = {`0x1_0000_0000`, 256 MiB} and `device_type` = `vram` |
| `opi5plus_publish_vram()` | same file | creates the node, attaches it, starts it; called by the probe at stage `L7`, before `com.apple.iokit.IOSurface` is loaded |
| `opi5plus_fill_vram_table()` | same file | writes IOSurfaceRoot's region table directly (see §4) and calls the installer the service keeps at `+0x188` |
| region guards | `publishSystemMemorySurface()`, `adoptDefaultSurface()` | both read `[surface+0x28]` and refuse to publish a surface whose region is NULL |

The memory range is deliberate: `[payload.adt] owned_memory_size` is 2 GiB **inside the firmware's low bank**,
so the 4 GiB bank at `0x1_0000_0000` is real DRAM the kernel does not own and cannot allocate from. Nothing the
VM does can overlap a display region placed there.

## 2. Where the node ends up

Three placements were tried, each read back with `fromPath` in both planes:

| run | placement | result |
| --- | --- | --- |
| `display42` | `attach(getServiceRoot())` | service root is the platform expert `J120AP` → path `/J120AP/vram`; `installMemoryRegions()` never printed → **region table never filled** |
| `display46` | `attachToParent(getParentEntry(gIOServicePlane))` | still not at `/vram` |
| `display48` | attach in the **device-tree plane** as well | `opi5plus: display: /vram under 'J120AP'; fromPath service 0 tree 0, name 'vram'` |
| `display49` | `attachToParent(IORegistryEntry::getRegistryRoot())` | `fromPath service 0 tree 0` — the same line |

So the entry exists, its name is `vram`, and the registry root we can reach is not the anchor `fromPath` uses
(the header notes that `/aliases` properties also form paths, which is one candidate for what it is doing
instead). That thread is parked; §4 is the way around it.

## 3. What the board says about the region

`display44` is the clean statement of the problem. The display's own `start()` was run again after `/vram`
existed, and it returned success without creating a region:

```
opi5plus: display: /vram published, 256 MiB at 0x100000000, for IOSurface's region table
opi5plus: display: IOSurfaceRoot start() -> 1, region count 0
```

The family's creator then produced a surface with no region and `layout_buffers` faulted at
`0xfffffff00578d110` — the same reset as every earlier attempt ([M5-17](M5-17-display-memory-region-required.md) §4).

## 4. The direct table fill (implemented, not yet read back)

From the IOSurfaceRoot layout in [M5-17](M5-17-display-memory-region-required.md) §4:

```
+0x158 + 8*i   const char *name      ← "vram"        (our static string)
+0x168 + 8*i   const char *alias     ← "PurpleGfxMem"
+0x178 + 4*i   uint32_t   cacheWord  ← 0x100  (0x400 when display-carveout-inhibitcache exists)
+0x180         uint32_t   count      ← 1
+0x188         void (*installMemoryRegions)(IOSurfaceRoot *)
```

`opi5plus_fill_vram_table(root)` writes those five fields and calls `+0x188`. It runs in the
`offline-legacy-default-surface=1` path, immediately before the family's creator, and the surface the creator
returns is published **only** when `[surface+0x28]` is non-NULL.

`offline-legacy-default-surface` values, all behind a boot argument because two of them are known to reset the
board:

| value | behaviour |
| --- | --- |
| `0` (default) | nothing; the family's creator stays gated, the display keeps M5-15's three-frame behaviour |
| `1` | fill the region table, run the installer, gate `+6016`, call the creator, publish only a region-backed surface |
| `2` | gate `+6016` only (reproducibility of the reset) |
| `3` | create a system-memory surface through the creator, publish only if region-backed |

## 5. A build trap worth remembering

`unixnu build kernel` **returns 0 even when the display archive fails to compile**: the step prints
`unixnu: python3 failed (1)` and the caller carries on to the payload step, which then packs the *previous*
kernel. Two runs (`display47`, `display48`) were wasted on a stale kernel before this was noticed. The only
trustworthy checks are:

```
grep -c 'error:' .../logs/archive-15-build-opi5plus-display.log     # must be 0
grep 'kernel: ' <build log>                                          # the sha256 that was linked
strings kernel.macho | grep '<a string only the new code has>'       # proof the edit is in the image
```

Two compile errors are easy to repeat in this file: `gIODeviceTreePlane` does not exist (the symbol is
`gIODTPlane`, declared in `IOKit/IODeviceTreeSupport.h`, which the archive's include set does not carry), and
an `extern "C"` declaration inside a function body is ill-formed — the declaration has to be at file scope,
above the class that calls it.

## 6. State of the tree

- `/vram` publication, the region-table fill, the `IOSurfaceRoot start()` retry and both region guards are in
  `package/Opi5plusDisplay/NativeLegacyDisplay.cpp`; the probe calls the publisher at `L7` and
  `native_legacy_publish_default_surface()` after `L9`.
- Default boot arguments keep the board stable (`default-surface=0`, `adopt-surface=1`); `display43` and
  `display48` each ran to their capture limit with no panic.
- Kernel `6c218c24…` is the build that contains the direct table fill; it is what the next run boots.

## 7. What is still open

1. Read back `region table filled … installer <p>` and `region count` from the `display50`-style run. Count 1
   means the table took; a later `created surface … region <非0>` means the creator can allocate from it.
2. If the installer is a thread-call entry that does not run synchronously, call the service's `start()`
   (`0xfffffff004ba90bc`) instead, which does the fill and the scheduling in the order Apple wrote it.
3. If the creator still comes back with `region 0`, the factory (vtable slot 26 of the singleton at
   `*(0xfffffff005d3ae58)`) is resolving the region by its own path lookup, and the `/vram` placement has to be
   solved after all — §2 lists the placements already excluded.
