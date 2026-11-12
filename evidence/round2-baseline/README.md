# Round 2 gate — build clean, payload packed at 993 bytes after dropping `bt7`/`bt8`

Date: 2026-11-11 (Asia/Shanghai). Scope: build, package, verify. No board, no ESP/SPI write, no
`wsl --unregister`. `board.toml` untouched; `payload-round1.img` and `payload-stub.img` untouched.

## Result

| item | value |
| --- | --- |
| build | **clean**: `:[0-9]+:[0-9]+: error:` = 0 in `kernel.log` and in every `archive-*.log` / `component-*.log` / `prelink.log` / `kernel-bootstrap.log` |
| compile errors to fix | **0** — the seven round-2 changes compiled as they were |
| `kernel/kernel.macho` before this build | `7cf1c39bb60e5f9ad462241c35cc3de244470ef0fff0926ab94a01782d576987` (51,695,616 B, left by the 19:44 build) |
| `kernel/kernel.macho` after this build | **`45fbfab3cae0c66dcf73b85ba08a82ae7a580b0c4e6beabab1d81f76dc0a3893`** (51,695,616 B) |
| `kernel.json` | `cc3ff5ec4ef7bd7cad1c7d2444f3accf29381f68b1b503d53d947ed08b4ebf1d` |
| `xnu-patches.list` | `3c0af5097e65aae95729e59358b1d6f83c4e2b0146d66b98f4fc53ac7fe2af0b`, 77 entries, unchanged |
| `xnu-inputs.json` | `b113f7684f19b539cdfeb3317c2809d190b307560d57397ccb1de4e9300e10d5` |
| payload | **produced** after the budget decision: `artifacts/m5/payload-round2.img`, 993-byte command line, see "Payload: produced" below |

`xnu-patches.list` sha256 is `3c0af5097e65aae95729e59358b1d6f83c4e2b0146d66b98f4fc53ac7fe2af0b` (77 entries,
the same list round 1 recorded).

The round-1 kernel (`7be4d4f9…`, 51,695,616 B) is still inside `artifacts/m5/payload-round1.img`; it was
extracted and hashed during this round (`mcopy ::/EFI/UniXNU/Payloads/kernel.macho`).

## Build command (as root in WSL distro `unixnu`)

```sh
cd /mnt/d/rk-mac/repos/UniXNU
UNIXNU_OUT=/opt/unixnu-out ./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
UNIXNU_OUT=/opt/unixnu-out UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553 ./unixnu build kernel -D ../UniXNU-opi5plus
```

`evidence/round2-baseline/10-build-kernel.sh` is that pair plus a before/after hash and the error counts;
`build-round2.log` is its transcript. Two build-environment preconditions (no source change):

