# M7-10 — The stand-in reaches /usr/libexec, and AMFI kills it for launch constraints

Device result, 2026-11-10. Two boots on top of M7-09: `patches/xnu/0006-block-exec.patch` (refuse the daemon's exec
so launchd fails its request) and `patches/xnu/0007-exec-stub.patch` (a payload directory union-mounted over
`/usr/libexec`, carrying a stand-in for the daemon).

    block boot   kernel ea195965ac3bd9eabb784001e7ce3cb3f99101b1e9720e1f18011cc3e8b0c414  log logs/boot-block-2.log
    stub boot    kernel 0bdba82c8c28a3638570f7f32186df594f07aa5638204af9469929726f18c6c5  log logs/boot-stub-1.log
                 payload 80c70f5681a89d52872d5bd580f47977fb4a1fe49599301f329ff5286fff45c1

## 1. Refusing the exec does not unblock SpringBoard

`opi5plus-block-exec=mobileactivationd` fires (`opi5plus: exec of /usr/libexec/mobileactivationd blocked`) and the
daemon no longer starts, but SpringBoard's main thread is **still** in the synchronous
`-[MadGate getActivationState:]` wait at t+1260, exactly as in M7-08. launchd accepts the request, fails to start
the job, and neither answers nor drops the message. So the fail-fast idea is dead: the service has to **reply**.

## 2. The union over /usr/libexec works

The stand-in is packed into the GPU add-on blob (`scripts/make-exec-stub-addon.py`), so the add-on installer puts it
on the Data volume before launchd runs, and the new mount puts it in front of the sealed binary:

    opi5plus: addon: 4 file(s): 1 written, 3 already in place, 0 failed: 0
    opi5plus: exec: /usr/libexec over /private/var/ngpu/addons/exec: stage 3 status 0;
                    /usr/libexec/mobileactivationd 2960496 -> 70176 bytes

`2960496` is the sealed daemon, `70176` is the stand-in, so the union resolved to the replacement and nothing else
in the boot moved (140 processes at t+1330, the usual churn, the extensions mount still reports stage 5).

## 3. AMFI kills the stand-in before its first instruction

    AMFI: Launch Constraint Violation (enforcing), error info: c[1]p[1]m[1]e[2], (Constraint not matched)
          launching proc[vc: 1 pid: 135]: /usr/libexec/mobileactivationd, launch type 1,
          failure proc [vc: 1 pid: 135]: /usr/libexec/mobileactivationd
    opi5: crash pid 135 (mobileactivation) rv 0x9 sig 9 pc 0x100c17e50 lr 0 …

56 attempts between t+775 and t+1320, one every ten seconds, each one a SIGKILL: the stub never runs, so it never
checks in to `com.apple.mobileactivationd.lockdown`, and SpringBoard waits as before. The trust cache is not the
problem — the boot reports all three modules, including the one that carries the stub's cdhash
(`2e9bfb493bc6f7bdb9c62818f8dc952fc2012c16`) — the launch constraint is. The constraint category of an owner-trusted
binary is not the platform one, which is what this port's base series already knows.

## 4. The fix is a boot argument that already exists

`patches/xnu/zzyc-owner-skip-launch-constraints.patch` clears `mac_proc_check_launch_constraints()` when
`sheng-no-lc=1` is on the command line. It is not on this payload's command line. The next boot adds it:

    --kernel-arg sheng-no-lc=1

and the expected sequence is: launchd starts the stand-in, the stand-in answers `getActivationState:` with
`@"Activated"` (evidence that SpringBoard leaves the wait is the sampler's `opi5: ubt SpringBoard` chain moving off
`_mach_msg2_trap`; the panel is the second witness).

## 5. What is still not explained

* the storage group of M7-09 is unchanged in both boots (`PerfPowerService`, `assistantd`, `dmd`, `ScreenTimeAgent`
  abort; `biomed`, `biomesyncd`, `nsurlsessiond`, `bluetoothd`, `callservicesd` exit 1);
* SpringBoard still spends ~19 minutes in dyld and the Objective-C runtime before it asks anything;
* the board did not come back from `sd-auto-reboot` once at the end of the block boot (no serial, no SSH); a power
  cycle brought it back, so the reset path needs a look of its own.

## Reproduce

    wsl -d unixnu -u root bash tools/wsl/tmp-build-stub.sh           # kernel 0bdba82c
    cd repos/UniXNU-opi5plus && python3 scripts/build-activation-stub.py
    python3 scripts/make-exec-stub-addon.py \
        ../MaliMetal/metal-plugin/out/NativeMaliMetal.addon \
        build/activation-stub/mobileactivationd /mnt/d/rk-mac/.tmp/native-plus-stub.addon
    python3 scripts/add-trust-cdhash.py .cache/gpu-trust.bundle 2e9bfb493bc6f7bdb9c62818f8dc952fc2012c16
    wsl -d unixnu -u root bash tools/wsl/tmp-payload-stub.sh        # payload 80c70f56, add sheng-no-lc=1 next time
    python tools\boot-uefi.py --local-payload artifacts\m5\payload-stub.img \
        --log logs\boot-stub-2.log --tail-secs 1750
