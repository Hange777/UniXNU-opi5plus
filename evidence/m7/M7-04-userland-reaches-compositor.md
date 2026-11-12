# M7-04 — Userland reaches the compositor; the GPU bundle was not mounted in this run

Device result, 2026-11-09, `sd-gpu-addon=1 sd-gpu-mount=0` (the add-on's files were already on the Data
volume, the union over `/System/Library/Extensions` was skipped on purpose). What this run establishes is
that the graphics bring-up now meets the real compositor, and how far the userland gets when the GPU stack
is installed but not visible.

## What ran

    opi5plus: addon: 3 file(s): 0 written, 3 already in place, 0 failed: 0
    opi5plus: addon: sd-gpu-mount=0, the bundle stays on the Data volume and /System/Library/Extensions is left as it is
    opi5plus: gpu prefs: backboardd.plist (deep framebuffer and compression off) for mobile and root: 0 failure(s)

    t+310 s   procs 3
    t+320 s   procs 106            (userland launched: launchd's children)
    t+695 s   procs 77             (settled)
    t+625 s   display: opened 3 … seen 3 clients, 3 calls
    t+710 s   display: opened 5 … seen 5 clients, 19 calls

    opi5plus: ps 27(SpringBoard) …, ps 57(backboardd) …, ps 85(IOMFB_FDR_Loader) …

SpringBoard, backboardd and the framebuffer loader all start, and five `IOMobileFramebufferUserClient`s
open the display. That is the furthest this port has got: earlier runs never had a compositor to talk to.

## What this run does not establish — no swap was ever submitted

    opi5: display t+710s: swap ok 0 rej 0 … frames 0 … flips 0, surfaces mapped 0

`rej 0` is the whole of it: a swap the display refuses would be counted, as it was in M5-19 (`swap ok 2
rej 0 copy 2`, and the refusals of M5-15 counted the same way). `swap ok 0 rej 0` with `surfaces mapped 0`
means the compositor never submitted a swap and no surface ever reached the display end — the client
connections are real, the drawing is not.

The reason is this run's own switch: with `sd-gpu-mount=0` the add-on's `NativeMaliMetal.bundle` stayed in
`/private/var/ngpu/addons` and never appeared in `/System/Library/Extensions`, so the Metal plug-in the render
server loads was not there to give it a drawable. The kernel side was registered (M7-03), the user side was
not reachable.

For the same reason the one line about the default surface must not be read as this run's failure:

    IOMFB: default_fb_surface: No Default FB Surface found. Return error

It is the original family reporting that its own lookup found nothing, printed by a lookup nothing asked for,
because no swap got far enough to need a surface. It is not evidence that a surface search refuses the
compositor: the reference port is device-verified to SpringBoard (`porting.toml` area `display`, "swap ok 34
rej 0, 33 VOP direct flips") and none of its 22 evidence documents mentions this line or the +5656 field at
all. It is a finding of M5-15/M5-16/M5-17, where swaps *did* complete; it is not a blocker of this run, and
publishing a surface is not what this round sets out to do.

## The run that would show a swap has never been made

The four configurations of this round, by whether the GPU stack was visible to the render server:

| run | add-on | union | Metal visible to userland | result |
| --- | --- | --- | --- | --- |
| A | on | on | yes — the only time | **silent at t+115 s** |
| B | off | — | no | t+385 s, procs 90 |
| C | on | on | yes | t+275 s, then the volume filled |
| D | on | off (this run) | no | t+710 s, procs 106, swap ok 0 |

A is the only run where the compositor could have drawn, and it is the run that wrote the 36 MB bundle into
the Data volume during boot and then went silent — the volume had about 1 MB left. So the configuration that
would answer "does the compositor submit a swap once it can see the plug-in" has not yet been observed with
a Data volume that fits. That, and nothing about surfaces, is what comes next.

## The Data volume: 64 MiB because the seed is 64 MiB

    analyticsd: ENOSPC: ttype INSERT num_blocks 127
    spaceman_fxc_print_stats: disk3 … smfree 277/16384 blocks

The Metal bundle alone is 36 MB on disk (19.4 MB `NativeMaliMetal` + 16.5 MB `libvulkan_panfrost.dylib`).

The 64 MiB is not a driver constant. `NativeSd.cpp` reads the container size out of the seed's own
superblock (`nx_block_size` at +36, `nx_block_count` at +40) and derives the window from it; the constant
that does appear in that file, `kDataMarginBlocks = 131072`, is the gap it insists on between the root image
and the window, and `kDataZeroBlocks = 131072` is how much it zeroes in front of the container when
formatting. What the payload carried was a 64 MiB `mkapfs` image (`tools/wsl/tmp-make-seed64.sh`: `truncate
-s 64M` then `mkapfs -L OwnerData`), and the payload carries it in full: 49 MiB kernel + 64 MiB seed + 12 MiB
add-on is the 125 MiB that made `[payload] size_mib` 192.

So growing the container does not need the driver changed, and it does not need a 2 GiB file in the payload
either. `NativeSd` already accepts a seed smaller than the container — `containerBytes < size` is the check
(`NativeSd.cpp:1266`), and `formatDataWindow` writes the seed and then zeroes up to 64 MiB after it. The
reference port does exactly this on NVMe ("the seed on the payload is only the front of the Data container:
an empty APFS container of 512 MiB has 18 blocks that are not zero").

## The container this round builds

2048 MiB, matching the reference port's `nvme-data-size`, measured against the board's own numbers:

    root image ends at      58204160   (NativeSd: "root ends at 58204160", every run since M4)
    userdata last LBA       62929886   (the card's GPT; [userdata] last_lba 62914559 is the placeholder)
    partBlocks              62897119   = 29.99 GiB
    container               4194304 blocks, + 2048 header = 4196352
    window                  58733535 .. 62929886
    margin to the root end  398303 blocks = 194 MiB   (needs 131072 = 64 MiB)

The front of that container is 9330688 bytes (8.90 MiB) — where `mkapfs` put everything it wrote — so the
payload goes from 192 MiB back to about 96. `containerBytes >= size` holds (2048 MiB against 8.90 MiB), the
container is MiB-aligned, and `header.dataBlocks` changes, so `persistentData()` reformats the window on the
first boot without `sd-data-reset`.

## What this rules out

The hang of the first add-on run (`t+115 s`, nothing written to the serial afterwards) did not reproduce in
either of the two later add-on runs: one with the union mounted and one without, both passing that point and
reaching userland. The only run that hung is the one that wrote the 36 MB into the Data volume during boot,
which is consistent with the full volume rather than with the mount. The mount itself reached stage 5 in
M7-03 and did not by itself stop the boot.
