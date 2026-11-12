// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of hooks that the SoC patch series references but this board does not build yet.
//
// The SoC series (patches/xnu/series of UniXNU-rk3588-common) applies unconditionally, because it is a
// property of the RK3588 and not of one board. Two of its patches expect the eMMC host driver:
//
//   rk3588-emmc-personality.patch   adds a KernelConfigTables entry with IOClass = NativeEmmc. It names the
//                                   class only in a string, so it links without the driver.
//   rk3588-root-trust.patch         calls native_emmc_root_trusted(dev_t) from bsd_init.c. That symbol is
//                                   defined by package/Rk3588Emmc (an EVB10 submodule), which this
//                                   repository does not carry: no eMMC module is fitted
//                                   (docs/hardware.md, and the board's own tree has no card on
//                                   mmc@fe2e0000 either).
//
// Without a definition the kernel link fails with
//   Undefined symbols for architecture arm64: "_native_emmc_root_trusted", referenced from _bsd_init
//
// The definitions are WEAK, so the day M4 fits the module and CONFIG_XNU_EXTRA_LD_ARCHIVES force-loads
// the real libRk3588Emmc.a, its strong definition replaces these and nothing else has to change.
//
// Each definition behaves as the absent driver: the root volume is never vouched for, so bsd_init keeps
// require_rootauth = TRUE and a non-sealed root is refused exactly as it would be without the patch.

#include <stdint.h>

#define STUB __attribute__((weak))

// dev_t is int32_t on Darwin for the 64-bit kernel ABI (bsd/sys/_types/_dev_t.h); using int32_t here keeps
// this file independent of the SDK headers, which are not on the include path of the kernel link.
int native_emmc_root_trusted(int32_t dev);
STUB int native_emmc_root_trusted(int32_t dev)
{
  (void)dev;
  return 0;
}
