# M5-17 — The default surface needs a display memory region, and this port has none

**Status:** static reverse engineering of the original IOMobileGraphicsFamily and IOSurface inside this port's
prelinked kernel, plus four board runs, settle what `default_fb_surface` needs. It is not a slot to
implement and not a surface to hand over: it is a **memory region named `PurpleGfxMem`**, which on a device
is the `/vram` node the platform driver publishes. Without it every surface the family lays out faults.

**Input:** `native-owner-mobile-graphics-candidate.macho` (prelinked, 51 MB; slide `-0x1000000`), the prelink
plan beside it, and `logs/display35c…40`. Kext symbol tables are stripped (zero symbols in the 0x4…–0x6…
range), so every name below comes from a string or a resolved kernel import.

## 1. `create_default_fb_surface` (slot 299, `0xfffffff005793694`)

Called from `start()` (`0xfffffff005787f28 ldr x8,[x8,#2392]` / `blr`). It is `void`; it stores what it
creates at `+5664` and **never** writes NULL there.

```
0x5793748 gate:  ldrb w8,[this+0x1780] ; tbz -> return
0x579375c/0x5793770: height == 0 or width == 0 -> return
0x579378c OSDictionary::withCapacity(1)
0x579379c OSString::withCString("PurpleGfxMem")
0x57937cc x1 = *"IOSurfaceMemoryRegion"        ; the key string, imported from IOSurface
0x57937d8 dict->setObject("IOSurfaceMemoryRegion", "PurpleGfxMem")
0x57937f8 x0 = [this+5632]   ; the IOCoreSurfaceRoot service cached by start()
0x5793804 bl 0x4baaa24       ; IOSurface *f(root, 0, props)
0x5793870 creator == NULL -> return
0x5793990 bl 0x579d0c0       ; layout_buffers(surface, [this+5808], [this+5812], &params)
0x579399c layout == 0 -> return
0x57939b0 getMemoryRegion(surface) -> 0x4b9ec0c (ldr x0,[x0,#40])
0x57939cc str x8,[this+5664]  ; the only store to +5664 in the whole family
```

So the dictionary asks IOSurface for a surface **in the region named `PurpleGfxMem`**, and the family then
lays it out and stores it. `+5664` stays empty whenever any of those steps fails.

## 2. The creator (`0xfffffff004baaa24`, IOSurface)

```
0x4baaa24 mov x3,#0 ; b 0x4baa9d0        ; the three-argument form the family uses
0x4baa9d0 … bl 0x4baaa2c                ; real body: new IOSurface + IOSurface::initWithProperties
0x4baa9ec ldr w2,[surface+0x10]          ; handle
0x4baa9f8 bl 0x4baab10                  ; install it in the root's handle array
```
Convention: `IOSurface *f(IOSurfaceRoot *root, void *unused, OSDictionary *props, void *unused2)` returning
a retained surface or NULL. `IOSurface::initWithProperties` (`0x4b99720`) reads about 35 `IOSurface*` keys
and **never** reads `IOSurfaceMemoryRegion` itself.

## 3. `PurpleGfxMem` is the `/vram` region's other name

| address | where | role |
| --- | --- | --- |
| `0xfffffff0045fd010` | family `__TEXT` | dictionary **value** built by slot 299 |
| `0xfffffff00431132e` | IOSurface `__TEXT` | **alias** of the region entry `{name "vram", alias "PurpleGfxMem"}` in `IOSurfaceRoot::start()` (`0x4ba90bc`) |
| `0xfffffff006b30f98` | family `__DATA` | dead pointer, nothing reads it |

`IOSurfaceRoot::start()` fills that entry only if `IORegistryEntry::fromPath("/vram")` finds a node:

```
0x4ba9374 IORegistryEntry::fromPath("/vram") ; 0x4ba938c cbz -> skip
0x4ba93b0 store name "vram" ; 0x4ba93c0 store alias "PurpleGfxMem" ; count++ at [root+384]
0x4ba93f0 ./0x4ba93f8 thread_call_allocate/enter -> installMemoryRegions() (0x4ba9b60)
```

`installMemoryRegions()` logs `IOSurfaceRoot::installMemoryRegions()` and returns immediately when the count
is 0. There is no `(name, base, size)` API anywhere in the image: the **only** route to a region is the
`/vram` node.

## 4. Why every attempt reset the board

`layout_buffers` (`0xfffffff00578d0c0`) does, as its fifth instruction:

```
0x579d108 ldr x0,[x8,#40]          ; surface->region  (IOSurface+0x28)
0x579d10c bl  0x4b9ec0c            ; getMemoryRegion
0x579d110 ldr  x8,[x0]             ; <-- faults when the region is NULL
0x579d114 ldr  x8,[x8,#176] ; br   ; region vtable slot 22
```

Both callers of `layout_buffers` (slot 299 at `0x5793990`, slot 333 at `0x5798cea0`) pass a surface that
came from IOSurface, and a surface created with no region has `+0x28 == NULL`. Hence:

