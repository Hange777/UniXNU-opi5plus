# Round 3 result — the injected HID reports are accepted, and SpringBoard leaves the lock screen

Date: 2026-11-11/12 (Asia/Shanghai). Board: Orange Pi 5 Plus (RK3588, 8 GiB, SD only, HDMI only).
Scope: one boot of the round-3 kernel and payload, t0 = 2026-11-11 23:54:05 (the kernel's first line), captured
to the end of the run. Wall-clock time is t0 + t.

The round-3 payload differs from round 2 in one command-line character (`opi5-hid-input=1` became `=2`, which
starts the synthetic report injector). With that, a HID report reaches the userland for the first time: the
kernel publishes the digitizer properties on an `IOHIDEventService`, `IOHIDEventDriver` opens it, 4,830 touch
reports are accepted with no failure, and the userland goes **from the lock screen into the home screen** —
which round 2 never did in 63 minutes.

| item | value |
| --- | --- |
| kernel | `0359b168244252f4d992d573f24fc8a82f370bd9f485fd9d0cf991f13e2d1d82`, 51,695,616 B (round 2: `45fbfab3…`) |
| payload | `artifacts/m5/payload-round3.img`, 100,663,296 B, sha256 `407ac58aa22700a5833f709bea900d8c042e715f735f399f3449228cefd75f13` |
| command line | 993 bytes, 54 arguments; the only difference from round 2 is `opi5-hid-input=2` |
| XNU patches | `xnu-patches.list` `3c0af5097e65aae95729e59358b1d6f83c4e2b0146d66b98f4fc53ac7fe2af0b`, byte for byte from round 2 — this round changed the board package only |
| build | five log error counts all 0, 170 s incremental (`evidence/round3-baseline/README.md`) |
| log | `logs/boot-round3.log`, 3.7 MB; the window closed at t+3600 s; project working tree, not committed |

Three files changed in this round, committed together with the pages that describe them:
`package/Opi5plusHid/NativeHidInput.cpp`
(the synthetic report injector), `package/Opi5plusHidProbe.cpp` (`opi5-hid-input` now selects how far the path
goes, `>=2` starts the injector) and `package/LegacyDisplayProbe.cpp` (the 30 s registry line counts matched
`IOHIDEventService` / `IOHIDEventDriver` instances).

## S1 — the kernel accepts the injected reports

Criterion: the descriptor the driver publishes carries a digitizer collection, an `IOHIDEventDriver` client opens
the service, and reports are accepted with the failure counter at zero for the whole run.

```
logs/boot-round3.log:1743  opi5: hid: inject: armed, mode 2, digitizer 1, 21 samples x 20 ms
logs/boot-round3.log:1756  NativeHidInput:0x10000019d open by IOHIDEventDriver:0x1000001a0 (0x0)
logs/boot-round3.log:1869  evb10: hid: digitizer properties published on the event service 0xfffffff361568a40
logs/boot-round3.log:1870  opi5: hid: inject: driver appeared 0xfffffff361568a40 open 1
logs/boot-round3.log:1880  … gesture 1: down at 16383,26213
logs/boot-round3.log:1921  … gesture 1: lift at 16383,6553, reports 23, fails 0, last result 0x0
```

Line 1869 is the line round 2 never printed: the digitizer properties are published *on the event service*, so
the report ID 3 collection is reachable from the kernel's HID event path, not merely present in a descriptor.

The injector's heartbeat then reports the same three counters for the rest of the run — 39 lines, from
`t+34 s` (line 1976) to `t+1171 s`:

| counter | first beat (line 1976) | last beat | over the whole run |
| --- | --- | --- | --- |
| `gestures` | 6 | 210 | 210 gestures, one every 5.4 s from about t+20 |
| `reports` | 138 | 4,830 | 4,830 reports, exactly 23 per gesture |
| `fails` | 0 | 0 | **0** |
| `last result` | `0x0` | `0x0` | **`0x0` throughout** |
| `open` | 1 | 1 | **1 throughout** |

23 reports per gesture is the shape the injector was built with — two for the Home press and release (report 2)
and 21 for the swipe (report 3, 80% to 20% of the screen height in 21 samples) — so the ratio says the timer, the
collection and the send path all ran regularly rather than retrying or dropping.

None of the injector's three failure lines appears anywhere in the capture: no `waiting: no event driver`, no
`waiting: not open`, no `giving up on the open gate`. The gate was satisfied before the first sample was due.

## S2 — a gesture turns into frames

Criterion: the GOP blit counter moves when a gesture lands on a freshly drawn screen, and the frames stop when the
screen stops changing.

The first four frames are the boot path's own drawing (the Apple logo and the first paint, the same shape round 2
had); they are listed for the timeline but are not attributable to the injector. What is attributable is everything
from t+1575 on.

