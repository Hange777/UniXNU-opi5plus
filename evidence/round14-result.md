# Round 14 result — real keyboard and mouse reach handleReport through the OHCI mailbox

Date: 2026-11-13 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
Two XNU boots this round: the tier-3 (enumeration) payload and the tier-5 (poll) payload, one
0x1d reset between them. The tier-5 capture (`logs/boot-round14-usb5.log`, project working tree,
**not committed** — `logs/**` is gitignored) is the round's main instrument; the tier-3 capture
is `logs/boot-round14-usb3.log`. Caliber note (same convention as the round-11/12/13 pages):
every line number on this page is the **CR-inclusive count, the Select-String caliber** (the two
round-14 logs carry interleaved bare-CR fragments, 1,273 of them in the usb5 log); a 1-based
LF-only count (ripgrep) drifts behind — the excerpts below carry both numbers for the key lines
(e.g. the final consumer pair is CR-inclusive 13535-13536 = LF 12289-12290; the last beat is
CR-inclusive 13938 = LF 12667; the capture's final line is CR-inclusive 13942 = LF 12669).

## What the round did, and why

The S1/S2 USB code (the board copy of the upstream polled-OHCI input path, committed at
`800f9e5`) had never run on the board. This round brought it up **in two staged tiers**, one
payload per tier, exactly as `board.toml`'s `opi5-usb` comment (:361-370; parser
`package/Opi5plusUsb/NativeUsb.cpp:821`, tier ladder in the comment at :815-817) defines them:

1. **S0 — the topology closure, on Linux.** Before pushing anything: on the running Linux the
   two USB 2.0 Type-A ports were confirmed to sit on the two OHCI chains (`fc840000` /
   `fc8c0000`) **directly, with no hub between port and controller**, and the GL3523 hub's real
   vendor ID is **05e3** — the 05a3 recorded in earlier notes was a typo, corrected here; the
   hub hangs on the xHCI chain, which tier 5 never starts, so it is irrelevant to this round.
   This matters because the driver refuses anything behind a hub or in an xHCI port
   (`NativeUsb.cpp:691`, "a hub: not supported") — the keyboard/mouse must be in the two
   USB 2.0 Type-A ports. [confirmed, on Linux]
2. **Tier 3** (`opi5-usb=3`): enumeration only — OHCIs started, ports powered, descriptors read,
   **no reports**. Verified on the board with the Razer Viper V2 Pro receiver in a USB 2.0 port.
3. **Tier 5** (`opi5-usb=5`): everything tier 3 does, plus boot-configure the keyboard/mouse and
   publish reports on the `native_usb_*` mailbox that `package/Opi5plusHid/NativeHidInput.cpp`
   consumes. This is the round's payload: the user drove the panel with a real mouse and keyboard.
4. **Nothing else changed.** No kernel source change this round (S1/S2 are already in `800f9e5`);
   the only working-tree edits the round leaves are this page, `porting.toml` (input findings +
   the device evidence entry), `HANDOFF.md`/`README.md` status text, and a header-comment byte
   count in `tools/wsl/pack-payload.sh`.

### Changes

| file | change |
| --- | --- |
| `board.toml` | (committed in `800f9e5`) the `opi5-usb` group with the tier-ladder and hub-semantics comment (:361-369) and `"opi5-usb=5"` (:370; the tier-3 boot used the same line temporarily edited to `=3` for that push) |
| `package/Opi5plusUsb/NativeUsb.cpp`, `package/Opi5plusHid/*` | no change this round — S1/S2 source as committed at `800f9e5` |

The command line is **972 B / 53 tokens** (round 13: 961 B / 52) — the only addition is
`opi5-usb=5 ` (+11 B); `opi5-surface-census=1` stays on the line (FAT read-back + the launchd
echo, CR-inclusive 2446 = LF 2319, double-verified).

## R0 — build identity

