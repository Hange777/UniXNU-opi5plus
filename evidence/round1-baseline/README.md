# Round 1 baseline check — reproduced or not?

Date: 2026-11-11 (Asia/Shanghai). Scope: build only. No instrumentation, no board, no ESP/SPI write.

## Build command (verbatim, as root in WSL distro `unixnu`)

```sh
cd /mnt/d/rk-mac/repos/UniXNU
UNIXNU_OUT=/opt/unixnu-out ./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
UNIXNU_OUT=/opt/unixnu-out UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553 ./unixnu build kernel -D ../UniXNU-opi5plus
```

`10-build-kernel.sh` is that pair plus a before/after sha256; `build-round1.log` and
`build-round1-run2.log` are its two runs (`tee` of stdout/stderr).

## Result

| artifact | round 5 (before this round) | run 1 | run 2 |
| --- | --- | --- | --- |
| `kernel/kernel.macho` sha256 | `4782b2210d50783ec614525ee41b8265df73fa83784dffcd93e021a54601331e` | `bd9a0b23c9435ad2c9a78bd9685daaa9ef11736adbe0397114097d0f07b2cdef` | `6792b4d3eb06294c0b842328b4be3633940dea82b97d92df89d9c052dd63a141` |
| `kernel.json` `native_sha256` | `a9ea2ae6635756fe10bcb1098105b45fd13bd8c5d8947ccfc8281c1eaabd7374` | `78522dd71921f3e18ad7477a91abb20279bfadf545de69da91f5c74713d60b8f` | `14b7436cec0165b7bea55efeb724742b34dcba1009690e275322e4d21dd6a96e` |

The pre-rebuild kernel in `/opt/unixnu-out/.../opi5plus_display/kernel/kernel.macho` had sha256
`4782b221…`, identical to the kernel inside `artifacts/m5/payload-stub.img` (both checked before the
rebuild, `00-baseline-probe.sh`).

`xnu-patches.list` sha256 is **`3c0af5097e65aae95729e59358b1d6f83c4e2b0146d66b98f4fc53ac7fe2af0b`**
(77 entries) for round 5 and for both rebuilds; `diff -u` between the two snapshots is empty
(`xnu-patches.round5.list` / `xnu-patches.list`, see `08-final-verify.sh`).

Build-error counts after the rebuild: `kernel.log` 0, all `archive-*.log` 0, all `component-*.log` 0,
`prelink.log` 0, `kernel-bootstrap.log` 0 (regex `:[0-9]+:[0-9]+: error:`).

Each run printed its own `kernel: <path> sha256 <hex>` line (`kernel.py:223`); the three values are the
table above.

## Why the sha256 does not match

`xnu-patches.list`, `xnu-inputs.json` (still the 03:20 signature: 77 patches, xnu `d8b8029`) and every
feed checkout are identical to round 5. The XNU tree itself was not recompiled: no `clang -c` line in
`kernel.log`, no `.o` newer than 05:00 under `build/xnu/ios-obj` (0 hits in `02-recompile-probe.sh`); only
`LD`, `STRIP` and `LIBTOOL` ran there. The component and archive steps did re-run (`ArmCpu.o` rebuilt at
13:35:00, `libSoftwareAES.a`/`libNativeSd.a`/`libMali.a` re-archived), and their output is identical to
round 5 — the 38-byte diff is the proof, not an assumption.

`cmp -l` between the round-5 kernel (extracted from the payload) and the rebuild shows **38 differing
bytes** (`07-diff-count.sh`):

* 8 sites, 2 bytes each: the low half of a 4-byte field whose value is a Unix timestamp equal to the
  wall clock of that run's link — round 5 `1794341274…1794341332` (04:07:54–04:08:52), run 2
  `1794375268…1794375325` (13:34:28–13:35:25);
* 22 bytes: the `_PrelinkKCID` base64 string in `__PRELINK_INFO`, which is derived from the file content.

There is no other difference: the compiled code, DATA and the original kernelcache part are byte-identical
to round 5. So the kernel is **semantically reproduced**, but the artifact is not bit-reproducible: the
link stamps the current time into the output, and every rebuild therefore changes sha256 and `_PrelinkKCID`.

## How the build picks the SoC patches (the hazard turns out not to apply)

The device repository pins the SoC feed and the build consumes the cached feed clone, not the local
`repos/UniXNU-rk3588-common` working tree:

