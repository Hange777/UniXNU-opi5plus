# M7-09 — The crash sites: Swift assertions, and an abort() out of PowerLog's storage

Device result, 2026-11-10. One boot of the M7-08 payload plus `patches/xnu/0005-crash-trace.patch`: every signal
death now prints the faulting thread's saved user registers and its user frame chain, in the same format the
sampler prints, so `dsc.py sym --slide` names each frame.

    kernel  1e1201e087af2b7f818f642dd7623367b06017439671e5840f0fe65c57f3de01
    payload 412358469e487a0728e82bb049cdfbc1eb90c47f10b56b29b740a507f927581c   (96 MiB)
    log     logs/boot-crash-1.log            (t+0 … t+1690)
    cache   slide 0x150c0000                 (from the same boot's sampler line)

## What the 118 signal deaths are

| process | count | pc | symbolized |
| --- | --- | --- | --- |
| liveactivitiesd | 17 | `0x1d69b64f8` | `SessionSyncEngine` (`_$s17SessionSyncEngine…MessageV0D4TypeOMa+0x13b0`), BRK #1, esr `0xf2000001` |
| PerfPowerService | 17 | `0x1e084af74` | `_pthread_kill` ← `_abort` ← `_err` ← `PowerLog -[PL… setEnabled:]` ← `startStorage` |
| assistantd | 17 | `0x1e084af74` | same abort shape, caller in its own text |
| fitcored | 16 | `0x1963a12f0` | `Swift._assertionFailure` ← `SeymourServices.Sandbox.bootstrap` ← `DaemonProtocol.main` |
| dmd | 16 | `0x1e084af74` | same abort shape |
| ScreenTimeAgent | 16 | `0x1e084af74` | same abort shape |
| mobileactivation | 16 | varies per launch | own text, `pc == lr`, BRK #1 — not resolved yet (§3) |
| launchd, MSUEarlyBootTask | 1 each | `_posix_spawn+8` | SIGHUP during spawn, not a fault |

## 1. Two of them are Swift preconditions in the daemons' own bootstrap

`fitcored` traps inside `_assertionFailure` called from `SeymourServices.Sandbox.bootstrap` and
`DaemonProtocol.main`; `liveactivitiesd` traps inside `SessionSyncEngine` from `SessionCore.Daemon.init`
(`__dispatch_once_callout`). Both are the first thing those daemons do at startup, so the precondition that
fails is about the environment they are being started in, not about work they were asked to do.

## 2. Four are `abort()` from a storage path

`PerfPowerService`'s chain is the readable one: `startStorage` → `setEnabled:` → `_err` → `abort` →
`pthread_kill`. `_err()` is the "log and die" helper, so PowerLog's storage could not be set up. `assistantd`,
`dmd` and `ScreenTimeAgent` die in the same `_pthread_kill ← abort` frames (their caller is in their own text).

That group lines up with the five daemons that **exit 1 instead of trapping** (`biomed` 57 times,
`biomesyncd` 74, `nsurlsessiond` 66, `bluetoothd` 63, `callservicesd` 52 in the M7-08 run): a whole class of
daemons cannot complete their startup on this system, and SpringBoard's synchronous XPC to two of them is what
holds the UI (M7-08). The common ingredient is what they do first: open their own database/container under
`/private/var` (PowerLog `startStorage`, Biome's `_BMXPCFileManager`). This boot's Data volume is the 2048 MiB
container, `NativeSd` reports 0 failures and `smfree` showed 505763/524288 blocks free at mount, so it is not
space; whether it is sandbox, container layout or the APFS view is the next question.

## 3. `mobileactivationd` dies in its own `main()` error path

Its 16 crashes all have `pc == lr`, esr `0xf2000001` (BRK #1), `x16` identical across launches, and a pc whose
page offset is always `0xac0` while its value moves by whole megabytes per launch (each `exec` gets its own ASLR
slide). The binary has exactly one `brk` at page offset `0xac0`, and the *distance* from the pc to the next frame
is the same in every crash (`0x198704`), so the site is fixed:

    0x1001eeac0  d4200020   brk #1                 <- the crash pc, in mobileactivationd (unslid)
    0x1001eeab8  9400022f   bl 0x1001ef374
    0x1001eeabc  97f99efd   bl <chained-fixup import>
    0x1001eeac0  d4200020   brk #1
    caller:      0x1000563b8  940661aa   bl 0x1001eea60     (return address 0x1000563bc, inside _main)

The symbol table puts the brk in **`_main.cold.5`** (the compiler's out-of-line error path for `_main`), and the
caller is `_main` itself. The block before the brk is "log/clean up, then die": two calls to functions in the
binary and two to chained-fixup imports. A `brk #1` at the end of a cold error block is what the compiler emits
for `unreachable` **after a `noreturn` call**; it only executes if that call *returned*. So `main()` takes its
failure branch, calls whatever it uses to die, that call comes back instead of aborting, and the unreachable
trap becomes the SIGTRAP launchd sees.

That also explains the shape: mobileactivationd never gets as far as serving the XPC that SpringBoard is blocked
on (M7-08). The open question is now narrow — which of the two imported calls in `_main.cold.5` is the one that
returns, and why (a chained-fixup bind that resolved to a no-op, or a call whose failure mode is "return"). The
OS-volume copy is the running image (the SystemOS volume has no `usr/libexec/mobileactivationd`), and its chained
fixups name the imports; that is the next step.

## 4. What the tooling now is

* `patches/xnu/0004-thread-sample.patch` — named-process user-stack sampler from the heartbeat;
* `patches/xnu/0005-crash-trace.patch` — signal-death registers and user chain from `exit_with_reason()`;
* `tools/wsl/tmp-sym-ubt.py` — per-sample, per-thread symbolized chains for a whole log;
* `tools/wsl/tmp-sym-crash.py` — grouped crash sites for a whole log;
* `tools/wsl/tmp-fetch-dsc.sh` + `tmp-fetch-zbitmap2.sh` — the scratch dyld shared cache under `/opt/uland`
  (7-Zip 25.01 for the sealed SystemOS volume, libzbitmap for the three decmpfs-14 files).

## 5. Next

The display chain is done; three userland items remain, in order:

1. `mobileactivationd`'s trap site (§3), because SpringBoard's main thread is blocked on its reply.
2. The storage group (§2): why `startStorage` and the container/database opens fail on a volume with 96 % free.
3. The five `exit 1` daemons, which the crash trace does not cover because they exit instead of trapping; the
   next instrument for them is `sd-trace-io=1` or a `printf` of `p->p_xstat` plus the last syscall.

## Reproduce

    wsl -d unixnu -u root bash tools/wsl/tmp-build-vramregion.sh      # kernel 1e1201e0
    wsl -d unixnu -u root bash tools/wsl/tmp-payload-bt.sh            # payload 41235846
    python tools\boot-uefi.py --local-payload artifacts\m5\payload-bt.img \
        --log logs\boot-crash-1.log --tail-secs 1750
    python3 tools/wsl/tmp-sym-crash.py /mnt/d/rk-mac/logs/boot-crash-1.log
