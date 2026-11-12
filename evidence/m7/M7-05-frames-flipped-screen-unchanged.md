# M7-05 — The compositor's frames are composed, copied and flipped; the panel still shows the boot logo

Device result, 2026-11-10, 1505 s, the configuration no earlier run reached: a 2048 MiB Data window on the card,
the GPU add-on installed **and** `/System/Library/Extensions` union-mounted (`sd-gpu-addon=1 sd-gpu-mount=1`),
and the driver fix of that same night (see "The panic this run fixed").

## What ran

    NativeSd: data-persist: header v1 state 2, 4194304 blocks, crc 0x03404a27, gen 1
    NativeSd: data-persist: published Data on the card: lba 58735583 + 4194304 blocks (2048 MiB), writable
    opi5plus: Data volume /dev/disk3s1 (device 1,6) goes on /private/var
    spaceman_fxc_print_stats: disk3 … smfree 505763/524288 … 99.56%
    opi5plus: /private/var is the Data volume now
    opi5plus: gpu prefs: backboardd.plist (deep framebuffer and compression off) for mobile and root: 0 failure(s)
    opi5plus: addon: 3 file(s): 0 written, 3 already in place, 0 failed: 0
    opi5plus: addon: mounted over /System/Library/Extensions: stage 5 status 0 (5 = our bundle visible next to the sealed ones)

For 25 minutes the system stayed up: 150 launchd children, SpringBoard (27), backboardd (105), logd (25),
runningboardd (26), and the rest. No panic, no reboot; `sd io 242067 requests 29034112 blocks 0 failures,
uptime 1500 s`.

## The GPU stack is doing work

    mali: user: status: vkpipe n=26 failed=0 total=707ms max=64ms, messages info 0 warning 22, cache hit 0 miss 58
    mali: user: status: K1 t=1000.804s rp1 dr2 kpx3600 cs0 bl0 ctc=16864us gpu=8224us st=8773us
    mali: user: status: K4 t=1014.217s rp1 dr2 kpx3600 cs0 bl0 ctc=10160us gpu=8774us st=1557us
    mali: user: status: J surface cache n=8 bytes=117964800 dup=0 us=19525 max=5654

26 Vulkan pipelines built, 0 failed, 707 ms total; the render server's commits complete in 10–17 ms with about
8.2 ms of GPU time; 8 surfaces of 118 MB are in the Metal side's cache. The plug-in the render server loads is
`NativeMaliMetal`, reached through the union mount.

## The compositor submits, the backend copies, the VOP flips

    opi5: display t+975s: swap ok 2 rej 0 … copy 2 (status 0, 14745600 B, 0 copies) retired 2 gen 2/2 frames 0 gop 0/2
    opi5: display t+990s: swap ok 5 rej 0 … copy 5 (status 0, 14745600 B, 0 copies) retired 5 gen 5/5 frames 0 gop 0/5
    opi5: display t+990s: vop direct (iommu 1, status 00000019 00000019): flips 5, surfaces mapped 2
                       (map failures 0, let go 0, remaps 2, zap timeouts 0), latch mean 6893 us longest 16977 us
    opi5: display t+990s: opened 5 (last pid 55 type 0 status 0 IOMobileFramebufferUserClient) seen 5 clients, 43 calls

`rej 0` with `swap ok 5` is the point: five swaps were **accepted**, five full frames of 14,745,600 bytes
(2560 × 1440 × 4) were copied with status 0, and five VOP flips latched (mean 6.9 ms, worst 17 ms). This is the
first run where the original family's swap protocol, this port's copy and the VOP's latch all complete with the
Mali stack live in userland.

Then it stops at five, because a static UI draws once: nothing on the screen changes afterwards, so nothing
re-submits.

## What the screen shows: the boot logo, not those five frames

Asked on the board at the end of the run: the HDMI output still shows the Apple logo the firmware drew. So the
five flipped frames are not what the panel scans out.

The first candidate — that this port programs a different base than the firmware — is **dead**, and the run's own
logs disprove it. Every boot has one firmware line and one `DARWINBOOT:` line, and inside each boot they are equal:

| log | `VOP VP2 enable Esmart2 … addr[…]` | `DARWINBOOT: video base=` |
| --- | --- | --- |
| boot-datagrow-1.log:1350 / :1559 | `0xE6730000` | `0xE6730000` |
| boot-datagrow-3.log:1350 / :1559 | `0xE6740000` | `0xE6740000` |
| boot-datagrow-4.log:1350 / :1559 | `0xE6740000` | `0xE6740000` |
| boot-datagrow-4-full.log:1351 / :1539 | `0xE6740000` | `0xE6740000` |

The base moves between boots because UEFI allocates the frame buffer each time (`0xE6760000`, `0xE5F60000`,
`0xE7440000` appear in older logs), and `NativeLegacyDisplay.cpp:654/:689/:928` validates it against
`PE_current_console()` rather than pinning it. Comparing run A's firmware line with run B's kernel line is not a
mismatch, it is two different boots.

What remains, and what this run cannot separate:

