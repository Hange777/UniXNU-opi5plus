# Agent instructions

This repository owns the Orange Pi 5 Plus board integration: memory layout, the SD storage path (and the
planned eMMC one), the GOP display path, input, Mali integration, board patches and packaging. Shared RK3588
resources are in `UniXNU-rk3588-common`. Keep board facts here, SoC facts in the SoC repository, and driver
internals in the driver repositories.

Read these UniXNU pages before editing:

1. [Porting](https://unixnu.com/porting/)
2. [Device repositories](https://unixnu.com/porting/device-repository)
3. [Device trees](https://unixnu.com/porting/device-tree)
4. [Porting records](https://unixnu.com/porting/records)

Rules specific to this repository:

- Only catalog components go into `[components] include`. Board drivers live under `package/` and are
  built by a `scripts/build-opi5plus-*.py`. Do not name a script like a UniXNU script: the device scripts
  are copied into the build tree after the UniXNU ones and would replace them (`scripts/compatibility.toml`
  in the UniXNU checkout lists the names the base already uses).
- The target is Mach-O; inline assembly goes through the Darwin assembler, where `;` starts a comment.
- Patches apply with plain `git apply` in series order. Generate a device patch against a tree that has the
  ARM64, SoC and OS series applied, never against the device series.
- Do not commit firmware, vendor binaries, generated includes, IPSW or Apple content, logs, images or build
  output. List vendor inputs in `proprietary-files.txt` with their SHA-256.
- Addresses, hostnames, accounts and local paths of the machines that build and run the board belong in the
  ignored `.env`, never in the repository.
- Update `porting.toml` findings or evidence in the same change as the code. Record `device-verified` only
  with a command, a full commit and an accessible log.

## What this board is not

Three EVB10 features have no counterpart here, and code copied from that repository must be checked against
this list before it is trusted:

| EVB10 | Orange Pi 5 Plus |
| --- | --- |
| eMMC `mmc@fe2e0000`, `Rk3588Emmc` + `bind-emmc-storage.py` | **No eMMC module.** The only block device is SD `mmc@fe2c0000` (DesignWare MSHC in SD mode). An eMMC module is Plan A for M4 and needs no device-tree change. |
| DSI panel + touch, `Evb10Touch` / `Evb10Hid`, `evb10-vop*` | **HDMI only** (the firmware detects and drives it as HDMI1, `DetectDisplays: HDMI0 Not Found, HDMI1 Success`), no panel, no touch controller. Display is the firmware's GOP frame buffer; input starts with scripted injection, then USB HID. |
| 4 GiB board, `dram_size = "0x100000000"` from a single 4 GiB bank | **8 GiB, in three banks** (`0x0020_0000`, `0x1_0000_0000`, `0x2_f000_0000`). The ADT takes one contiguous range; this port declares the high 4 GiB one. Read the comment in `dts/opi5plus.dts` before changing it. |

## Boot arguments

`[payload] kernel_args` lists only arguments that something in the resolved build actually parses, and each
group names its parser. When adding one, say where it is read; when a driver arrives, the argument goes in
with it. Arguments for absent hardware are commented out with the step that will switch them on.

## Run builds from the UniXNU checkout

```sh
./unixnu defconfig opi5plus_defconfig -D ../UniXNU-opi5plus
./unixnu fetch -D ../UniXNU-opi5plus
./unixnu status --json -D ../UniXNU-opi5plus
./unixnu tree -D ../UniXNU-opi5plus
python3 scripts/porting-record.py check --device ../UniXNU-opi5plus
```

Report commands, results, required local inputs, and hardware checkpoints. Write concise English describing
current behavior.
