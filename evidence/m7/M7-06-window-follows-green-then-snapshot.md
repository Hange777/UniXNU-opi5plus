# M7-06 — The scanout follows this port's window: green reached the panel, then the compositor's own frame came back

Device result, 2026-11-10, one boot, three changes on top of M7-05: a readback of the direct path on every report
beat, a one-shot green fill of the boot framebuffer with the window pointed at it, and `rk3588-vop-fence=1`.

## What the run printed

    opi5: display t+940s: vop direct (iommu 1 …): flips 2, surfaces mapped 2 …, latch mean 12738 us longest 15845 us
    opi5: display t+940s: readback: window f2000000, surface pa 1ce01b000 px ff000000 ff000000 ff000000 ff000000,
                          boot px ff00ff00 ff00ff00 ff00ff00 ff00ff00 (window on a compositor surface)
    opi5: display t+940s: fence: 2 of 2 swaps arrived with GPU work not done; 2 flips waited (mean 7621 us,
                          longest 8299 us, 0 gave up)
    opi5: display t+990s: late paint done: the boot framebuffer is green and the window points at it after 2 flips

and, after the third swap arrived:

    opi5: display t+1200s: readback: window f3000000, surface pa 1ccdf3000 px ff000000 ff000000 ff000000 ff000000,
                          boot px ff00ff00 … (window on a compositor surface)

## What the panel did

Reported from the board while the run was up: the Apple logo, **then green for a few seconds, then the Apple logo
again**.

That one sentence settles the question this boot was built for. The green fill is this port writing the boot
framebuffer and pointing the VOP's Esmart2 window at it; the panel turned green, so the window the firmware scans
is the window `evb10_vop_direct_flip()` programs, and `CFG_DONE` (`0x48004`) latches it. The base-address
candidate was already dead (M7-05); the register-latch candidate is dead now too.

Then the third swap arrived, the window moved to another compositor surface (`f3000000`, physical
`0x1ccdf3000`), and the panel went *back to the Apple logo*. The window was demonstrably being scanned — it had
just been green — so the surface it moved to contains the Apple logo. The compositor is scanning out a picture of
the boot screen, not a UI.

## What that means

The display chain is complete end to end: compositor swap accepted → surface mapped through the VOP IOMMU into a
slot → `MST`/`VIR` written and latched → panel scans those pages. Three frames went through it and the panel
followed every one, including the one this port filled by hand.

What is missing is no longer in the display path: it is the content of the frames. SpringBoard's first frames are
the boot screen's own picture, and the userland is not getting past that. The evidence from M7-05 points the same
way — `backboardd` fails 20 of its 39 display calls, all on surface id `0x80000000` with `e00002d5`
(`kIOReturnNoSpace`), and a dozen daemons exit and are restarted every beat.

Two smaller facts the same boot pinned down:

- **The fence matters and works.** `2 of 2 swaps arrived with GPU work not done`, both flips waited (mean 7.6 ms,
  longest 8.3 ms), none gave up. That reproduces the 81% measurement in `package/Mali/docs/notes.zh-CN.md` on this
  board and shows the wait is bounded, not a stall.
- **`gop 0/3` is unchanged**, as predicted: it is `opi5_gop_present()` refusing because `evb10-fb-w`/`evb10-fb-h`
  are not on the command line, not a compositor symptom.

## Where this leaves the port

| question | answer |
| --- | --- |
| does the VOP scan the address this port programs? | yes — the green fill appeared on the panel |
| do the compositor's frames reach the panel? | yes — the third swap replaced the green with its own surface |
| is the panel showing a UI? | no — the surface holds the boot screen's picture |
| what is missing | the userland's own frames: the default surface registration (`0x80000000`, `kIOReturnNoSpace`) and SpringBoard getting past its first frame |

The next step is therefore in the userland path, not the scanout: why surface id `0x80000000` has no place in the
surface-id table, and why the daemons keep exiting. The readback added here is the instrument that will show the
first real UI frame: `px` stops being uniformly `ff000000` at the corner and the boot framebuffer stops being the
only thing that changes.