- **The composited content.** `sel6 args 1 80000000 0` and three like it answer `e00002d5`, which is
  `kIOReturnNoSpace` (`0x2d3` would be `Busy`): the surface-id table had no place for surface `0x80000000` — the
  default surface of M5-15/M5-16 — while `0x80000001` and `0x80000003` succeed. `backboardd` (pid 105) fails 20 of
  its 39 display calls, all on that id. Frames that flipped may be a blank or a stale surface.
- **The register latch.** `evb10_vop_direct_flip()` writes the window's `MST` (`0x1c14` for Esmart2) and then
  `CFG_DONE` (`0x48004`); `evb10_vop_direct_latched()` reads `MST` back and compares it with what was written. A
  shadow register that never loads would read back correctly and still leave the panel on the old buffer, so
  `flips 5` does not by itself prove the scanout moved.
- **The fence.** `rk3588-vop-fence` was not in this run's arguments and the `fence:` line never appeared.
  `package/Mali/docs/notes.zh-CN.md` (2026-10-29, tear1/up1…up4) measured what that costs: 108 of 133 swaps (81%)
  arrived while backboardd's GPU work was still running, so a zero-copy flip can show a half-drawn frame. It does
  not explain an unchanged logo, but it must be on in the same run, or a readback cannot be told apart from a
  frame that was not finished yet.

One more number the run does explain, and it is not a compositor symptom: `gop 0/5`. `opi5_gop_present()` needs
`evb10_fb_kva`, `opi5_gop_pixels`, `evb10_fb_w` and `evb10_fb_h`, which `opi5_fb_override()` sets only when
`evb10-fb-w` and `evb10-fb-h` are on the command line (`LegacyDisplayProbe.cpp:717`, gate at `:768-771`), and
neither is. So the kernel's CPU path into the scanned buffer has never written a pixel — everything seen on HDMI so
far is firmware-drawn. That widens the explanation space rather than narrowing it: not only may the compositor's
frames not be scanned, no kernel-side frame ever has been.

The next experiment therefore does not rely on "does the panel change colour" alone. It reads back, in one boot:
the IOVA the window was told to scan, the surface's first physical segment, the surface's first four pixels, and
the boot framebuffer's first four pixels — plus a one-shot green fill of the boot framebuffer with the window
pointed at it, taken once the flips have stopped moving, and `rk3588-vop-fence=1` in the same run
(evidence/m7/M7-06).

## The panic this run fixed

The run before this one (2 GiB window, union mounted, 660 s) died in `logd_helper`:

    disk3: I/O error.
    IOBlockStorageDriver[IOBlockStorageDriver]; executeRequest: request failed to start!
    panic(cpu 2 …): Kernel data abort. at pc 0xfffffff00496fdf4 … far: 0x00000000000000a8
    Panicked task …: pid 89: logd_helper

Disassembling the prelinked kernel at that address (`__PLK_TEXT_EXEC`, `ldr x8, [x8, #168]` on a null object)
shows the crash is in Apple's storage stack, in its error path after our block device refused a request. The
refusal was ours: `doAsyncReadWrite` answered `kIOReturnBadArgument` for a zero-length request, which the
storage stack issues during a write burst. Three changes went in, and this run is their verification:

- A zero-length request completes with success and 0 bytes instead of being refused.
- Writes go through the shared 64 KiB bounce buffer in chunks, like reads, instead of allocating a temporary
  buffer of the whole request (`IOMallocData(bytes)`, the first thing to fail under load).
- A refused request is logged once per reason (lba, count, window size, descriptor length, direction), and a
  failed write is retried once after a flush.

`0 failures` over 242,067 requests and 29,034,112 blocks, and 25 minutes up with the same write load, is the
verification.

## Three facts this run pinned down while it was being read

**The output is HDMI1, not HDMI0.** `DetectDisplays: HDMI0 status: Not Found`, `HDMI1 status: Success`,
`Primary: HDMI1`, and the firmware sets up `[INIT]VOP update mode to: 2560x1440p60, type: HDMI1 for VP2`. VP2
carries exactly one enabled plane, `Esmart2`, which is the window `rk3588-vop-vp=2 rk3588-vop-esmart=2` programs,
so there is no z-order ambiguity to chase. `docs/hardware.md`, `plan.md` and `AGENTS.md` all say "HDMI0 only" and
are wrong.

**R2b's memory window is in force.** The zone dump shows `kernel pa 0x10007C000`, `args pa 0x104250000`,
`window 0xF8000000 bytes`: the kernel runs in the upper bank with 3.875 GiB, as the reference port does, and the
VOP IOMMU's status words went from `00000019 00000011` to `00000019 00000019`, the upstream value. One
documentation inconsistency is left: `board.toml` still has `dram_size = "0x100000000"` (4 GiB) against
`owned_memory_size = "0xF8000000"` and the DTS's `reg = <1 0 1 0>`; the reference port carries `0xF8000000` in all
three. It boots, but `devicetree.py` reconciles `dram_size` against the hardware DTS and the three should agree.

**`e00002d5` is `kIOReturnNoSpace`, not `Busy` and not `NotReady`.** The general table ends at `0xe00002ea`
(`kIOReturnInvalid`); `Busy` is `0x2d3`, `NotReady` is `0x2d5` only if one reads the vendor table, which this is
not. The name matters: "no space in the table" points at the surface-id → slot registration of the default
surface, not at contention.