| time | `gop blit` frames | what it is |
| --- | --- | --- |
| t+675 | 2 | first frames — the Apple logo on the panel |
| t+680 | 3 | |
| t+900 | 4 | |
| t+1560 | 4 | still 4: nothing has changed on screen for 11 minutes |
| t+1575 | 5 | the lock screen appears and the next gesture lands on it |
| t+1580 | 7 | |
| t+1595 | 11 | |
| t+1600 | 13 | |
| t+1605 | 16 | |
| **t+1615** | **20** | the unlock animation and the home screen finish drawing |
| t+1800 | 20 | static: nothing has changed on the home screen |
| t+2520 | 25 | one later repaint |
| t+2525 | 26 | |
| t+3600 | 26 | nothing more to the end of the window |

Sixteen of the round's twenty-six frames are copied inside 40 s of the lock screen appearing, in one continuous
run, and the counter then holds — apart from six more at t+2520 and t+2525, after which it does not move again
until the window closes at t+3600. That is not a rate and not tearing — it is one composited transition, drawn
once and left alone, which is the same reading round 2 gave its own static screen.

## S3 — the userland leaves the lock screen

Criterion: the panel shows the home screen, and the transition is reproducible from the log's own timing.

Observed on the panel by the operator: the Apple logo at about 00:05, the lock screen at about 00:20, and then
**the home screen, reached with no pointer and no keyboard** — the device unlocks itself.

The log and the wall clock agree: the first `gop blit` frames arrive at t+675 ≈ 00:05, the lock screen's first
frame is t+1560 ≈ 00:20, and the 16-frame burst falls in the 55 s that follow it. Round 2's log, with no injector,
holds 4 frames at t+1500 and then creeps by one frame a minute for the rest of the hour.

Attribution: the two rounds differ in exactly one argument, `opi5-hid-input=2`. The injector runs one Home
press/release plus one swipe down the screen's centre line every 5.4 s from t+20, so the first cycle whose samples
land on the freshly composited lock screen is the one that unlocks it. The 55 s from the lock screen's first frame
to the twentieth frame is the compositor's time to draw the transition, not the injector's latency.

## Round 2 against round 3

Same board, same card, same display path, same command line except one value.

| metric | round 2 (`opi5-hid-input=1`) | round 3 (`opi5-hid-input=2`) |
| --- | --- | --- |
| `gop blit` timeline | 2 at t+865, 4 at t+930, held to t+1500, 8 at t+1515, then **+1 frame a minute** for the rest of the run | 2 at t+675, 4 at t+900, **16 frames in 40 s at t+1575**, 26 at t+2525 and static |
| `gop blit` at the end | **54 frames**, 796,262,400 B | **26 frames**, 383,385,600 B (the totals section explains why fewer) |
| `vsync sent` | 0, all 62 lines | **0**, all 23 lines |
| `swap ok` | 54 accepted, 0 refused | **26 accepted, 0 refused** |
| what the panel shows | the lock screen, for the whole 63 minutes | lock screen, then **the home screen** |
| HID reports accepted | 0 (none could be) | **14,927, 0 failed** |

## Negative results

