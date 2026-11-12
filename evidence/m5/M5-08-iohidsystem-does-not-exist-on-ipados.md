# M5-08: `IOHIDSystem` does not exist on iPadOS 17 — the gate was a macOS name

Date: 2026-11-07. Corrects `M5-06` and `M5-07`.

## 1. What was wrong

Both documents ended on "`IOHIDSystem` is absent, and backboardd waits for it". That name comes from macOS
`IOHIDFamily`. This board runs the iPadOS 17.7.11 (`21H461`) kernelcache, and that kernel has no such class:

```
$ strings -a kernelcache.decompressed | grep -E '^IOHID[A-Za-z]+$' | sort -u
IOHIDALS…  IOHIDAsyncReportQueue  IOHIDBacklightLevel  IOHIDClientData  IOHIDDevice  IOHIDDeviceElementContainer
IOHIDDeviceShim  IOHIDElement  IOHIDElementContainer  IOHIDEvent  IOHIDEventDriver  IOHIDEventDummyService
IOHIDEventQueue  IOHIDEventService  IOHIDEventServiceFastPathUserClient  IOHIDEventServiceQueue
IOHIDEventServiceUserClient  IOHIDEventSource  IOHIDInterface  IOHIDKeyboardEventDevice  IOHIDLibUserClient
IOHIDOOBReportDescriptor  IOHIDOrientationType  IOHIDPlacementType  IOHIDPointingEventDevice
IOHIDProviderPropertyMerger  IOHIDReportElementQueue  IOHIDResource  IOHIDResourceDeviceUserClient
IOHIDResourceQueue  IOHIDUserDevice …
```

`IOHIDSystem` appears exactly once in the whole 46 MB image, in a log format string
(`"IOHIDSystem posted stackshot event 0x%08x"`). The prelink info's `IOKitPersonalities` for
`com.apple.iokit.IOHIDFamily` contain only `IOHIDResource`, the user clients and the event-driver personalities;
there is no personality that creates `IOHIDSystem` either. On iOS the event system is the `IOHIDEventService`
stack (`IOHIDEventDriver` + `IOHIDEventServiceUserClient`), not a kernel `IOHIDSystem`.

So the registry line in the display reporter was asking for a class that can never appear, and the "gate"
conclusion of `M5-06`/`M5-07` is retired. The HID work itself is still correct and useful (the port builds, the
device starts on the original `IOHIDResource`), it just was not the reason nobody opens the framebuffer.

The reporter now also prints `IOHIDResource` and `IOHIDEventService` class matches, which are the names that
can actually appear here.

## 2. What is left, and the timing explanation

Facts, unchanged:

| fact | evidence |
|---|---|
| the framebuffer service is published and matchable by class | `class match IOMobileFramebuffer published, registrations 1` |
| no client ever opens it | `opened 0`, `seen 0 clients`, `busy 0` |
| the userland is complete and running | `procs 85`, SpringBoard, runningboardd, logd, amfid, … |
| the reference port's client appears about 65 s after boot | `M5-06` §1 (`swap ok 26` at t+65 s on NVMe) |

Our root is on the SD card and the same boot takes about 475 s to reach 85 processes, roughly seven times the
reference's. Their first client at t+65 s therefore corresponds to something like t+450…t+700 s here, and every
capture so far stopped at t+600 s -- before `backboardd` (the display server) even appeared. In `display15`
(no HID) `backboardd` shows up at about t+720 s.

So the next run watches 1200 s instead of 600 s.

## 3. Run

Payload `c79d5801…` (kernel `9d96dcdd…`, 64 MiB Data seed, `sd-auto-reboot=1400`), `logs/display24-a.txt` and
`logs/display24-b.txt`. Result in section 4.
