# OpenURMA source dependency boundary

The lab currently fetches `bojieli/OpenURMA`, but the new modular simulator is
not architecturally coupled to OpenURMA's original SystemC device model.  Three
different source roles are easy to confuse:

| Role | Current source | What the lab uses |
| --- | --- | --- |
| Guest kernel drivers | pinned openEuler OLK 6.6 (`oe66`) | UBUS, UMMU, UBASE/UBCORE, UBURMA and UDMA kernel modules |
| Guest user space | official openEuler UMDK git repository | `liburma`, `urma_admin`, `urma_perftest`, and provider code |
| Historical integration tree | pinned `bojieli/OpenURMA` checkout plus the lab bundle | original gem5 scaffold, legacy provider/tests, and a gitlink location for UMDK |
| Lab-owned hardware simulation | this repository | UB-HOST/UB-NET protocols, standalone UDMA model, gem5/QEMU adapters, and ns-3 UB fabric adapter |

## Direct uses today

`sources/OpenURMA` is still a build-time dependency in the following places:

1. `scripts/build/build_gem5.sh` imports the original gem5 scaffold and its
   `NICTopologySC` sources.  This is the compatibility/integrated path while
   the gem5 front end is being moved fully behind UB-HOST.
2. `configs/single_node_fs_openurma.py` derives from the original full-system
   machine configuration in that scaffold.
3. The official UMDK checkout currently lives at
   `sources/OpenURMA/integration/umdk/vendor/umdk`.  It is a separate upstream
   Git repository pinned by its own commit and bundle; OpenURMA is only its
   directory container here.
4. The initramfs build retains the legacy OpenURMA provider, kernel helper and
   smoke-test sources for compatibility and A/B comparison.
5. The legacy in-process UB switch includes the old adapter protocol header.
   The modular `udma-device-sim` plus ns-3 UB-NET path does not need that switch.

The fetched OpenURMA target is deliberately not a pristine public baseline.
`patches/source/openurma.bundle` reproduces the lab integration commit recorded
in `SOURCE_REVISIONS.md`.  That bundle contains the historical simulator work;
it must not be described as official driver code.

## What does not come from OpenURMA

- Official kernel behavior is compiled from the pinned openEuler kernel and
  UMMU repositories.
- Official user-space URMA behavior is compiled from the pinned UMDK repository.
- Device MMIO, DMA, interrupt, SQ/CQ, transport and network behavior in the new
  architecture is implemented by this lab's simulator-neutral model and thin
  simulator adapters.
- ns-3 owns fabric-visible packet serialization, link/switch delay, routing and
  queueing; QEMU and gem5 own guest execution and guest memory access.

Consequently, running the official driver stack does not mean the original
OpenURMA SystemC hardware model is being used.  On the modular QEMU path it is
not used at all.

## Planned decoupling

The dependency can be reduced without changing the guest ABI:

1. fetch UMDK directly into `sources/umdk`;
2. move the maintained full-system base configuration into this repository;
3. finish the gem5 UB-HOST cutover and remove `NICTopologySC` from the modular
   build;
4. keep the OpenURMA checkout only as an optional legacy/reference target.

After those steps, QEMU and modular gem5 experiments will require OLK, UMDK,
UMMU, the lab model, and ns-3-UB—but not the OpenURMA repository itself.