1. **No digitizer refusal.** The whole log carries no `ignoring digitizer event … from unsupported service` and
   no `unsupported service` at all. That line is what keeps `backboardd` from refusing a report it cannot route,
   and it never printed, so the reports were not merely accepted by the kernel and then dropped at the user end.
2. **`vsync sent` is still 0.** Round 3 does not change this: the counter reads 0 in every vsync line, as in
   round 2. The unlock is not driven by a vsync; the compositor drew the transition and stopped.
3. **A wireless mouse still does nothing, and that is the expected result.** XNU has no USB host stack here: no
   USB configuration in `configs/`, and `IOUSBHostFamily` / `AppleUSBXHCI` are not in the prelink. The
   `XHCI: Initialize DWC3` / `XhcDriverBindingStart` lines in this log are the firmware's `XhciDxe.efi`, printed
   before `DARWINBOOT: hand-off`. The hot-plug is therefore a measurement of a path that does not exist yet, and
   it confirms the round-2 conclusion rather than contradicting it.

## Judgements this round retires or corrects

* **`IOHIDSystem` is retired as a metric.** The class does not exist in 21H461 and no personality in any prelinked
  kext creates one, so its `absent` reading — every 30 s for the whole of rounds 1 and 2 — carried no information
  (`evidence/m5/M5-08`). The metric that does answer is the `IOHIDEventService` / `IOHIDEventDriver` instance
  count, and in this run it is non-zero on both sides of the link.
* **The three `XhcDriverBindingStart` lines are firmware output, not XNU's.** They are printed before the
  `DARWINBOOT: hand-off` line by the firmware's own `XhciDxe.efi`, so they say the firmware has a USB host
  controller and nothing about the kernel. Round 3's mouse measurement is the first positive confirmation of this.
* **Round 2's "input does not work at all" was a command-line result, not a broken link.** `sheng-touch-hid=1`
  (which builds the digitizer collection) and `offline-hid-auto-home` (the only self-triggered report) were both
  absent, and `native_touch_set_sink()` is empty on this board, so no report could have arrived by design. With
  `opi5-hid-input=2` the same device carries 4,830 reports, which is what makes this a correction rather than a
  new finding.

## End-of-window totals

The window closed at **t+3600 s**, the same point round 2's did: the payload's `sd-auto-reboot=3600` fired, the
last display sample is `t+3600s` (line 27539) and the last injector heartbeat `t+3603 s` (line 27544). After that
the log carries the *next* boot's firmware and Linux output — `Ubuntu 22.04.5 LTS 5.10.0-1012-rockchip` from line
28226 — which is not part of this window. The capture was not cut short; it ended because the board rebooted
itself, so these are the totals of a full window and not a partial run.

**Line-number convention.** Every `logs/boot-round3.log:<line>` citation on this page — here and in the
sections above — is a `grep -n` / `wc -l` count. The serial log embeds isolated `\r` characters, which some
tools treat as line breaks, so for this capture PowerShell's `Get-Content` numbers 2,222 lines higher than the
file really has (30,535 against 28,313); check a citation with `grep -n` or `wc -l`, not `Get-Content`.

| metric | round 3, at t+3600 s | where |
| --- | --- | --- |
| `gop blit` frames | **26** | line 27539 |
| frames refused | **0** | |
| bytes copied | **383,385,600 B** (26 × 14,745,600) | |
| last frame | 14,745,600 B in **91,633 µs**, max 95,855 µs | |
| `swap ok` | **26** accepted, `rej 0`, `copy 26`, `retired 26`, `timeouts 0`, `prepare-fail 0` | line 27538 |
| `vsync sent` | **0**, with `arms=0`, in all 23 vsync lines of the run; the last is `t+2525 s` | |
| registry | `IOMobileFramebuffer` published, `IOHIDSystem` absent, `IOHIDEventService 1`, `IOHIDEventDriver 1` | line 27540 |
| injector | **649 gestures, 14,927 reports** (exactly 23 each), `fails 0`, `last result 0x0`, `open 1` | line 27581 |

