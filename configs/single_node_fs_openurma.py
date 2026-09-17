# SPDX-License-Identifier: Apache-2.0
"""Compatibility wrapper for OpenURMA's clean full-system configuration."""

import importlib.util
import os
from pathlib import Path

from m5.objects import ArmSystem, GenericTimerMem
from m5.util import addToPath
from m5.util.fdthelper import (
    FdtNode,
    FdtProperty,
    FdtPropertyStrings,
    FdtPropertyWords,
)


# The upstream research scaffold names the author's original gem5 checkout.
# Seed the import path from this lab's own location before loading it so the
# same config works in Docker Desktop or a native Linux checkout.
LAB_ROOT = Path(os.environ.get(
    "OPENURMA_LAB_ROOT", str(Path(__file__).resolve().parents[1])
))
GEM5_CONFIGS = LAB_ROOT / "gem5" / "configs"
addToPath(str(GEM5_CONFIGS))
addToPath(str(GEM5_CONFIGS / "example" / "arm"))


OPENURMA_ROOT = Path(
    os.environ.get("OPENURMA_ROOT", str(LAB_ROOT.parent / "OpenURMA"))
)
UPSTREAM = OPENURMA_ROOT / (
    "eval/twonode/gem5_scaffold/configs/single_node_fs_clean.py"
)
UNSAFE_EARLY_EL2_FEATURES = {"FEAT_HCX", "FEAT_SME"}
OFFICIAL_UDMA_UBRT = 0x2D010000
# SPIs 100..103 are the VExpress PCI INTx range and are occupied when the
# dual-node OOB e1000 is present.  The matching UBIOS entry is emitted by the
# OpenURMA model with this otherwise-unused platform SPI.
OFFICIAL_UDMA_UBC_IRQ = 104
OFFICIAL_UDMA_V2M_BASE = 0x2C1C0000
OFFICIAL_UDMA_V2M_SIZE = 0x1000
OFFICIAL_UDMA_V2M_SPI_BASE = 256
OFFICIAL_UDMA_V2M_SPI_COUNT = 64


spec = importlib.util.spec_from_file_location("openurma_fs_upstream", str(UPSTREAM))
upstream = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(upstream)

upstream_create = upstream.create


def _omit_mmio_timer_from_dtb(_timer, _state):
    """Keep the device modeled, but do not advertise its unconfigured frames."""
    return iter(())


def _official_udma_device_tree(original):
    def generate(system, state):
        root = original(system, state)

        chosen = FdtNode("chosen")
        chosen.append(FdtPropertyWords(
            "linux,ubios-information-table", state.addrCells(OFFICIAL_UDMA_UBRT)
        ))
        root.append(chosen)

        # VExpress_GEM5_V1 already instantiates this GICv2m frame in the
        # hardware model, but gem5 does not emit a DT node for it.  Advertise
        # the existing frame so the guest can create an MSI parent domain.
        v2m = FdtNode(f"msi-controller@{OFFICIAL_UDMA_V2M_BASE:x}")
        v2m.append(FdtPropertyStrings(
            "compatible", ["arm,gic-v2m-frame"]
        ))
        v2m.append(FdtProperty("msi-controller"))
        v2m.append(FdtPropertyWords(
            "reg",
            state.addrCells(OFFICIAL_UDMA_V2M_BASE)
            + state.sizeCells(OFFICIAL_UDMA_V2M_SIZE),
        ))
        v2m.append(FdtPropertyWords(
            "arm,msi-base-spi", [OFFICIAL_UDMA_V2M_SPI_BASE]
        ))
        v2m.append(FdtPropertyWords(
            "arm,msi-num-spis", [OFFICIAL_UDMA_V2M_SPI_COUNT]
        ))
        v2m.appendPhandle(system.realview.gicv2m)
        root.append(v2m)

        ubc = FdtNode("ubc@0")
        ubc.append(FdtPropertyStrings("compatible", ["ub,ubc"]))
        ubc.append(FdtPropertyWords("index", [0]))
        ubc.append(FdtPropertyWords(
            "msi-parent", [state.phandle(system.realview.gicv2m)]
        ))
        ubc.append(FdtPropertyWords(
            "interrupts", [0, OFFICIAL_UDMA_UBC_IRQ - 32, 4]
        ))
        root.append(ubc)

        # UBFI matches this placeholder by index, then supplies the MMIO
        # resource and firmware metadata from the UBIOS UMMU table before the
        # unchanged official ummu.ko probes it.
        ummu = FdtNode("ummu@0")
        ummu.append(FdtPropertyStrings("compatible", ["ub,ummu"]))
        ummu.append(FdtPropertyWords("index", [0]))
        ummu.append(FdtPropertyWords(
            "msi-parent", [state.phandle(system.realview.gicv2m)]
        ))
        root.append(ummu)
        return root

    return generate


def create_olk66_compatible(args):
    # VExpress_GEM5_V1 exposes both the architected CP15 timer and optional
    # memory-mapped timer frames.  The latter require secure firmware to grant
    # non-secure CNTACR access.  Our direct-kernel boot has no such firmware;
    # depending on the boot state, OLK 6.6 can therefore leave common timer
    # initialization pending on a frame that it can never use and fall back to
    # the 250 Hz jiffies clock.  The CP15 timer is complete and sufficient for
    # this SMP guest, so omit only the unusable MMIO DT node.
    original_timer_dtb = GenericTimerMem.generateDeviceTree
    original_system_dtb = ArmSystem.generateDeviceTree
    GenericTimerMem.generateDeviceTree = _omit_mmio_timer_from_dtb
    if args.official_udma_discovery:
        ArmSystem.generateDeviceTree = _official_udma_device_tree(
            original_system_dtb
        )
    try:
        system = upstream_create(args)
    finally:
        GenericTimerMem.generateDeviceTree = original_timer_dtb
        ArmSystem.generateDeviceTree = original_system_dtb
    timer_message = "DT advertises CP15 timer only"
    kept = []  # Preserve every extension that direct EL2 boot can expose.
    removed = []
    for extension in system.release.extensions:
        name = getattr(extension, "value", str(extension))
        if name in UNSAFE_EARLY_EL2_FEATURES:
            removed.append(name)
        else:
            kept.append(extension)
    system.release.extensions = kept
    print("[openurma-fs-wrapper] disabled early-EL2 features: " +
          ", ".join(sorted(removed)))
    print(f"[openurma-fs-wrapper] {timer_message}")
    if args.official_udma_discovery:
        print("[openurma-fs-wrapper] official UDMA discovery DT enabled; "
              f"UBRT=0x{OFFICIAL_UDMA_UBRT:x}")
    return system


upstream.create = create_olk66_compatible
upstream.main()