| item | value |
| --- | --- |
| kernel sha256 | `8463916c3426b570af4edfaa17105d1e06fb79ce001b998c045095c6dce048ea` (51,736,576 B; `logs/build-round14.log`) |
| payload, tier 3 | `artifacts/m5/payload-round14-usb3.img`, 96 MiB (100,663,296 B), sha256 `fdd3eabb7316ff8c3faee42ba32ed75356303af69eecb4074d04f63a4b6f0f35` |
| payload, tier 5 | `artifacts/m5/payload-round14-usb5.img`, 96 MiB (100,663,296 B), sha256 `a77a50f7b3de39f795d5ea1709f4af5fa40c6e3e0d2b21a77eb48d79bca45c67` (overwrote the tier-3 volume for the second boot) |

Both payloads pack the **same kernel**; the tiers differ only in the FAT volume's boot-argument
line.

## S1 — tier 3: the receiver enumerates

Criterion: with no host stack anywhere in the prelink, this port's own polled-OHCI driver alone
brings up the chain, powers the port and reads the device. Every criterion below is from
`logs/boot-round14-usb3.log`:

| criterion | reading | verdict |
| --- | --- | --- |
| OHCIs started, port power | `VBUS_EN GPIO3_B7 high (dr_l 0x8800 ddr_l 0x9840)` (:1748) | ✓ |
| DMA pools, no conflict | `ohci0 dma pool at bus 0x62000000 (bounce, non-cacheable)` (:1786), `ohci1 ... 0x62010000` (:1795) — two disjoint 64 KiB pools | ✓ |
| device on ohci1 port 0 | `full speed device` + `device vid 0x1532 pid 0xa6 class 0 mps0 64` (:3297-:3298) — the Razer Viper V2 Pro receiver | ✓ |
| interfaces | `boot keyboard yes (iface 1 ep 2 mps 16), boot mouse yes (iface 0 ep 1 mps 16)` (:3299) — boot mouse iface0 + boot kbd iface1 + the vendor iface2 | ✓ |
| mouse report protocol | descriptor 94 B (:3300); `id -1, 8 bytes, buttons@0/3 x@32/16 y@48/16 wheel@24/8` (:3301) | ✓ |
| no hub refusal | 0 "a hub: not supported" lines — the device is on the OHCI chain, as S0 predicted | ✓ |
| ~2 s re-enumeration | three enumeration sets ~2 s apart (:3297, :3302, :3316) — expected in tier 3: nothing drains the mailbox, so the driver re-reads the descriptors (source read, `NativeUsb.cpp:694/:751`) [confirmed by source] | ✓ |
| reports | none — `HID polling started` never prints, no `poll frames=` line anywhere (tier 3 is enumeration-only, `board.toml` :362-364) | ✓ (by design) |
| panic | 0 | ✓ |

The boot itself was unremarkable: at its last beat (t+905, :5684) the display counters read
swap ok 2 / rej 0 / 0 copies / gop 0/2, fence 2 of 2 waited (mean 6,922 us, longest 7,595 us,
0 gave up), procs 119, `lsr 0`.

## S2 — tier 5: real reports reach handleReport

Criterion: the mailbox hands real HID reports to the existing consumer and the UI moves. All
readings from `logs/boot-round14-usb5.log`:

| criterion | reading | verdict |
| --- | --- | --- |
| polling starts | `opi5: usb: ohci1 HID polling started (keyboard 1, mouse 1)` (:1829) | ✓ |
| enumeration at boot | the receiver enumerates right after start, without waiting for a hot-plug | ✓ |
| heartbeat health | 193 `poll frames=` lines, **errors=0 in every one**, to the capture's end (:13935) | ✓ |
| interaction | the operator's 4 sessions produced **24 poll windows with non-zero reports**; the first window took **1,998 reports** (:3601), the largest took **3,701** (:12528); wheel 0→4 (:4706); kbd_seq 2→4→6→8 (:7757, :10568, :12970) | ✓ |
| terminal kernel counters | `hid_total=32161 kbd_seq=8 mouse_seq=32153 wheel=4 proto=1` (:13540, :13935) — and `hid_total = kbd_seq + mouse_seq` exactly | ✓ |
| consumer side | 36 `opi5: hid: mouse:` report pairs, one per burst window; final `6154 reports to handleReport, last result 0x0` / `USB reports handed on 6154, with motion 6067, sum |dx| 125188 |dy| 102998` (:13535-:13536 = LF 12289-12290) | ✓ — **real input data reaches the userland's HID consumer** |
| 8 ms aggregation | `hid_total` (32,161) is ~5x the handed count (6,154): a burst arrives between consumer ticks and each tick flushes what accumulated (`NativeHidInput.cpp:529-536`, source read) [confirmed by source] | ✓ |
| injector coexistence | the synthetic injector ran the whole capture alongside real input: final `gesture 347 ... reports 7981, fails 0, last result 0` (:13933) — no interference in either direction | ✓ |
| panic | 0 | ✓ |
| hot-plug | not measured — the operator did not hot-plug during tier 5 [unobtainable this round] | — |