1. `prepare-xnu-source.py` restores 26 omitted Apple headers over HTTPS. Direct TLS is reset by the local
   network (`ConnectionResetError: [Errno 104]`); the proxy git already uses answers 200, so
   `http_proxy`/`https_proxy=$PROXY` (the local NAT gateway — see the host repo's `.env`) are exported for the build.
2. `unixnu` runs the archives before the final XNU compile; a `startup.o.json` compile record left by the
   previous build makes the bootstrap pass skip and archive-03 fail. The record is moved aside first.
   The bootstrap pass itself ends in a link error (`ld: symbol(s) not found`), which is expected: it only
   produces the compile records. The final compile and link then succeed.

## 0002 patch vs its generator

`python tools/wsl/935-regen-0002.py --repo <temp copy> --scratch <temp>` regenerated
`0002-mount-data-volume.patch` from `tools/wsl/331-patch-xnu-data-mount.py` against feed `d8b8029`;
the output is **byte-identical** to `patches/xnu/0002-mount-data-volume.patch` (71,306 B, sha256
`9fe0bb7a4b2a0794af7447407f6e432d20ef4a27d0acd68931bed13be375bd48`), and the old-vs-new tree diff is
empty (0 lines, 0 hunks for each of the four files). The generator and the patch are in sync; nothing
was hand-edited.

`xnu-inputs.json` (the build's own patch signature, 77 patches, source commit
`d8b80295118ef25ac3a784134bcf95cd8e88109f`):

| patch | sha256 |
| --- | --- |
| `0002-mount-data-volume.patch` | `9fe0bb7a4b2a0794af7447407f6e432d20ef4a27d0acd68931bed13be375bd48` |
| `0003-gpu-addon.patch` | `b4508068cc2cb7de09a448085052bbe6f63f38361acd1b671474fa74622aa4e2` |
| `0004-thread-sample.patch` | `ce806c0fa9d9e3f804386579d7612c3bf8e8d7ff0af3c7411ce9a7ac5ded1ac5` |

## Markers in the new kernel

`strings -a` counts on the linked kernel; the same counts on the kernels extracted from
`payload-round1.img` and `payload-stub.img` are the baseline columns, because all three carry the same
prelinked Apple kernelcache.

| marker | new | round 1 | round 5 | verdict |
| --- | --- | --- | --- | --- |
| `opi5plus_text_base` (symbol) | 0 (nm), 0 (strings) | 0 / 0 | 0 / 0 | new code **present but inlined**; see disassembly |
| `text 0x` | 0 | 0 | 0 | emitted through `PE_kputc` immediate stores, no literal; see disassembly |
| `biplanar surface accepted` | **1** | 0 | 0 | **new, present** |
| `biomed` | 1 | 1 | 1 | unchanged (0002's watch table / Apple kernelcache) |
| `callservicesd` | 19 | 19 | 19 | unchanged |
| `runningboardd` | 7 | 7 | 7 | unchanged (Apple kernelcache) |
| `contextstored` | 2 | 2 | 2 | unchanged |
| `opi5plus: stat ` | 2 | 2 | 0 | round-1 marker kept |
| `opi5plus: sysnum ` | 2 | 2 | 0 | round-1 marker kept |
| `bh %#x` | 1 | 1 | 0 | round-1 marker kept |
| `opi5plus-bt5` / `-bt8` / `-bt-secs` | 1 / 1 / 1 | 1 / 1 / 1 | 1 / 1 / 1 | boot-argument names the sampler reads |

Disassembly (`llvm-objdump -d --disassemble-symbols=_opi5plus_ubt_for_thread`, linked kernel, kernel
symbol at `0xfffffff00735c9ac`):

* `bl 0xfffffff0072ea210 <_lck_rw_try_lock_shared>` at `0xfffffff00735ca08` and
  `bl <_lck_rw_done>` at `0xfffffff00735ca54` — `opi5plus_text_base()` is compiled into the caller
  (`static`, one call site), which is why it has no symbol of its own.
* `mov w0, #0x5d` (']'), `#0x20`, `#0x74` ('t'), `#0x65`, `#0x78`, `#0x74`, `#0x20` at
  `0xfffffff00735cd2c`…`0xfffffff00735cd8c` — the `"] text "` part of the exit line.
* `mov w0, #0x30` ('0'), `mov w0, #0x78` ('x') at `0xfffffff00735cda8`/`0xfffffff00735cdb8` and 14 more
  call sites — the `0x` of `opi5plus_ubt_hex()`, inlined. The pair is what prints `text 0x…`.

The three `static` helpers and the bitfield of slot names are therefore in the binary even though
`strings` cannot see their literals, which confirms the round-1 reading of the same effect.

## Payload: blocked (not produced) — superseded by "Payload: produced" below

`tools/wsl/tmp-payload-stub.sh` (copied verbatim to
`evidence/round2-baseline/20-pack-payload-round2.sh` with only the destination paths changed) makes 56
kernel arguments once `opi5plus-bt5..bt8` are added, and the command line is **1047 bytes**:

* round-1 payload command line (read back from `payload-round1.img`): 52 arguments, 946 bytes;
* the four new slots add 26 + 19 + 26 + 26 characters + 4 spaces = 101 bytes;
* `unixnu payload` refuses more than 1000 bytes (`scripts/unixnu/payload.py:35`), because DarwinBoot
  copies the line into a 1024-byte buffer. 1047 > 1024, so it would also not boot.

`unixnu` returned rc=1 and wrote nothing; the script's `cp` then copied the **existing**
`payload.img` to the destination, which is byte-identical to `payload-round1.img`
(`66b2cf33…`). That file was deleted again — **there is no `artifacts/m5/payload-round2.img`**, and
`payload-round1.img` (`66b2cf33…`, 100,663,296 B) and `payload-stub.img` (`4e0fba78…`, 100,663,296 B)
are unchanged.

47 bytes have to go from the command line. Every argument in it is parsed by this kernel (each name is a
literal in the binary), so no argument is provably inert; the choice of which experiment to drop is not
this round's to make. Candidates and their sizes (name=value plus one space):

| to drop | bytes | line after |
| --- | --- | --- |
| `offline-legacy-default-surface=0` | 33 | 1014 (still over) |
| `offline-legacy-unit-scale=2` | 28 | 1019 (still over) |
| `offline-legacy-adopt-surface=1` | 31 | 1016 (still over) |
| `offline-legacy-default-surface=0` + `sd-clock-div=0` | 33 + 15 = 48 | 999 |
| `offline-legacy-default-surface=0` + `sheng-vsync=60` | 33 + 15 = 48 | 999 |
| `offline-legacy-default-surface=0` + `sd-watchdog=15` | 33 + 15 = 48 | 999 |
| `offline-legacy-default-surface=0` + `sd-paint=0` | 33 + 11 = 44 | 1003 (still over) |

Alternatives that need a different decision: pass the four slot names from `board.toml` instead (off
limits for this round), or keep only four slots in the payload.

## Payload: produced — `bt7` and `bt8` dropped

The decision was to keep two of the four new sampler slots: `bt5=contextstored` and `bt6=biomed`. Dropping
`bt7` and `bt8` removes 54 bytes from the 1047-byte line (the overflow was 47); every other argument of
round 1 is untouched, and `board.toml` is still not edited.

* `tools/wsl/tmp-payload-stub.sh` — the two `--kernel-arg opi5plus-bt7=callservicesd` and
  `--kernel-arg opi5plus-bt8=runningboardd` entries are removed and nothing else changes; the continuation
  backslashes are intact and `bash -n` returns rc=0. `20-pack-payload-round2.sh` is kept as the pre-decision
  snapshot (it still lists all four slots).
* Packed with `.tmp/r2-payload-round2-fixed.sh`: the text of the edited stub with only the destination paths
  changed (scratch log `/tmp/payload-round2-fixed.log`, copy to `artifacts/m5/payload-round2.img`), run as
  root in the WSL distro `unixnu`. **No kernel rebuild** — `unixnu payload` copies the file named by
  `--kernel`, and `$OUT/kernel/kernel.macho` still carries its 20:20:20 mtime and the `45fbfab3…` hash.

| item | value |
| --- | --- |
| kernel command line | **993 bytes**, 54 arguments (round 1: 946 bytes, 52 arguments) |
| line read back from the payload | identical to the build's `payload.json` `kernel_args`; `opi5plus-bt5=contextstored` and `opi5plus-bt6=biomed` present, no `opi5plus-bt7` / `-bt8`, no duplicate |
| `artifacts/m5/payload-round2.img` | 100,663,296 B (96 MiB), sha256 `80e4f2965501789a851974bd8366fd686c56aacc6bc443ceb2f3c36fc527d2e1` |
| kernel inside the payload (`mcopy ::/EFI/UniXNU/Payloads/kernel.macho`) | 51,695,616 B, sha256 `45fbfab3cae0c66dcf73b85ba08a82ae7a580b0c4e6beabab1d81f76dc0a3893` |
| `payload-round1.img` / `payload-stub.img` | unchanged, `66b2cf33…` / `4e0fba78…` |

The bracket of seven arguments was checked one by one in the `unixnu.toml` inside the payload —
`opi5plus-bt5=contextstored`, `opi5plus-bt6=biomed`, `offline-legacy-paint=0`, `sd-auto-reboot=3600`,
`sd-syscall=1`, `offline-legacy-validate=1`, `sd-data-persist=1` — all present; a set difference against the
line in `payload-round1.img` shows exactly those two additions and nothing removed. No board, no ESP or SPI
write, no kernel source touched.

## Files written by this round

* `evidence/round2-baseline/README.md` — this note.
* `evidence/round2-baseline/10-build-kernel.sh`, `build-round2.log` — the build and its transcript.
* `evidence/round2-baseline/20-pack-payload-round2.sh` — the payload script (destination paths only).
* `evidence/round2-baseline/30-verify-build.sh`, `40-verify-markers.sh`, `41-probe-tools.sh`,
  `42-verify-symbols.sh`, `43-probe-symbols.sh`, `44-verify-symbols2.sh`, `45-verify-disasm.sh`,
  `50-verify-payload.sh`, `60-args-budget.sh`, `61-args-numbers.sh`, `62-board-args.sh` — the checks quoted above.
* `evidence/round2-baseline/stash/` — the `startup.o.json` compile record moved aside before the build.
* `tools/wsl/tmp-payload-stub.sh` — edited (the `bt7` / `bt8` entries removed, nothing else); the round-2
  packing used `.tmp/r2-payload-round2-fixed.sh`, its read-back check `.tmp/r2-verify-payload-fixed.sh`.
* `artifacts/m5/payload-round2.img` — the round-2 payload (`80e4f296…`, 100,663,296 B).
* No source file, patch, `board.toml` or payload image of round 1 was modified.
