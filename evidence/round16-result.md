# Round 16 result — the exit-78 storm is the bluetoothd block-exec retry echo, not an exec failure

Date: 2026-11-13 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
One XNU boot this round, and it was an **unattended forensics run**: the round-18 instrumentation
(the per-exec hook and the xpcproxy syscall watch) rode a detached boot while nobody was at the
panel, the board stayed in XNU overnight by the user's instruction, and the closing task sent the
0x1d that brought Linux back. The main capture (`logs/boot-round16.log`, project working tree,
**not committed** — `logs/**` is gitignored) is the round's instrument; `logs/boot-round16-cap2.log`
is a later re-attach that continued the same run from t+1305. Caliber note (different from the
round-11..14 pages): every line number on this page is the **LF-only count** — the file is split on
`\n`, so the bare-CR fragments merge into the preceding line. The LF-only total is 6,097 lines; the
CR-inclusive (`File.ReadAllLines`) count is 6,931. The excerpts below carry the LF numbers; a
CR-inclusive count drifts ahead of them.

## What the round did, and why

Round 14 exposed an `exit xpcproxy[...] rv 0x4e00 ... code 78` storm (~ EX_CONFIG) next to the UI
wedge and read it, conjecturally, as an application-spawn failure. Round 16 was the forensics step:
name what a dying xpcproxy fails to start. The instrumentation is the round-18 implementation
(uncommitted until this page's commit):

1. **sd-exec=1** (patch 0002's new `opi5plus_trace_exec`, board.toml's new argument): one
   `opi5plus: exec <pid> <path>` line per `execve()` that reaches the hook, so a dying process's
   exec target is named and paired with the exit ring by pid (execve does not change the pid).
2. **xpcproxy joins the syscall watch** (patch 0002): the watch list gains `"xpcproxy"`, execve
   (nr 59) joins the path-registering calls, and the ENOENT filter lets nr-59 through — the
   hypothesis was that the storm's execve ENOENT *is* the finding.
3. **Full frame-pointer chains** (patch 0004): the heartbeat's user chain now ends with
   `| fps` and the frame pointer of every frame it printed, so a stopped chain reads frame by
   frame; `tmp-sym-ubt.py` learned the optional group.
4. **Budget surgery for the two new bytes:** `opi5-surface-census=1`, `opi5-hid-input=2` and
   `opi5plus-bt4=chronod` are commented out (22 + 17 + 21 bytes freed) for `sd-exec=1` (+10);
   the command line went 990 B / 54 arguments -> **940 B / 52 arguments**.

### Changes

| file | change |
| --- | --- |
| `board.toml` | `sd-exec=1` added (:255 area) with the hook comment; `opi5-surface-census=1`, `opi5-hid-input=2`, `opi5plus-bt4=chronod` commented out for headroom (each with the round-18 note and the restore line); header comment now says 52 arguments / 940 bytes |
| `patches/xnu/0002-mount-data-volume.patch` | watch list +`"xpcproxy"`, path-register +`case 59` (execve), the ENOENT filter's `&& slot->nr != 59` exception, comment updates (:67-72, :135-136, :245-252 of the patch) |
| `patches/xnu/0004-thread-sample.patch` | the heartbeat chain's `| fps` frame-pointer list (7 lines) and the header paragraph |
| `tools/wsl/331-patch-xnu-data-mount.py` (root repo) | the generator of the 0002 block above, same three edits |
| `tools/wsl/tmp-sym-ubt.py` (root repo) | CHAIN regex accepts the optional trailing `| fps ...` group; older logs still parse |
| `tools/wsl/pack-payload.sh` | header byte count 53/972 -> **52/940** |

## R0 — build identity

| item | value |
| --- | --- |
| kernel sha256 | `b81136989882f55992978435c3696a4ca349e0d605f407b51c5c87897e7e8c4b` (51,736,576 B; `logs/build-round16.log` last line, re-hashed in the build tree this round) |
| payload | `artifacts/m5/payload-round16.img`, 96 MiB (100,663,296 B), sha256 `12badb7934e57033b5fac23d0aad2aebf98bce11cb67bea3a7cf05d475f2a533` (re-hashed this round) |
| command line | **52 tokens / 940 B** — counted from the launchd echo (:2194): `sd-exec=1` added; `opi5-surface-census`, `opi5-hid-input`, `opi5plus-bt4` gone from the line (FAT + launchd double-verified, :1586 fragment + :2194) |

## S0 — the execution chain, unattended

The board opened the round in XNU from the round-15 overnight sit. The opening 0x1d (reset #1)
brought Linux back; the round-16 payload was pushed over it; SSH rebooted into UEFI (reboot #1);
`boot-uefi.py` captured detached (`--tail-secs 900`, `logs/boot-round16.t0` 10:25:44). The capture
ended **naturally** with the last beat t+1240 (:6094); the board stayed in XNU afterwards with
nobody at the panel, and `boot-round16-cap2.log` re-attached later at beats t+1305-1330. The
capture's own first lines carry the previous session's marker: `iBoot didn't extract panic log
from previous session crash` — the expected consequence of a 0x1d panic reset (no panic log is
saved), recorded here so it is not read as a new fault.

Because the injector was dropped from the command line (see the budget surgery) and nobody touched
the mouse:

* the USB receiver was **enumerated and configured at boot** (`ohci0 device vid 0x1532 pid 0xa6`
  :1819 — this time on ohci0, not ohci1; `HID polling started (keyboard 1, mouse 1)` :1823), and
  every one of the 124 `poll frames=` lines reads `reports=0 errors=0 hid_total=0` — **the
  receiver sat untouched the whole run** [confirmed];
* the lock screen was never passed [confirmed — expected, and recorded as the round's own shape:
  an idle desktop, `procs` flat at 161 after the spawn wave].

## S1 — the forensics core: the storm pairs 1:1 with the bluetoothd exec refusals

### The exec hook works

Every `execve()` that reached the hook printed its line — three in the whole capture:

| line | reading |
| --- | --- |
| :1992 | `opi5plus: exec 1 /usr/appleinternal/sbin/launchd.development` |
| :1995 | `opi5plus: exec 1 /sbin/launchd` |
| :2240 | `opi5plus: exec 7 /System/Library/Filesystems/apfs.fs/apfs_boot_util` |

### The storm, paired

The capture holds **93 `exit xpcproxy[...] code 78` events and 93 `exec of /usr/sbin/bluetoothd
blocked` events** (the boot-args echo :2194 contains the string `block-exec` but is not an event;
counted at event granularity, splitting on CR and LF). In order they form a **strict
`B X B X ...` alternation — refusal first, corpse second, 0 orphans either way**:

| property | reading | verdict |
| --- | --- | --- |
| counts | X = 93, B = 93 | 1:1 |
| order | starts `B` (first refusal :2435), ends `X` (last corpse :6096); zero `XX` / `BB` adjacencies in the whole sequence | strict lock-step |
| pids | 81 -> 373, monotonically rising, no reuse | fresh process per retry |
| cadence | ~1 per 10 s from t+~315 to t+1240 (93 events over ~925 s) | constant |
| cap2 continuation | 3 X / 3 B, again alternating (`B X401 B X409 B X412`), beats t+1305-1330 | the loop kept running unattended |

The start anchor is exact: the first refusal sits between beat t+315 (:2355) and beat t+320
(:2442), the first corpse just after beat t+320 (:2446) — i.e. the loop starts with the desktop
daemon spawn wave itself (`procs` 2 -> 3 at t+310, 108 at t+320).

### The mechanism [confirmed for the observed data]

`launchd`/`xpcproxy` spawn jobs through **posix_spawn(2), not execve(2)** — patch 0006's own
comment recorded this long ago ("an execve-only version never fired"). So:

* the sd-exec hook, which sits on the **execve entry**, never sees a posix_spawn job — three exec
  lines in 1,240 s is the hook working correctly on the only calls that reach it, and it is a
  **coverage fact about the instrument, not silence about the jobs**;
* the 0006 block-exec gate refuses bluetoothd **at the spawn entry** (EPERM); launchd retries
  every ~10 s; xpcproxy exits **EX_CONFIG (78)** — the refusal and the corpse are the same event
  seen twice, which is why they alternate 1:1 with no orphans;
* therefore **the round-14 reading of the storm as an application-spawn failure (exec target
  ENOENT) is falsified for the observed data**: there is no execve ENOENT anywhere under the
  storm — no `sysnum xpcproxy[...] nr 59` line exists in the capture at all (the xpcproxy sysnum
  lines that do exist, :2394-:2434, are early-bootstrap pids 24-45 hitting the pre-existing churn
  nrs 338/550/535/286) — and the pairing names the trigger: the round-4 block-exec gate.
* the xpcproxy watch and the nr-59 ENOENT exception stayed **unexercised** on this boot — the
  instrument was built for the wrong layer; it is kept (it is correct code and costs nothing) and
  the record says so.

### What the storm is, and is not

* It is the **designed self-noise of `opi5plus-block-exec=bluetoothd`**: round 4 turned
  bluetoothd's crash loop into a refusal loop — the job never stops being spawned, it only stops
  reaching code (round 4's own result: 268 corpse lines -> 0). Round 16 shows the other side of
  that trade: the corpse line reappears as xpcproxy's EX_CONFIG instead of bluetoothd's own exit.
* It is **not** the app-launch defect: the cadence is constant and interaction-independent
  (round 15 already measured it unchanged through the user's interaction storm; this round it ran
  identically with nobody at the panel). **The exit-78 layer is removed from the
  application-launch problem list.**
* What round 16 does **not** have: no named launchd `<Error>` line and no `webbookmarksd`
  anywhere (0 occurrences) — round 14's named-line case did not reproduce in this window
  [unobtainable here]; the r14 webbookmarksd/AppOS item stays a separate low-frequency lead.

## S2 — the older layers, unchanged or evolving

| layer | reading |
| --- | --- |
| sysnum churn list | biomed nr 338 errno 14 (:2446), contextstored nr 550 errno 22 (:2607), nr 535 errno 78 (:2671), nr 170 errno 34 (:3296); biomesyncd nr 535 errno 78 (:3860), nr 170 errno 34 (:4014) — the round-1-era daemon churn continues as before |
| keybagd, new named path | `sysnum keybagd[45] nr 475 errno 17 path /var/logs` (:2514) — an EEXIST on the Data volume's log directory |
| nsurlsessiond, named paths | `nr 475 errno 17 path /var/mobile/Library/com.apple.nsurlsessiond/E8C9...` and `nr 136 errno 17 path /var/mobile/tmp/com.apple.nsurlsessiond/` (:4656-:4661), plus `nr 10 errno 1 path /private/var/mobile/Library/com.apple.nsurlsessiond/0A89...` (:4660) — Data-volume permission-class failures (EEXIST / EPERM), a small independent item |
| **thread flood (new)** | `process contextstored[56] crossed thread count high watermark (32), EXC_RESOURCE suppressed` (:5531) — and the same line for `PerfPowerService[73]` (:4494). The daemons open threads past the 32 watermark; contextstored's flood is a new shape of the old churn |
| bluetoothd refusals | 93 block-exec lines, the S1 pairing |

## S3 — parity: a clean idle boot

| counter | reading | verdict |
| --- | --- | --- |
| beats | 5 s to 1240 s, `failures 0` in every beat, `stage 32 (L12 late done)` throughout | ✓ |
| final beat | t+1240: `requests 183117, blocks 27161600, failures 0, procs 161, vfree 288530, vcmode 8, lsr 0` (:6094) | ✓ |
| fence | all 24 fence beat lines read `0 of N swaps arrived with GPU work not done; 0 flips waited (0 us, 0 gave up)` — `mali-sched-wait=2` holds on an idle desktop too | ✓ |
| final display lines | t+1230: `swap ok 4 rej 0 (reason 0), 0 copies, gop 0/4`; `vop direct: flips 4, surfaces mapped 2, latch mean 7642 us longest 15836 us (0 over 50 ms), back to boot 0`; readback `window on a compositor surface` (:5998-:6002) | ✓ (4 swaps in 1,240 s is the idle shape: the desktop never changed) |
| wedge / freeze | none — the kernel beat to the capture's end, procs steady at 161 | ✓ |
| real panic | 0 (the only `panic` matches are boot-arg text and the iBoot previous-session marker) | ✓ |

## S4 — wrap-up

The capture was released after its natural end; cap2 re-attached later. The closing task sent 0x1d
(reset #2 of the round) and verified Linux over SSH:

```
py -3 tools\rssh.py 192.168.1.13 hange **** --cmd "cat /proc/uptime"
30.98 188.19
```

— a fresh boot ~31 s after the reset byte, first try. The board ended the round in Linux.

## Next questions

1. **The bluetoothd noise itself (user decision, recorded not changed this round):** the
   refusal/corpse pair every ~10 s is the block-exec gate's own echo, ~96 pairs over 1,015 s
   (93 + 3 across the two captures). Options: accept it (current default — it costs one line per
   ~10 s and nothing else), or silence the *retry* (a launchd-side throttle is not in our control;
   removing the block-exec entry brings the round-3 crash loop back). No round-16 change.
2. **The real application-launch defects (unchanged, carried):** the CollectionsPoste SIGKILL
   loop (thinner since round 15 but not gone), the UI wedge (the round-14 mach_msg2 thread PCs
   `0x1daf40b58` / `0x1daf46bdc`, the second wedge mechanism round 15 separated from the fence
   layer), and the white blocks (region-level sampling at the user's coordinates is still the
   missing instrument).
3. **The contextstored thread flood (new):** which thread pool grows past the 32 watermark and
   what it waits on — the existing ubt sampler can watch contextstored directly (a `bt` slot is
   free again after the round-18 budget surgery).
4. **webbookmarksd / AppOS:** still a separate low-frequency lead from round 14; not reproduced
   in this window.

## The human-eye items

* **Nobody was at the panel** for this run: the receiver was in place (enumerated at boot) and
  untouched (124 poll lines, `reports=0` throughout), the lock screen was never passed, and the
  desktop stayed idle — that is the round's own design, not a regression.
* The mouse/keyboard feel questions carry forward from round 14 unchanged.

## Verbatim log excerpts

Line numbers are the LF-only caliber (see the header note).

* The launchd command-line echo (:2194; 52 tokens / 940 B, middle elided):

```
com.apple.xpc.launchd|1969-12-31 16:05:34.598122 <Notice>: boot-args = -v serial=1 debug=0x10014e ... sd-data-seed=1 sd=4 sd-root-trust=1 sd-data-ram=1 sd-data-persist=1 sd-ps=300 sd-exit=1 sd-exec=1 sd-syscall=1 ... opi5-gop-mirror=0 sheng-aks=1 offline-key-events=1 opi5-hid=1 opi5-usb=5 mali=8 mali-metal=1 mali-sched-wait=2 opi5plus-bt=SpringBoard opi5plus-bt2=backboardd opi5plus-bt3=WidgetRenderer_D opi5plus-bt-secs=30 opi5plus-block-exec=bluetoothd panic_on_crash_disabled=1
```

* The exec hook's three lines (:1992, :1995, :2240):

```
opi5plus: exec 1 /usr/appleinternal/sbin/launchd.development
opi5plus: exec 1 /sbin/launchd
opi5plus: exec 7 /System/Library/Filesystems/apfs.fs/apfs_boot_util
```

* The pairing's first two events and the anchor beat between them (:2435-:2446; the corpse line
  and the biomed churn line share an LF line — a bare-CR merge, exactly the caliber this page
  warns about):

```
opi5plus: exec of /usr/sbin/bluetoothd blocked (opi5plus-block-exec=bluetoothd)
opi5plus beat 320 s, requests 19528, blocks 6976256, failures 0, procs 108, stage 32 (L12 late done), ...
opi5plus: exit xpcproxy[81] rv 0x4e00 sig 0 code 78 age 4opi5plus: sysnum biomed[43] nr 338 errno 14 path <nopath>
```

* The storm's last event and the final beat (:6094-:6096):

```
opi5plus beat 1240 s, requests 183117, blocks 27161600, failures 0, procs 161, stage 32 (L12 late done), vfree 288530, vspec 290087, vcomp 0, vinact 72351, vact 101797, vext 431783, vpur 1162, vcact 0, vcmode 8, dbavail 286254, lsr 0
opi5plus: exit xpcproxy[373] rv 0x4e00 sig 0 code 78 age 4
```

* The thread-flood lines (:4494, :5531; the second is a bare-CR merge with a storm corpse):

```
process PerfPowerService[73] crossed thread count high watermark (32), EXC_RESOURCE suppressed, internal diagnostics disabled.
process contextstored[56] crossed thread count high watermark (32), EXC_RESOURCE suppressed, internal diagnostics disabled.
```

* The Data-volume permission-class sysnum lines (:2514, :4656-:4661):

```
opi5plus: sysnum keybagd[45] nr 475 errno 17 path /var/logs
opi5plus: sysnum nsurlsessiond[244] nr 475 errno 17 path /var/mobile/Library/com.apple.nsurlsessiond/E8C9768780A371F36651AB699D884E881706B377
opi5plus: sysnum nsurlsessiond[244] nr 136 errno 17 path /var/mobile/tmp/com.apple.nsurlsessiond/
opi5plus: sysnum nsurlsessiond[244] nr 10 errno 1 path /private/var/mobile/Library/com.apple.nsurlsessiond/0A89FC4ACDE771671AAE29D989A1A8F4D51045BC
```

* The final display lines (:5998, :6000, :6002):

```
opi5: display t+1230s: swap ok 4 rej 0 (reason 0) copy 4 (status 0, 14745600 B, 0 copies) retired 4 gen 4/4 frames 0 gop 0/4 timeouts 0 prepare-fail 0, ...
opi5: display t+1230s: vop direct (iommu 1, status 00000019 00000019): flips 4, surfaces mapped 2 (map failures 0, let go 0, remaps 2, zap timeouts 0), latch mean 7642 us longest 15836 us (0 over 50 ms), copy fallbacks 0, back to boot 0
opi5: display t+1230s: fence: 0 of 4 swaps arrived with GPU work not done; 0 flips waited (mean 0 us, longest 0 us, 0 gave up)
```

* The idle USB receiver (:1819, :1823, :2000):

```
opi5: usb: ohci0 device vid 0x1532 pid 0xa6 class 0 mps0 64
opi5: usb: ohci0 HID polling started (keyboard 1, mouse 1)
opi5: usb: poll frames=8642 reports=0 errors=0 hid_total=0 kbd_seq=0 mouse_seq=0 wheel=0 proto=1
```
