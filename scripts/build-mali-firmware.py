#!/usr/bin/env python3
"""Kernel-step archive entry: turn .cache/firmware/mali_csffw.bin into mali_csffw.inc.

Mali.cpp carries the CSF firmware as a C initialiser list, so this has to run before
scripts/build-opi5plus-drivers.py Mali and before the kernel link.

The firmware is a vendor binary (274432 bytes; this board's copy is the Rockchip BSP's
/lib/firmware/mali_csffw_g15p0/mali_csffw.bin, sha256
ad91511a77fa110af46a2a29cad9e225524d9cfa9fe35bdce96606d354e607de -- NOT the EVB10's build) and is
neither committed nor embedded in any repository. Put it in .cache/firmware/ first; the digest and its
provenance are in proprietary-files.txt.

    scripts/build-mali-firmware.py                    # .cache/firmware/mali_csffw.bin
    MALI_CSFFW=/path/to/mali_csffw.bin scripts/build-mali-firmware.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# package/ in the device repository, kits/opi5plus/ in the build tree: the same alias
# build-opi5plus-drivers.py documents. The generated .inc lands next to the generator, which is where
# Mali.cpp's #include "mali_csffw.inc" looks for it.
GENERATOR = "tools/gen-fw-inc.py"
mali_dir = next((p for p in (ROOT / "package/Mali", ROOT / "kits/opi5plus/Mali")
                 if (p / GENERATOR).is_file()), None)

if mali_dir is None:
    sys.exit(
        f"neither {ROOT}/package/Mali/{GENERATOR} nor {ROOT}/kits/opi5plus/Mali/{GENERATOR} exists: "
        "initialise the package/Mali submodule"
    )

env_fw = os.environ.get("MALI_CSFFW")
firmware = Path(env_fw) if env_fw else ROOT / ".cache/firmware/mali_csffw.bin"
if not firmware.is_file():
    sys.exit(
        f"{firmware} is missing: copy the Rockchip BSP's "
        "vendor/rockchip/common/gpu/MaliG610/firmware/mali_csffw.bin (or the board's "
        "/lib/firmware/mali_csffw_g15p0/mali_csffw.bin) to .cache/firmware/ -- see proprietary-files.txt"
    )

subprocess.run([sys.executable, str(mali_dir / GENERATOR), str(firmware)],
               check=True, env={**os.environ, "MALI_CSFFW": str(firmware)})
