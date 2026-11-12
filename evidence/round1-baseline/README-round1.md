# Round 1 baseline — instrumented kernel and payload

Date: 2026-11-11 (Asia/Shanghai). Build only: no board, no ESP/SPI write, no `wsl --unregister`.
This note is the record of the round-1 build gate. It sits next to the earlier reproducibility note
(`README.md`, written before the instrumentation existed).

## Result

| item | value |
| --- | --- |
| kernel build | **clean**: `BUILD_RC=0`, 0 hits of `:[0-9]+:[0-9]+: error:` in `kernel.log`, `kernel-bootstrap.log`, every `archive-*.log` and `component-*.log` |
| `kernel.macho` sha256 | `7be4d4f9413580d6755c1f13fdbe68ad562022422abda124aa1952be553fdc91` |
| `kernel.macho` size | 51,695,616 B (round 5: 51,679,232 B, **+16,384 B**) |
| payload | `artifacts/m5/payload-round1.img`, 100,663,296 B, sha256 `66b2cf33836bcbe7211a4b474e5980596005a029d4900719d6850772042334e4` |
| kernel inside the payload | same sha256 as the build output (checked with `mtype` + `cmp`) |
| round-5 payload | `artifacts/m5/payload-stub.img` untouched, sha256 `4e0fba78…266f4` |
| `xnu-patches.list` | 77 entries, sha256 `3c0af5097e65aae95729e59358b1d6f83c4e2b0146d66b98f4fc53ac7fe2af0b` (unchanged from round 5: the list records the resolved series, not patch contents) |
| patch identities the build consumed | `xnu-inputs.json` records `0002 = a2633499…`, `0004 = db234956…`, byte-identical to the two files on disk |

## Build command (verbatim, root in WSL `unixnu`)

```sh
cd /mnt/d/rk-mac/repos/UniXNU
UNIXNU_OUT=/opt/unixnu-out ./unixnu defconfig opi5plus_display_defconfig -D ../UniXNU-opi5plus
UNIXNU_OUT=/opt/unixnu-out UNIXNU_UNIQUE_CHIP_ID=4f504935504c5553 ./unixnu build kernel -D ../UniXNU-opi5plus
```

## Build-environment conditions (not source changes)

