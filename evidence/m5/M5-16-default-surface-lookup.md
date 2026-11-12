# M5-16 — Where `default_fb_surface` looks, and the byte that stops the family creating one

**Status:** static reverse engineering of the *original* IOMobileGraphicsFamily inside this port's prelinked
kernel answered the question M5-15 left open, and the answer is not the slot the docs' wording suggested.
The adapter now publishes the surface the user client looks for.

**Input:** `native-owner-mobile-graphics-candidate.macho` (prelinked, 51 MB), component
`com.apple.iokit.IOMobileGraphicsFamily`, `__TEXT` = `0xfffffff0045f5780..0xfffffff0045fee88`,
`__TEXT_EXEC` = `0xfffffff00576c000..0xfffffff0057a2f74`, prelink slide `-0x1000000`. The kext's own
symbol table is stripped (`nsyms = 0`), so every name below is a string or a resolved kernel import.

## 1. The message and its one caller

`"IOMFB: %s: No Default FB Surface found. Return error\n"` is at VA `0xfffffff0045f6acc`; its `%s` is the
neighbouring `"default_fb_surface"` at `0xfffffff0045f6b02`. An ADRP+ADD / ADRP+LDR / pointer-table scan of
every executable section finds exactly one materialisation site, in the file-local helper
**`0xfffffff005777464`**:

```
fffffff0057774ec: adrp x8, 0xfffffff0045f6000
fffffff0057774f0: add  x8, x8, #0xb02     ; "default_fb_surface"
fffffff0057774f8: adrp x0, 0xfffffff0045f6000
fffffff0057774fc: add  x0, x0, #0xacc     ; "IOMFB: %s: No Default FB Surface found..."
fffffff005777500: bl   0xfffffff0057a26f8 ; PLT -> _IOLog
```

The helper is called from the user client (`0xfffffff0057792ac`), not from the family, and returns
`kIOReturnError` (`0xE00002BC`) when the lookup comes up empty.

## 2. The field is +5656, and there is no installer for it

The helper resolves the framebuffer with `safeMetaCast(provider, IOMobileFramebuffer)` (`self+216`), then
calls **vtable slot 174** (`0xfffffff00578cbf8`), a command-gate wrapper that runs **slot 333**
(`0xfffffff00578cca8`) under `fCommandGate` (`+5640`):

```
fffffff00578ccd4: ldr  x8, [x8, #5656]    ; the default surface pointer
fffffff00578cce0: tbnz w8, #0, ...        ; NULL -> return 0 -> kIOReturnError
fffffff00578ccf0: bl   ...                ; IOSurface getter: ldr x0, [x0, #48]
fffffff00578ccfc: tbnz w8, #0, ...        ; 0 -> return 0
...                                        ; layout request from f5808/f5812 (1080x1920 -> 1242x2208)
fffffff00578d098: bl   ...                ; IOSurface::getID: ldr w0, [x0, #16]
```

So the value backboardd gets is an **IOSurface object pointer at `Legacy+5656` (0x1618)** and its 32-bit id
at `IOSurface+16`. Every store to +5656 in the whole candidate is a store of zero: init
(`0xfffffff005787000`), `stop()` (`0xfffffff005788a84`), and slot 333's own teardown
(`0xfffffff00578d038`). On real hardware that field is written by the platform driver, which this port does
not have.

## 3. The family does create a surface — one field away, behind an unset byte

`create_default_fb_surface` is **slot 299** (`0xfffffff005793694`), called unconditionally from `start()`
(slot 86) at `0xfffffff005787f28`. It reads the boot size (`+5808`, `+5812`), builds an OSDictionary with
`"PurpleGfxMem"`, calls an IOSurface-kext routine with the IOSurfaceRoot service it found by name
(`"IOCoreSurfaceRoot"`, stored at `+5632`), and stores the result at **`+5664` (0x1620)**:

```
fffffff0057939cc: str x8, [x9, #5664]     ; this->f5664 = surface
```

It returns early unless the byte at **`+6016` (0x1780)** is set:

```
fffffff005793748: mov  x9, #0x1780
fffffff005793754: ldrb w8, [x8]           ; gate byte
fffffff005793758: tbz  w8, #0, <return>
```

**Nothing in the candidate writes +6016.** That byte belongs to the platform driver as well. So on this
board slot 299 ran, saw zero, and returned; `+5664` stayed NULL, and the lookup at `+5656` never had
anything to find.

## 4. What slot 316 is (and is not)

The docs' wording ("the family's `create_default_fb_surface()` slot returns void") invites reading slot 316
as that slot. It is not. Slot 316 (`0xfffffff00579ea44`) is a 20-byte no-op that stores its bool argument and
returns; it is called twice, always as `slot316(this, false)` immediately before `vtable[290]`
(`kickSwapQueue`), from the jump-table dispatcher at `0xfffffff005795ec0`. Implementing it cannot fix this
failure, and the guard that stands there now is harmless.

## 5. The change this port makes

`package/Opi5plusDisplay/NativeLegacyDisplay.cpp`, `startDisplay()`:

1. write `1` to `+6016` **before** calling the original `start()`, so slot 299 is not gated;
2. after `start()` returns, read `+5664`; if `+5656` is still empty, `retain()` the created surface and store
   it at `+5656`, logging the pointer and the IOSurface id.
3. if `+5664` is NULL the log says so, with both possible reasons (still gated, or no IOSurfaceRoot service).

The extra `retain()` is mandatory rather than cosmetic: another family path at `0xfffffff00579437c` does
`release; this->f5664 = NULL`, and a bare alias would dangle. The library keeps its own reference through
`stop()`, which releases `+5656`.

## 6. Risks, named

| risk | why it matters |
| --- | --- |
| `0x1618` / `0x1620` / `0x1780` are private offsets | any family build change invalidates them silently; the assert on the instance extent (`0x18a0`) does not cover them |
| slot 333 runs under the command gate | the write must happen before any user client can reach the gate, which `start()` guarantees; a later write would need the work loop |
| geometry | slot 333 feeds `+5808`/`+5812` into its layout request, so the surface must match what `installBootDimensions()` publishes (2560x1440 here) |
| lifetime | the reference is ours; releasing it twice, or letting `stop()` release ours, is a use-after-free |

## 7. What is still not established from the binary

Whether `+6016` is what the real platform driver sets or whether it sets `+5656` directly; and whether
`+5664` ends up non-NULL on this board once the gate byte is written. Both are answered by the device log of
the next run, which prints the created pointer and the id, or says that nothing was created.

## 8. The first attempt panicked, and why (2026-11-08, run `display35c`)

Setting the `+6016` byte *before* the original `start()` was wrong, and the board said so:

```
panic(cpu 4 caller 0xfffffff0079f913c): Kernel data abort. at pc 0xfffffff00578d110, lr 0xfffffff00578d110
x0: 0x0            x1: 0xa00  x2: 0x5a0   x9: 0xaaaaaaaa   x16: 0xfffffff004b9ec0c
esr: 0x96000005   far: 0x0
```

`esr 0x96000005` is a translation fault at level 1 with `far 0`, i.e. a NULL dereference; `x1`/`x2` are
2560/1440, this port's boot dimensions; `x9` is XNU's freed-object poison; `x16` is the IOSurface-kext entry
point the creator calls. In words: the creator ran with no IOSurfaceRoot service behind it.

The reason is ordering, and the probe's own log has it: this port loads `com.apple.iokit.IOSurface` from the
probe at stage `L7`, *after* the family's `start()` has already done its own `waitForService("IOCoreSurfaceRoot")`
and stored nothing at `+5632`. With the byte forced on, the creator ran against a NULL service.

Two changes follow, and both are in the tree now:

1. the byte is only written when the family's `+5632` is non-empty, and the backend resolves
   `IOCoreSurfaceRoot` itself (`waitForMatchingService`, 2 s) and stores it at `+5632` when the family lost
   the race — so the creator always has the service it dereferences;
2. `start()` no longer forces the byte at all; the probe asks for the default surface *after* `L9`, through
   `native_legacy_publish_default_surface(display)`, once the kext is loaded and the service answers.

A wrong `+6016` is therefore loud rather than silent on this board, which is worth knowing: the byte is a
switch that turns a missing service into a kernel panic.

## 9. The creator runs and still produces nothing (run `display36`, kernel `461686c7`)

With the gate set *after* `IOCoreSurfaceRoot` exists (the backend resolves the service itself and stores it at
`+5632`), both lines print and the board does not panic inside the creator:

```
opi5plus: display: IOSurfaceRoot 0xfffffff2d4b18300 stored at +5632 for the family
opi5plus: display: create_default_fb_surface() produced no surface (+5664 null) even with IOSurfaceRoot 0xfffffff2d4b18300
```

So the missing service was only half of it: slot 299 runs, talks to the service, and still leaves `+5664`
empty. The dictionary it builds carries the string `PurpleGfxMem`, which on a device names the memory region
the platform driver registered; nothing in this port registers one. The creator therefore has nothing to
allocate from, and the default surface cannot be conjured from the service alone.

Worse, enabling the gate is not free even when the creator returns nothing: a *later* family path reaches
`0xfffffff00578d110` and dereferences NULL with this port's own dimensions in the argument registers
(`x1 0xa00`, `x2 0x5a0`, `esr 0x96000005`, `far 0`, `x9 0xaaaaaaaa`) and the board resets. Two runs
(`display35c`, `display36`) ended with exactly that signature, before userland was up. The gate is therefore
behind `offline-legacy-default-surface=1` and off by default; the honest default is the M5-15 behaviour
(three frames, no panic), not a reset loop.

Next candidate, in order: find what registers the `PurpleGfxMem` region on a device (the platform driver's
`IOPlatformExpert` memory-region call, or the IOSurface kext's region registry), and only then re-enable the
gate; or keep the default surface out of the path entirely and find what the compositor waits for after its
third frame (selector 6 answers `kIOReturnBusy` for `0x80000000` only).