The whole frame timeline, which is what the totals have to be read against:

| t+ | 675 | 680 | 900 | 1560 | 1575 | 1580 | 1595 | 1600 | 1605 | 1615 | 2520 | 2525 | 3600 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| frames | 2 | 3 | 4 | 4 | 5 | 7 | 11 | 13 | 16 | 20 | 25 | 26 | 26 |

Sixteen frames in the 55 s from the lock screen's first paint, then fifteen minutes of nothing, six more at
t+2520-2525, then **26 unchanged for the final 18 minutes**.

### The frame total is lower than round 2's, which is the expected direction

Round 2 ended at **54 frames / 796,262,400 B**; round 3 ended at **26 / 383,385,600 B**, less than half. Read as a
rate that looks like a regression, and it is not one:

| | round 2 (`opi5-hid-input=1`) | round 3 (`opi5-hid-input=2`) |
| --- | --- | --- |
| frames at t+2520 | 36 | 25 |
| end of window | 54 frames, 796,262,400 B | 26 frames, 383,385,600 B |
| shape after the lock screen | 4 → 8 at t+1515, then **one frame every 60 s with no gap** from t+2555 to t+3575 | 4 → 20 in 55 s, +6 at t+2520-2525, then **flat for 18 minutes** |
| what the panel showed | the lock screen, never left it | the home screen from t+1615 on |

Round 2's counter was a clock: the lock screen it never left repaints itself about once a minute, and each repaint
is one frame. Round 3's counter measures a transition: the unlock is drawn once over about a minute and the home
screen behind it is static, so the counter stops. A round that unlocks therefore totals *fewer* frames than a round
that stays locked, and the two totals are not comparable as throughput. **The frame count is not the criterion;
the shape of the frames is** — a 16-frame burst inside 55 s against a one-frame-per-minute drip. `vsync sent`
remained 0 throughout, so nothing here is vsync-driven.

### Figures quoted earlier in this page

The S1 section was written while the capture was still running, at about t+1171 s, and quotes "210 gestures /
4,830 reports" because those were the counters at that moment. The finished figures are the ones in this section —
**649 gestures, 14,927 reports, 26 frames** — and the S1 numbers are a snapshot of the same run, not totals. The
same applies to the `4,830` in `README.md`, `evidence/HANDOFF.md`, `evidence/README.md` and `porting.toml`.
Nothing else in them changes.

## Known visual defects

*Three white rounded rectangles where the home screen's widgets belong.* `WidgetRenderer_Default` (pid 433) is
running, and it opens its Metal connection at `logs/boot-round3.log:9532-9533` (`mali: connection opened by
WidgetRenderer_D`, `metal: bundle loaded in WidgetRenderer_Default (pid 433)`) between the `t+1580 s` and
`t+1590 s` display samples — the moment the unlock transition starts drawing. So the widget process did start and did get a GPU
connection; what comes out of it is an empty surface. That places the defect on the driver side, with the banded
large glyphs (`metal2vulkan-degrade-shadowed-texture`: the driver refuses a linear `A8Unorm` texture whose row
pitch is 352 bytes), not on the process side. Icons, the dock and the wallpaper render correctly. This is an
appearance defect and it blocks nothing.

## What this record does not claim

* It does not claim the injection is a touch panel. It is a scripted gesture stream: one Home press/release and
  one vertical swipe down the centre line every 5.4 s. It proves the HID event path end to end and says nothing
  about coordinates from a real digitiser.
* It does not claim `vsync sent=0` is understood. It is unchanged from round 2 and still unexplained.
* It does not claim the horizontal banding inside the large glyphs is fixed or diagnosed: this round did not
  revisit it, and the home screen has no large glyph to compare.
* It does not claim USB input is close. The host stack is absent from the kernel configuration and from the
  prelink; the mouse hot-plug measures that absence.
* The log stays in the project working tree (`logs/boot-round3.log`) and is not committed; `AGENTS.md` forbids
  committing logs. This page cites line numbers into that file rather than copying it.