1. `prepare-xnu-source.py` restores 26 omitted DriverKit headers from `raw.githubusercontent.com` over four
   threads. Direct TLS to that host is reset here (`ConnectionResetError: [Errno 104]`); the proxy that git
   already uses (`$PROXY`, the local NAT gateway — see the host repo's `.env`) fetches all 28 files. The build therefore ran with
   `http_proxy`/`https_proxy`/`all_proxy` exported.
2. The generated `bsd/DEVELOPMENT/Makefile` in `OBJROOT` is not refreshed when `bsd/conf/` changes, so it was
   removed once to force `doconf` to regenerate it (build output, not source).
3. `unixnu` runs the archives *before* the final XNU compile. A failed `kern_exec.c` compile makes `make`
   delete `kern_exec.o`, after which `archive-03` fails before the compile that would recreate it. The stale
   compile record `osfmk/DEVELOPMENT/startup.o.json` was moved aside so `unixnu` took its own bootstrap path
   (`XNU_BOOTSTRAP=1`, no archive checks), which compiles XNU first.

## Compile errors: 2, both fixed

Both were in patch `0002`, so both were made in the generator `tools/wsl/331-patch-xnu-data-mount.py` and the
patch was regenerated with `tools/wsl/935-regen-0002.py`. `git apply --check` passes (rc 0, with and without
`--exclude=CodeSignature`).

1. **`vm/vm_page.h` not found.**
   `bsd/kern/kern_exec.c:9163:10: fatal error: 'vm/vm_page.h' file not found`.
   The header exists in the source tree but is not exported (`EXPORT_HDRS/osfmk/vm/` has `pmap.h`,
   `vm_pageout.h`, `vm_map.h`, … but not `vm_page.h`), and bsd objects compile only against the exported set.
2. **`setbit` macro collision** (surfaced by the first, wrong fix: adding `-I$(SRCROOT)/osfmk` to
   `kern_exec.o`, which was reverted).
   `osfmk/kern/misc_protos.h:59:13: error: expected expression` / `expected ')'` — osfmk declares
   `extern void setbit(int, int *)` while `bsd/sys/param.h:210` **defines `setbit` as a macro**, so
   `<vm/vm_page.h>` can never be included from a bsd file.

Fix actually kept: `kern_exec.c` no longer includes the header. It declares
`void opi5plus_vm_page_counts(unsigned long long *, unsigned long long *, unsigned long long *)` and calls it;
the helper is appended to `osfmk/kern/debug.c` (which already reaches `vm_page.h` through
`vm_compressor.h`) and returns exactly `vm_page_free_count`, `vm_page_speculative_count` and
`VM_PAGE_COMPRESSOR_COUNT`. Printed values and their semantics are unchanged.

## Payload boot arguments (read back from the generated payload)

`unixnu payload` printed the full argument list; the three the round asked for:

| argument | value | required |
| --- | --- | --- |
| `offline-legacy-paint` | `0` | `0` ✔ |
| `sd-auto-reboot` | `3600` | `3600` ✔ |
| `sd-syscall` | `1` | `1` ✔ |

`offline-legacy-paint=0` and `sd-auto-reboot=3600` are the only edits to `tools/wsl/tmp-payload-stub.sh`.
The payload was written to the new path by a derived copy of that script whose only differences are the three
destination paths, so `payload-stub.img` could not be overwritten.

## Comparison with the round-5 kernel

`cmp -l` over the whole file reports **19,313,174 differing bytes** (the new kernel is 16,384 B longer, so
`cmp` also reports `EOF on round5-kernel.macho`). Differences are spread over almost every 1 MiB bucket
(≈9.5 M of them inside the first 16 MiB, where code and constants live) — i.e. **not** the round-5 pattern of
38 bytes (8 link timestamps + `_PrelinkKCID`) that a semantically identical rebuild produces.

That is the expected and desired outcome for this round: the diffs are substantive, so the instrumentation is
in the binary. The eight 4-byte link-time fields that round 5 identified no longer hold timestamps at their old
offsets (the insertion shifts everything after it), so the "mask equals the timestamp/KCID set" criterion of
round 5 cannot be applied to this pair; it only applies when nothing but the link clock changes.

## Marker strings in the payload kernel

Search: `strings -a` and a raw byte count on the extracted `kernel.macho`.

| marker | result |
| --- | --- |
| `opi5plus: stat ` | **hit** (2) |
| ` vfree ` | **miss** (0) |
| ` dbavail ` | **miss** (0) |
| `opi5plus: sysnum ` | **hit** (2) |
| ` age ` | hit (3) — but round 5 has 3 as well, so this one is not attributable to the new exit code by itself |
| `bh %#x` | **hit** (1); the whole literal is `opi5: ubt %s tid %#llx st %#x pri %d/%d cpu %d wait %d ev %#llx cont %#llx bh %#x wq %p` |
| `opi5plus_ubt_for_thread` | **hit** (2, as `_opi5plus_ubt_for_thread`) |

The two misses are *literals passed to `opi5plus_pe_puts()`*, and the same is true of every other string in that
beat function (`opi5plus beat `, `, procs `, ` age `). The code is present: `kern_exec.o` contains the helper
symbol `opi5plus_vm_page_counts` (twice), the local `vfree_pages`, `opi5plus_ps_thread` and the `sd-auto-reboot`
/`sd-ps` boot-arg strings, and the round-5 kernel already lacked those same `pe_puts` literals. The most likely
cause is `clang` inlining the tiny `static opi5plus_pe_puts()` and folding the constant string into immediate
stores, which leaves no contiguous literal. This was **not proven** by disassembly. Literals that reach the
binary (`printf` format strings, the sampler's format string) are all present.

Consequence for the board run: the beat line should still print `vfree/vspec/vcomp/dbavail` at runtime, but the
payload alone cannot prove it — the serial log is the check that settles it.

## Files written by this round

* `README-round1.md` — this note (the only document this round was allowed to write).
* `artifacts/m5/payload-round1.img` — the new payload; `payload-stub.img` is untouched.
* `tools/wsl/331-patch-xnu-data-mount.py`, `tools/wsl/935-regen-0002.py` — the compile-error fix.
* `repos/UniXNU-opi5plus/patches/xnu/0002-mount-data-volume.patch` — regenerated (`a2633499…`).
* `tools/wsl/tmp-payload-stub.sh` — the two boot-argument edits only.
