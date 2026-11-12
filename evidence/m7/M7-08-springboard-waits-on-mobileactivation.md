# M7-08 — Where SpringBoard is: a synchronous XPC to mobileactivationd that never returns

Device result, 2026-11-10. One boot of the M7-07 payload plus `patches/xnu/0004-thread-sample.patch`: the
heartbeat samples the user threads of up to four named processes every `opi5plus-bt-secs` seconds and prints
each thread's last-trap registers and the frame-pointer chain of its user stack.

    kernel  33a2a7320ac21cab42c857937ab32db2174b15c1789c50a24e57f8c0353b4f44
    payload 12bb15425e4a8715220c51be6636b38aa2836179cd9c4ee3f1d459bff36632a1   (96 MiB)
    args    opi5plus-bt=SpringBoard opi5plus-bt2=backboardd opi5plus-bt-secs=60
    log     logs/boot-bt-1.log
    cache   base 0x180000000, first mapping 0xf378000, slide 0xf378000  (printed by the sampler itself)

Symbolising the printed addresses (see §"How to read the addresses" below) turns every frame into a name.

## The render server is not the problem; SpringBoard is

`backboardd`'s threads sit in their normal `mach_msg2_trap` waits on the render-server loop. SpringBoard's
main thread does this:

| t | main thread (tid 0x126) | threads |
| --- | --- | --- |
| 360 s | `dyld4::prepare` → `notifyDebuggerLoad` → `addImages` → `ProcessSnapshot::addSharedCacheImage` | 1 |
| 660–900 s | `objc::realizeClassWithoutSwift` → `prepareMethodLists` → `map_images_nolock` (same pc for four samples) | 1 |
| 960 s | `load_categories_nolock` → `load_images` → `notifyObjCInit` | 1 |
| 1020–1080 s | `lookUpImpOrForward` → `realizeClassMaybeSwiftMaybeRelock` | 3 → 6 |
| 1140–1440 s | `mach_msg2_trap` — and it never leaves | 6 … 10 |

So SpringBoard spends its first **~19 minutes of life inside dyld and the Objective-C runtime** (single
threaded until t+1000), and then blocks in one synchronous XPC that never answers. The three frames M7-06 saw
at t+935 arrive while SpringBoard is still realizing classes; when it enters the wait at t+1140, nothing
changes any more and the compositor stops submitting.

## What the wait is

Full chain of the stuck thread at t+1440 (every address symbolized):

    _mach_msg2_trap+8
      _mach_msg_overwrite+0x1ec
      _mach_msg+0x14
      __dispatch_mach_send_and_wait_for_reply+0x20c
      _dispatch_mach_send_with_result_and_wait_for_reply+0x38
      _xpc_connection_send_message_with_reply_sync+0x100
      ___NSXPCCONNECTION_IS_WAITING_FOR_A_SYNCHRONOUS_REPLY__+0xc
      -[NSXPCConnection _sendInvocation:orArguments:count:methodSignature:selector:withProxy:]+0x7e8
      -[NSXPCConnection _sendSelector:withProxy:arg1:]+0x70
      __NSXPCDistantObjectSimpleMessageSend1+0x38
      -[MadGate getActivationState:]+0x100                     MobileActivation
      _getActivationState+0x28
      _MAEGetActivationStateWithError+0xc
      -[SBLockdownManager _setupActivationState]+0x38            SpringBoard
      -[SBLockdownManager lockdownState]+0x20                    SpringBoard

Two more SpringBoard threads are in the same shape, waiting on different services:

    -[PLClientLogger xpcSendMessageWithReply:withClientID:withKey:withPayload:]      PowerLog (TCC permission)
    -[_BMXPCFileManager _configureConnection:] / _newConnectionForDomain:             BiomeFoundation  (→ biomed)
    -[FBSProfileManager _workQueue_reloadManagedApplicationBundleIDs] → -[MCProfileConnection managedAppIDs]
                                                                                     ManagedConfiguration

None of these calls has a timeout. `SBLockdownManager lockdownState` is the state SpringBoard asks for before
it decides which UI to put up, so the whole boot is gated behind mobileactivationd's answer.

## Why the answer never comes

From the same run's exit ring (`opi5plus: exit …` lines):

| process | exits in 1470 s | mode |
| --- | --- | --- |
| fairplayd.A2 | 107 | exit 0 (normal churn) |
| biomesyncd | 74 | exit 1 |
| nsurlsessiond | 66 | exit 1 |
| bluetoothd | 63 | exit 1 |
| biomed | 57 | exit 1 |
| callservicesd | 52 | exit 1 |
| **mobileactivation** | **17** | **SIGTRAP** |
| PerfPowerService | 17 | 16 × SIGABRT |
| liveactivitiesd | 16 | SIGTRAP |
| assistantd | 16 | SIGABRT |
| fitcored | 14 | SIGTRAP |
| dmd / ScreenTimeAgent | 14 / 14 | SIGABRT |

So the daemons SpringBoard is waiting on are exactly the daemons that die within seconds of starting:
`mobileactivationd` traps (SIGTRAP) 17 times, `biomed` exits 1 fifty-seven times. launchd restarts them
because the XPC connections exist, they die before replying, and SpringBoard's synchronous call waits forever.
The stall is not in the display chain, the GPU, the framebuffer or the region table: it is a userland crash
loop that the UI is synchronously blocked on.

## How to read the addresses

The sampler prints, once per sample, the shared cache's base, first mapping and slide. `pc`/`up` are the
**slid** addresses; the symboliser subtracts the slide. The cache is extracted from the sealed SystemOS volume
with 7-Zip 25.01 (libfsapfs cannot read a sealed volume), the three decmpfs-14 files are decoded with
libzbitmap, and `dsc.py` from `repos/uefi-evb10/tools/xnu-probe/symbolize` does the naming:

    7zz x 044-76034-038.dmg -o/opt/uland/dsc -y 'System/Library/Caches/com.apple.dyld/*'
    python3 decmpfs14.py "dyld_shared_cache_arm64.symbols:com.apple.decmpfs" \
        "dyld_shared_cache_arm64.symbols:com.apple.ResourceFork" dsc/dyld_shared_cache_arm64.symbols
    ULAND=/opt/uland python3 dsc.py sym --slide 0xf378000 0x1e2ffcfd8 0x1deef4928

`tools/wsl/tmp-sym-ubt.py` does the whole log at once: it collects every address, symbolizes it and prints the
per-sample, per-thread chains, and `tools/wsl/tmp-fetch-dsc.sh` + `tools/wsl/tmp-fetch-zbitmap2.sh` build that
scratch directory (about 3 GB under `/opt/uland`).

## What this changes in the plan

The next measurement is not another display experiment. The five daemons that trap or exit need to say *where*
they die: `patches/xnu/0005-crash-trace.patch` prints the saved user registers and the user frame chain of every
signal death from `exit_with_reason()`, in the same format, so the trap site symbolizes the same way. That is the
run after this one.

## Reproduce

    wsl -d unixnu -u root bash tools/wsl/tmp-build-vramregion.sh      # kernel 33a2a732
    wsl -d unixnu -u root bash tools/wsl/tmp-payload-bt.sh            # payload 12bb1542
    python tools\boot-uefi.py --local-payload artifacts\m5\payload-bt.img \
        --log logs\boot-bt-1.log --tail-secs 1750
    python3 tools/wsl/tmp-sym-ubt.py /mnt/d/rk-mac/logs/boot-bt-1.log
