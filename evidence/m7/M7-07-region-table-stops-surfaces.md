# M7-07 — An entry in IOSurfaceRoot's region table stops the render server from making surfaces

Device result, 2026-11-10. One boot of the M7-06 payload plus one change: at the default-surface entry point
the port now writes IOSurfaceRoot's region table (the fields `IOSurfaceRoot::start()` would write) and calls
the installer the service keeps at `+0x188`, with the fill on by default for this run.

    kernel  03d601328dfa638db043cb68ee69914258e503e92bb4b4542873944c9b072c3a
    payload 97f44d202d218c5bb6aa75d6f9ba154b4274ae2bc9f5e4d949bbb5d4b703eb7e   (96 MiB)
    log     logs/boot-vramregion-1.log     (t+0 … t+1900, board reset at t+2400 by sd-auto-reboot)

## What the registry model says, checked against the service's own code

The offsets M5-17 recorded are the ones `IOSurfaceRoot::start()` writes, read back from the binary:

    fffffff004ba9374  IORegistryEntry::fromPath("/vram")   ; the only source of the entry
    fffffff004ba93ac  ldr  w8,  [x19, #0x180]              ; count
    fffffff004ba93b8  add  x10, x19, x8, lsl #3
    fffffff004ba93bc  str  x9,  [x10, #0x158]              ; names[i]   = "vram"
    fffffff004ba93c8  str  x9,  [x10, #0x168]              ; aliases[i] = "PurpleGfxMem"
    fffffff004ba93d0  str  w22, [x8,  #0x178]              ; cache word  = 0x100, or 0x400 when the
    fffffff004ba93dc  str  w8,  [x19, #0x180]              ;              inhibit-cache lookup answered
    fffffff004ba93e8  str  x0,  [x19, #0x188]              ; installer = 0x4ba9b60, ALWAYS set
    fffffff004ba93f0  thread_call_allocate/enter           ; start() schedules it asynchronously

`installMemoryRegions()` (`0x4ba9b60`) logs, returns at once when the count is zero, and otherwise walks the
table *contents* — alias, name, cache word — building the region objects. It never looks the node up again,
which is why writing the table by hand can work at all.

## What the board printed

    opi5plus: display: /vram published, 256 MiB at 0x100000000, for IOSurface's region table
    opi5plus: display: IOSurfaceRoot 0xfffffff430e48700 stored at +5632 for the family
    opi5plus: display: region table filled (1 entry vram/PurpleGfxMem), installer 0xfffffff004ba9b60
    opi5plus: display: region count after installer 1
    opi5plus: display: region count settled 1 after installer wait

and the fill ran **twice** (once from the probe's publish call, once from the family's own `start()`), because
the guard that was supposed to skip the second one read `+0x180` from the *family* instance instead of from the
`IOSurfaceRoot` object. Both lines report the same result, so the double write is harmless — but it is a real
bug fixed in the same change.

## What userland did afterwards

    t+680  mali: user: metal: bundle loaded in backboardd (pid 57); plugin in backboardd, accelerator port 0x6d07
    t+725  opi5: display c:   s6#0 0 80000003 0 -> 0 [5820]=0 [5824]=0
    t+1590 opi5: display t+1590s: swap ok 0 rej 0 (reason 0) copy 0 … flips 0
    t+1330 mali: user: status: G commit-to-complete n=0 total=0ms
           mali: user: status: J surface cache n=0 bytes=0 dup=0 us=0
           mali: user: status: vkpipe n=24 failed=0 …            (24 pipelines, so Metal itself works)
           mali: user: status: L lin 0/0/0 sh 0 cp 0 bk 0 pass 0/0

SpringBoard (pid 27), backboardd (57), launchd and runningboardd are alive at t+1200 and none of them exits;
419 other process exits are the usual churn (fairplayd.A2, biomesyncd, biomed, bluetoothd, callservicesd …).
The render server loads its Metal plug-in, opens a framebuffer client, makes **one** selector-6 call, and then
never submits a frame — through t+1590 the family is asked for no swap at all, and the Mali driver's surface
cache never leaves zero bytes.

## The contrast that makes it a regression

M7-06 is the same payload without the region entry:

| | M7-06 (empty table) | this run (one entry) |
| --- | --- | --- |
| Mali surface cache | `n=6 bytes=88473600` | `n=0 bytes=0` |
| GPU submissions | 3 swaps, 3 flips | none |
| family swaps | `swap ok 3` at t+935 … t+1000 | `swap ok 0` through t+1520 |
| panel | boot logo → green → boot logo | boot logo, unchanged |

So an entry whose region has no memory behind it is worse than no entry: surfaces that resolve it stop being
allocated at all. There is no `/vram` node for the installer to attach a range to (M5-18: `fromPath("/vram")`
answers 0 even with the node attached to the registry root), which is exactly the missing half.

## What the tree does now

* the fill is behind `offline-legacy-vram-region`, **default 0**, with the measurement above in the comment;
* the guard reads the service's own `+0x180` and skips a second fill;
* `offline-legacy-default-surface` still gates the family's own creator and is still 0 (it panics, M5-17);
* `offline-legacy-adopt-surface=1` stays on, but it can only fire once a swap is submitted, which needs the
  render server to render — so the adopted-surface route is blocked behind the same stall.

## What this rules out, and what it points at

The default surface is **not** the reason the desktop does not appear. A device that reaches its lock screen —
the EVB10 port — never calls `create_default_fb_surface()` and never touches `+5656` at all. The remaining
question is therefore what the render server is waiting for *before* its first submit, and the next boot
answers it with the thread sampler (M7-08): `opi5plus-bt=SpringBoard`, `opi5plus-bt2=backboardd`,
`opi5plus-bt-secs=60`.

## Reproduce

    wsl -d unixnu -u root bash tools/wsl/tmp-build-vramregion.sh      # the build this run booted
    wsl -d unixnu -u root bash tools/wsl/tmp-payload-datagrow.sh
    python tools\boot-uefi.py --local-payload artifacts\m5\payload-datagrow.img \
        --log logs\boot-vramregion-1.log --tail-secs 1900

The same kernel with `offline-legacy-vram-region=1` is the run recorded above; without the argument the table
stays empty and M7-06's three frames come back.