| run | what was tried | result |
| --- | --- | --- |
| `display35c` | gate `+6016` set before `start()`, no service | panic at `0x579d110` |
| `display36` | gate set after the service exists, creator called | creator produced nothing, then panic |
| `display39` | gate set, creator not called | **still** panic: the gate alone lets the family create a region-less surface |
| `display40` | no gate; own surface via the creator with system-memory keys | surface created (id 1, descriptor set) but `region 0`; not published; **still** panic |

`display40`'s log line is the decisive one:

```
opi5plus: display: created surface 0xfffffff63699afa8 id 1 region 0 descriptor 0xfffffff29d6aa268 for 2560x1440
opi5plus: display: that surface has no memory region, so layout_buffers() would fault on it; not publishing
```

So an IOSurface made without the region key gets a memory descriptor but **no region object**, and merely
*creating* it is enough to make the family lay it out and fault.

## 5. What the port does now

| boot argument | default | effect |
| --- | --- | --- |
| `offline-legacy-default-surface=0` | 0 | nothing; the family's creator stays gated and `default_fb_surface` keeps answering `kIOReturnError` (the M5-15 state: three compositor frames, no panic) |
| `=1` | | gate `+6016` set, creator called (panics: kept for reproducibility) |
| `=2` | | gate set, creator not called (panics: kept for reproducibility) |
| `=3` | | create a system-memory surface through the creator, publish only if `[surface+0x28]` is non-NULL (measured NULL, so it does not publish) |
| `offline-legacy-adopt-surface=1` | 0 | adopt the first compositor surface the display is asked to present as the default surface, again only when its `[surface+0x28]` is non-NULL |

Two guards are permanent and independent of these arguments: `publishSystemMemorySurface()` and
`adoptDefaultSurface()` both read `[surface+0x28]` and refuse to publish a surface without a region. That
turns a reset loop into a log line.

## 6. What would actually unlock the desktop

A `/vram` node in the IORegistry before `IOSurfaceRoot::start()` runs, whose memory the runtime region
factory (vtable slot 26 of the singleton at `*(0xfffffff005d3ae58)`, built at run time) can map. The ADT can
only publish nodes under `/arm-io/` (`devicetree.py` writes `cpus`, `psci`, `arm-io`, `chosen`), and
`fromPath("/vram")` wants a *root* child named `vram`, so the node has to come from a platform driver that
publishes an IOService named `vram`, or from a change to the ADT generator so that a root-level node can be
declared.

Until that exists, the honest default is `display37b`'s behaviour: three compositor frames at 2560x1440,
swap completed, no panic, and `default_fb_surface` unanswered.

## 7. Reproduce

```
# stable (no default surface)
offline-legacy-default-surface=0 offline-legacy-adopt-surface=0

# the four reset runs, in order
offline-legacy-default-surface=1   # display35c/36
offline-legacy-default-surface=2   # display39
offline-legacy-default-surface=3   # display40
```

Kernel hashes: `display39` `bb5e07ff…`, `display40` `4cf4aed8…`, `display41` `7dd30e17…`.

## 8. `/vram` is published now, and IOSurface still has no region (run `display42`, kernel `9f04adcc`)

The display archive publishes the node itself — a plain `IOService` named `vram` under the service root,
carrying a 256 MiB range at `0x1_0000_0000` (the 4 GiB DRAM bank the kernel does *not* own: `owned_memory_size`
is 2 GiB inside the firmware's low bank), and the probe calls it before it asks for the IOSurface kext:

```
opi5plus: display: /vram published, 256 MiB at 0x100000000, for IOSurface's region table
opi5plus: display: IOSurfaceRoot 0xfffffff6aa2fc700 stored at +5632 for the family
opi5plus: display: create_default_fb_surface() produced no surface (+5664 null) even with IOSurfaceRoot …
```

`IOSurfaceRoot::installMemoryRegions()` never prints — not once in the whole boot. Per §4 that line is
unconditional at the head of the function, so the function never ran, so **`IOSurfaceRoot::start()` never ran**:
the service exists (the family holds a pointer) but its region table was never filled. That is why `/vram`
alone changed nothing and why the gate still ends at `0xfffffff00578d110` — the creator found no region,
produced a region-less surface, and `layout_buffers` faulted on it.

So the remaining step is not publishing the node, it is making IOSurfaceRoot build its region table **after**
the node exists: fill `+0x158 + 8i` (name "vram"), `+0x168 + 8i` (alias "PurpleGfxMem"), `+0x178 + 4i`
(cache word) with the count at `+0x180`, then call the region installer the service keeps at `+0x188`
(`0xfffffff004ba9b60`), or restart the service's `start()` (`0xfffffff004ba90bc`) now that `/vram` answers.

The stable default stays `offline-legacy-default-surface=0`; `/vram` publication is kept because it is a
prerequisite and is harmless on its own (run `display42` panicked only once the gate was set).