* `repos/UniXNU-opi5plus/feeds.toml:1-5` — `[[source]] kind = "soc" name = "rk3588"`, commit
  `b5f5d949e9573b5e2232c2547abf2e3c8f7a05fe`; `unixnu.lock:24-25` records the same commit.
* `scripts/unixnu/profiles.py:27` calls `feeds.resolve_soc`; `feeds.py:255-277` uses a `local` entry or
  `--local-module` for a working tree, otherwise the cached git checkout under `ctx.feeds_dir`
  (`config.py:130`: `<out>/feeds`), fetching only if it is missing.
* `/opt/unixnu-out/feeds/UniXNU-rk3588-common-75c7120d` is at `b5f5d94` (clean, detached); the directory
  key is `sha1(url + "\n" + commit)[:8]`, which is `75c7120d` for this commit (`01-feed-commits.sh`).
* Round 5's own `xnu-patches.list` names the patches at that cache path, and
  `patches.py:66` writes the list from the resolved series, so the build read `b5f5d94`, not the local clone.
* The local clone is only used if `repos/UniXNU-opi5plus/local.toml` exists with
  `local = "../UniXNU-rk3588-common"` (template: `local.toml.example`); it does not exist.

Step 2: `git -C repos/UniXNU-rk3588-common cat-file -t b5f5d94` → `commit` (the object is already present,
no fetch was needed). It is not an ancestor of HEAD `5abfa39` (`merge-base --is-ancestor` exit 1). The
series at `b5f5d94` contains `rk3588-cpu-idle.patch` (`git show b5f5d94:patches/xnu/series`); the HEAD
working tree at `5abfa39` does not (9 patch files instead of 10). That mismatch is real but irrelevant to
this build, because the working tree is not an input.

## Side effects on the repositories (read this)

No source file was edited by hand and no manual `git checkout/reset` was run, but the project's own archive
step does write into a checkout: `apply-armcpu-patches.py` (SoC feed, `board.toml` archive entry
`apply-armcpu-patches.py`) runs `git -C drivers/ArmCpu checkout -- .` and then applies the four
`ArmCpu-rk3588-*.patch` files. `tree/drivers/ArmCpu` is a symlink to `repos/UniXNU/drivers/ArmCpu`, so both
build runs restored that submodule to its pin and re-applied the patches; `ArmCpu.cpp` was rewritten
(mtime 13:34:57, 88 insertions/9 deletions) and is left "M" in `git status`, which is exactly the state it
was already in before the build (checked at 13:30, before the first run). Anyone with extra manual edits in
that submodule would have lost them to this documented step; none could be observed, and the kernel diff
shows the compiled result is the same as round 5.

`git status` of the other repositories is otherwise unchanged: `repos/UniXNU` has the same five pre-existing
entries (`drivers/ArmCpu`, `drivers/DarwinBoot`, two `package/UniXNUPkg` files, untracked `package/Build/`),
`repos/UniXNU-rk3588-common` is clean. `repos/UniXNU-opi5plus` is not a git checkout at all (no `.git`), so
these evidence files are plain files, not a commit. All other writes are build output under
`/opt/unixnu-out` and the files in this directory.

## Files in this directory

* `README.md` — this note.
* `xnu-patches.list`, `xnu-patches.round5.list` — the snapshot required by the round (identical).
* `10-build-kernel.sh` — the build command pair used.
* `build-round1.log`, `build-round1-run2.log` — full stdout/stderr of both runs.
* `00-baseline-probe.sh` — pre-rebuild hashes, error counts, payload extraction.
* `01-feed-commits.sh` — feed cache HEADs vs `unixnu.lock`, rk3588 series listing.
* `02-recompile-probe.sh` — shows the XNU tree was not recompiled (0 compile lines, 0 new objects).
* `03-diff-bytes.sh`, `07-diff-count.sh` — the 38-byte diff and the values at each site.
* `08-final-verify.sh` — patch-list snapshot/diff, error counts, `kernel: … sha256` lines, toolchain.

## Consequence for the next round

The gate "rebuilt kernel sha256 == `4782b221…`" cannot be met by any rebuild: the 8 timestamps and the KCID
change on every link. Before instrumentation, choose one of:

1. compare kernels with the 8 timestamp fields and `_PrelinkKCID` masked (input/code identity is already
   proven, not assumed), or
2. make the kernel link reproducible (fixed epoch, e.g. `SOURCE_DATE_EPOCH`/`faketime` around
   `build-xnu-4k-baseline.sh`), which is a build-script change and therefore the coordinator's call.

Do not start the instrumentation round against a sha256 equality gate.