## S3 — parity at the final beats (t+1890/1920)

| counter | round 14 (tier 5) | round 13 | verdict |
| --- | --- | --- | --- |
| swap ok (final) | **2,596**, rej 0 (reason 0) (:13778) | 175, rej 0 | both 0 refused; totals differ (this boot had a real, moving UI), recorded as-is |
| copies per swap | `0 copies` on every swap line | 0 copies | ✓ |
| gop presents/refusals | `gop 0/2596` on every beat | `gop 0/175` | ✓ (letterbox never presents, mirror stays off) |
| timeouts / prepare-fail | 0 / 0 | 0 / 0 | ✓ |
| `vop direct` latch | mean **11,600 us**, longest 17,096, **0 over 50 ms** (:13779) | 11,829 / 17,058 / 0 | ✓ |
| flips / mapped / remaps | 2,595 / 3 / 3 (:13779) | 175 / 3 / 3 | ✓; mapped 3 again matches the census's 3 surfaces |
| `back to boot` | **1** (:13779) | 0 | the designed blank-swap path at a session teardown (round 11's source read); one occurrence, not a copy fallback |
| fence | **2,503 of 2,551 swaps arrived with GPU work not done; 1,805 flips waited (mean 11,879 us, longest 250,346 us, 2 gave up)** (:13781) | not printed in the round-13 table | first gave-ups in the record and a 250 ms longest wait — new under interactive load; noted in "next questions" |
| swap rate during interaction | `113 frames since the last report, busiest second 51, gap shortest 5472 us, mean 26905 us` (LF 10771, t+1725) | ~50 fps in the unlock animation | ✓ same tier; the busiest-second rate holds while the mouse moves |
| injector | **7,981 reports / fails 0** at t+1925 (:13933), `open 1` throughout | 11,155 / 0 | ✓ |
| panics | 0 | 0 | ✓ |
| process count (final beat) | **224** (t+1925, :13938) | 228 | ✓ |
| `lsr` | first 0 (t+5, :2008), final **25** (t+1925, :13938), no `-` anywhere | first 0, final 15 | ✓ same dense-read shape; the larger final is card reads during the frozen-UI phase |

## S4 — the freeze event (new problem,分层记录)

* **Phenomenon (user at the panel) [confirmed]:** the mouse worked (mode5's full chain, S2's
  counters); clicking applications opened them and they immediately quit; then the screen
  froze; unplugging/replugging USB changed nothing.
* **Passive serial probe [confirmed, round session]:** `logs/probe-frozen.py` listened on COM3
  read-only for 15 s (the probe's stdout was not saved to the project tree; its readings are
  recorded here from the session):
  * kernel alive — beats 2720/2725/2730 normal (`failures 0, procs 229, lsr 14`), so the
    heartbeat kept feeding the watchdog;
  * userland alive — launchd spawn/exit lines kept printing, the injector's gestures 490-492
    ran normally, backboardd's mali status lines kept refreshing;
  * **swap frozen at 2,596** — the capture's final value, never rising again: the compositor
    stopped committing new frames, so the panel shows the last frame the display hardware still
    scans. The freeze is downstream of the swap, not in the display path.
  * **`ubt` samples caught the wedge:** backboardd/WidgetRenderer_D threads repeatedly stopped
    at the same two PCs (**0x1daf40b58 / 0x1daf46bdc**, `st 0x1` waiting state); chronod and
    backboardd threads are in the same dump. The same PCs already appear in the capture's own
    `ubt` lines (the capture's tail, SpringBoard-labelled threads at 0x1daf40b58/0x1daf46bdc) —
    the wedge pre-dates the probe.
  * between beats `requests 280270 / blocks 39512704` were completely static — the freeze
    started after the interaction/app-launch attempts.
* **Clues (the shape was known earlier) [conjecture — causality not proven]:**
  * an `exit xpcproxy[...] rv 0x4e00 sig 0 code 78` storm (≈ EX_CONFIG, launchd repeatedly
    failing to spawn something): **163 lines** in the tier-5 capture, pids 81 through 599, the
    last three 595/597/599 (the capture's final line, CR-inclusive 13942 = LF 12669);
  * `mali: user: backboardd[57]: msg 34..62 ... the driver refuses a DRM_FORMAT_MOD_LINEAR
    image for host allocations` — 14 lines (:4102 first): the app-texture path is being refused.
* **Conclusion [layered]:** not a kernel death, not a watchdog scenario (the heartbeat kept
  running and failures stayed 0). It is a UI/render-thread wedge — userland threads parked at
  the same waiting point — plus an application-spawn failure (exit 78): an application-launch
  layer problem that only interaction exposes. The WidgetRenderer threads sitting in the wedge
  queues keep this in the same family as the white blocks.
* **Recovery:** the round's closing task sent 0x1d (the round's second) and the board was back
  in Linux — `/proc/uptime` read 123.17 s over SSH, consistent with a fresh boot at the probe's
  time (two earlier probes timed out against a stale host guess and were retried once).

## S5 — wrap-up and budget

* SSH was used twice to reboot into UEFI (tier-3 boot, then tier-5 boot); 0x1d was used once
  between the boots; the tier-5 capture was released at ~t+1930 so the passive probe could hold
  COM3 — the capture's last beat is t+1925 (:13938), and the probe then read the frozen board at
  beats 2720-2730.
* The tier-5 payload reached the board with:

```
cmd.exe /c py -3 tools\boot-uefi.py --port COM3 --baud 1500000 --log logs\boot-round14-usb5.log
  --tail-secs 2700 --local-payload artifacts\m5\payload-round14-usb5.img
```

* The closing task's 0x1d is the round's second reset; the board ended the round in Linux.

## Next questions

1. **The application-launch layer (the freeze's other half):** attribute the 163
   `exit xpcproxy[...] rv 0x4e00 ... code 78` lines — which binaries fail to spawn and why
   (EX_CONFIG usually means a malformed/missing plists or a bad executable path).
2. **The wedge PC:** symbolise 0x1daf40b58 / 0x1daf46bdc against the dyld shared cache
   (`dsc.py --slide`, the same path that named SpringBoard's MadGate wait in M7-08) — the next
   round can name the exact function the UI threads park in.
3. **The mali host-alloc refusals:** the 14 `refuses a DRM_FORMAT_MOD_LINEAR image for host
   allocations` lines (backboardd, msgs 34-62) sit on the app-texture path; check whether the
   exit-78 applications fail because their textures are refused.
4. **Hot-plug:** unmeasured this round — plug the receiver after the desktop is up and measure
   the enumeration-to-first-report latency.
5. **Fence gave-ups:** 2 gave up and a 250 ms longest wait appear for the first time under
   interactive load; worth a look when the wedge is pursued.

## The human-eye items

* **The panel was driven with a real mouse and keyboard** (confirmed — by the user's account and
  by every counter in S2). This is the first time anything other than the scripted injector
  moved this UI.
* The mouse/keyboard **feel and handing** (latency, smoothness, button behaviour) is the user's
  evaluation to give — the operator was at the panel for the interactive windows; no structured
  feel report was collected this round.
* The white blocks were not re-checked this round (the interaction windows took the operator's
  attention); rounds 11-13's readings carry forward.

## Verbatim log excerpts

The captures live only in the project working tree. Line numbers are the CR-inclusive
(Select-String) count; LF-only equivalents are given for the excerpts.

* The launchd command-line echo (CR-inclusive 2446 = LF 2319; 972 B / 53 tokens, middle elided):

```
com.apple.xpc.launchd|1969-12-31 16:05:33.850424 <Notice>: boot-args = -v serial=1 debug=0x10014e ... opi5-gop-mirror=0 opi5-surface-census=1 sheng-aks=1 offline-key-events=1 opi5-hid=1 opi5-hid-input=2 opi5-usb=5 mali=8 mali-metal=1 opi5plus-bt=SpringBoard opi5plus-bt2=backboardd opi5plus-bt3=WidgetRenderer_D opi5plus-bt4=chronod opi5plus-bt-secs=30 opi5plus-block-exec=bluetoothd panic_on_crash_disabled=1
```

* Tier-3 enumeration (CR-inclusive 3298-3301):

```
opi5: usb: ohci1 device vid 0x1532 pid 0xa6 class 0 mps0 64
opi5: usb: ohci1 configuration 84 bytes: boot keyboard yes (iface 1 ep 2 mps 16), boot mouse yes (iface 0 ep 1 mps 16)
opi5: usb: ohci1 mouse report descriptor 94 bytes: 05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 75 01 95 05 81 02 75 01 95 03 81 03 06 00 ff 09 40 75 08 95 ...
opi5: usb: ohci1 mouse report protocol: id -1, 8 bytes, buttons@0/3 x@32/16 y@48/16 wheel@24/8
```

* Tier-5: polling starts (CR-inclusive 1829) and the first burst window (CR-inclusive 3601 =
  LF 3150):

```
opi5: usb: ohci1 HID polling started (keyboard 1, mouse 1)
opi5: usb: poll frames=8860 reports=1998 errors=0 hid_total=1998 kbd_seq=2 mouse_seq=1996 wheel=0 proto=1
```

* Tier-5: the final consumer pair (CR-inclusive 13535-13536 = LF 12289-12290):

```
opi5: hid: mouse: 6154 reports to handleReport, last result 0x0
opi5: hid: mouse: USB reports handed on 6154, with motion 6067, sum |dx| 125188 |dy| 102998, newest raw 00 ff 00 00
```

* Tier-5: the final swap / `vop direct` / fence lines (CR-inclusive 13778-13781 = LF
  12514-12517, t+1920):

```
opi5: display t+1920s: swap ok 2596 rej 0 (reason 0) copy 2596 (status 0, 14745600 B, 0 copies) retired 2596 gen 2596/2596 frames 1 gop 0/2596 timeouts 0 prepare-fail 0, sd pio bursts 85630160, chunk us 2146473225, chunks 308225, fill us 1527456144, drainopi5: display t+1920s: registry: class match IOMobileFramebuffer published, IOHIDSystem absent; our IOName NativeLegacyDisplay IOClass ?, registrations 1, stage 10, busy 0; IOHIDEventService 2, IOHIDEventDriver 2
opi5: display t+1920s: vop direct (iommu 1, status 00000019 00000019): flips 2595, surfaces mapped 3 (map failures 0, let go 0, remaps 3, zap timeouts 0), latch mean 11600 us longest 17096 us (0 over 50 ms), copy fallbacks 0, back to boot 1
opi5: display t+1920s: fence: 2503 of 2551 swaps arrived with GPU work not done; 1805 flips waited (mean 11879 us, longest 250346 us, 2 gave up)
```

(the swap line carries the registry line interleaved after `drain` — a bare-CR fragment; the
`vop direct` and `fence` lines are clean)

* The capture's last beat, the injector's final heartbeat and the final exit-78 line
  (CR-inclusive 13933-13942 = LF 12663-12669):

```
opi5: hid: inject: gesture 347: lift at 16383,6553, reports 7981, fails 0, last result 0
opi5plus beat 1925 s, requests 279441, blocks 39452800, failures 0, procs 224, stage 32 (L12 late done), vfree 2033, vspec 333969, vcomp 0, vinact 126113, vact 192943, vext 579242, vpur 2784, vcact 0, vcmode 8, dbavail 291599, lsr 25
opi5plus: exit xpcproxy[599] rv 0x4e00 sig 0 code 78 age 3
```
