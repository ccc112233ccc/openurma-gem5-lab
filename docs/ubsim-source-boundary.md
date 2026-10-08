# UBSim source boundary

UBSim is the name of this lab and its stable `UBSIM_*` configuration namespace.
The executable path does not fetch, build, or link the retired compatibility
repository that was used during the first prototype.

## Current sources of truth

| Responsibility | Source |
| --- | --- |
| Linux UB/UBUS/UBASE/UBCORE/UBURMA/UDMA drivers | pinned openEuler OLK 6.6 checkout (`oe66`) |
| UMMU kernel and userspace support | pinned openEuler UMMU checkout (`deps/ummu`) |
| `liburma`, `urma_admin`, `urma_perftest`, UDMA provider | pinned openEuler UMDK checkout (`sources/umdk`) |
| guest CPU, memory and platform | pinned gem5 plus `configs/arm64_fs.py` |
| MMIO/DMA/IRQ simulator boundary | lab-owned UB-HOST protocol and thin gem5/QEMU adapters |
| device behavior | lab-owned standalone `udma-device-sim` and `udma-model` |
| link, switch, routing and queueing | pinned ns-3-UB plus lab-owned UB-NET adapter |

The official driver and provider sources are not patched to emulate hardware.
They execute normally inside the guest. Missing hardware behavior is supplied
outside the guest through UB-HOST; wire-visible frames cross UB-NET into the
ns-3 process.

## Reproducibility

`scripts/fetch-sources.sh` reconstructs the tree from public upstreams and the
reviewable gem5/UMDK bundles under `patches/source/`. There is deliberately no
legacy bundle, SystemC hardware scaffold, compatibility provider, or
in-process peer-ring backend.

Historical result documents are evidence only and are not build inputs.

For operation-level support, simplifications and missing behavior, see the
[official driver coverage audit](validation/official-driver-coverage.md).
Module packaging or successful command completion alone is not proof of
complete hardware behavior. The guest also includes the lab-owned
`ubsim_ub_v2m.ko` bridge, which is not an official UB driver.
