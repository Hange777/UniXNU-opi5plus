# M7-01 — The MaliMetal stack, built

Host result. Nothing in this file ran on the board: it records what the user-space half of the GPU
port is, how it was built on this machine, what came out, and the one thing still missing between
that output and a composited desktop.

## Why this is the next gate

`logs/boot-mali-1.log` shows the kernel half finished: power domain 12 up, GPU_ID `0xa8670005`
(arch 10.8.6, product 7), the 274432-byte firmware mapped in 7 sections, AS0 enabled, the MCU booted
and raised its interrupt, CSF interface v1.1.0 with 8 groups, the global-interface ping acknowledged
and `MaliService` registered. What the log also shows is that nothing renders: SpringBoard's Metal
clients have no driver to open, because a stock iPadOS has an AGX accelerator and this board has a
Mali-G610. The user-space half is MaliMetal: PanVK (Vulkan) cross-built for `arm64-apple-ios`, the
`darwin-drm` stand-in that turns Panthor ioctls into `MaliService` calls, and the Metal plug-in that
puts the two behind the stock clients' Metal API.

## The chain and where each input came from

| Step | Input | Source here |
| --- | --- | --- |
| 1 | LLVM/Clang 20.1.8, host x86, for `mesa_clc` / libclc / `llvm-spv` | built from `llvmorg-20.1.8.tar.gz`, `-DLLVM_TARGETS_TO_BUILD=X86` |
| 2 | libclc + `mesa_clc`, SPIRV-Tools/Headers/Translator, glslang | MaliMetal `mesa-build/fetch.sh`, ~20 min together |
| 3 | Mesa 26.1-devel + 12 PanVK patches | AOSP `external/mesa3d` at `875160ed`, because that is the tree the series was written against (Android's own host is unreachable from here; the LineageOS mirror carries the same commit) |
| 4 | Mesa host tools, then `libvulkan_panfrost.dylib` for iOS | `build-mesa-host.sh`, `build-mesa-ios.sh` with the iPhoneOS 16.5 SDK (`theos/sdks`) |
| 5 | `libnative_metal_bridge.a` (Rust) | `metal2vulkan` @ `720ef1ce` + the 37 upstream `AdrenoMetal` patches @ `d6ca655` + this board's 8 MiB pool patch |
| 6 | `NativeMaliMetal.bundle` | `build-plugin.sh`, then `make-addon.py` |

Two inputs were not where the documentation says, and both were substituted rather than skipped:

- **Mesa.** `MESA_SRC_ORIG` is "a plain copy of the Mesa tree (26.1-devel)". Upstream `main` today is
  `27.0.0-devel`, and `26.1.0` / `staging/26.1` (26.1.8) both fail patch 0004 by one hunk. The AOSP
  commit the notes name (`875160ed`) fails the same hunk — the patch's *changed* lines match exactly,
  only two lines of following context differ (`minTexelBufferOffsetAlignment` went from 64, with a
  compressed-format comment, to 16). The series therefore applies with `--fuzz=3`: fuzz relaxes
  context matching, never the change itself.
- **Toolchain.** The scripts call the toolchain by AOSP-prebuilt names (`$LLVM_BIN/clang-20`,
  `llvm-ar`, `llvm-ranlib`, `llvm-strip`), and the LLVM the Mesa chain built has no AArch64 backend.
  Every arm64-apple-ios compile here — the `darwin-drm` shim, PanVK, the bridge, the plug-in — uses
  Ubuntu's LLVM 21.1.8 through a name-compatible alias directory, so the scripts still find `clang-20`,
  `llvm-ar`, `llvm-ranlib` and `llvm-strip`. `llvm-as` comes from the same 21. The from-source LLVM 20
  stayed in place for the host tools, which is what it was built for.

## What came out

    $MESA_BUILD/build/mesa-ios/src/panfrost/vulkan/libvulkan_panfrost.dylib    16512128 B
        cdhash 0e5bef072a2f63f9c8ae8ea9d3d532ebff1300cc
    $BRIDGE_OUT/target/aarch64-apple-ios/release/libnative_metal_bridge.a      191536040 B
    metal-plugin/out/NativeMaliMetal.bundle/
        Info.plist                                                                   617 B
        NativeMaliMetal                                                       19437232 B
            cdhash 52235460ed3d218c465277dd1450bd76731445c4
        libvulkan_panfrost.dylib                                              16512128 B
    metal-plugin/out/NativeMaliMetal.addon   11708216 B, 3 files
        sha256 966cd80026d473058cc1c2eda0d387515993a5ec74049a5208be066516052835

The two cdhashes are what an owner trust cache has to carry for the bundle to execute; `build-plugin.sh`
prints them for exactly that reason.

## What is still missing: the kernel has to deliver the addon

`unixnu build payload` accepts `--gpu-addon FILE` and puts it in `/chosen/memory-map/GpuAddon`, and
`make-addon.py` writes the `NGPUADD2` format that option validates. The receiving half is a kernel
patch this port does not have: `evb10-userland-mounts.patch` is where the EVB10 reads GpuAddon,
writes the files to `/private/var/ngpu/addons/` and union-mounts that directory over
`/System/Library/Extensions`, which is where the Metal loader looks.

Its shape, measured here rather than assumed: **one file** (`bsd/kern/kern_exec.c`) and **two hunks**
— a 10-line call site under `#if DEVELOPMENT || DEBUG` and a 2263-line addition at the end of the
file. It applies to this port's prepared XNU work tree with `--fuzz=3` (hunk 1 lands at 8060, hunk 2
at 8285 with a 14-line offset; our `0002-mount-data-volume` is what moved the context).

It cannot be vendored unchanged, which is what the reference port does, because it is a board's whole
userland hook rather than just the addon: APFS mounts, the SystemOS union, prefs seeding, `ps`, thread
dumps, screen shots and the exit trace, and its Data-volume half calls `native_emmc_*` — this board
has no eMMC driver and mounts its Data volume through `native_sd_data_device` already (patch 0002).
The next step is to port the part that matters: GpuAddon out of the device tree, the `NGPUADD2`
unpack, the file writes under `/private/var/ngpu/addons`, and the union mount over
`/System/Library/Extensions`, wired into the existing exec hook instead of a second Data mount.

## Commands

    # the whole build, in the order that works here
    bash mesa-build/fetch.sh && J=16 bash mesa-build/build-llvm.sh
    bash mesa-build/build-clc.sh && bash mesa-build/build-spirv-glslang.sh && bash mesa-build/build-libdrm.sh
    bash mesa-build/prepare-mesa-src.sh          # 12 patches, --fuzz=3
    bash mesa-build/build-mesa-host.sh && bash mesa-build/build-mesa-ios.sh
    bash metal-plugin/m2v-patches/apply.sh && bash metal-plugin/build-bridge.sh
    bash metal-plugin/build-plugin.sh && python3 metal-plugin/make-addon.py
