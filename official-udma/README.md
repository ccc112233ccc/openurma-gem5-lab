# Official UDMA bring-up track

This directory contains the isolated bring-up path for running the OLK UDMA
kernel driver without modifying its source.  The existing
`openurma_ubcore.ko` path remains the functional reference until every gate in
this track passes.

## Frozen source baseline

| Component | Revision |
| --- | --- |
| gem5 | `b1a44b89c7bae73fae2dc547bc1f871452075b85` (`v24.0.0.1`) |
| OLK 6.6 | `5078a3a23a1e1825ec136485173ec98668cdd640` |
| OpenURMA | `0ae5dce300154d761f97095864bda0cf2546b265` |
| vendored UMDK | `4eab3e4ad170b06bfe5d5c1014341e81edb9bf58` |

The working trees contain the current laboratory changes.  They are inputs to
the baseline and must not be reset as part of official-driver bring-up.

## Milestone gates

| Gate | Required observable result | Status |
| --- | --- | --- |
| G0 | Existing dual-node functional profile still passes | existing baseline |
| G1 | Official `udma.ko` and its required lower stack build for AArch64 | passed |
| G2 | A simulated UBASE auxiliary device named `ubase.udma.0` is discovered | passed |
| G3 | Unmodified `udma.ko` completes `probe()` and registers a ubcore device | passed; `udma0` and `/dev/uburma/udma0` persist |
| G4 | `urma_admin show` reports the official UDMA device and EID | passed; configurable EID and 400-Gb/s port report `ACTIVE` |
| G5 | Context, token, segment, JFC/JFS/JFR/Jetty creation succeeds | passed; stock provider also receives and activates process-scoped TPs |
| G6 | Two-node `urma_perftest send_lat` completes through official `udma.ko` | pending |

Passing a build or module-load gate is not counted as data-plane support.

## Hardware contract boundary

The official driver is an auxiliary driver matching `ubase_core.udma`.  It
does not probe the current OpenURMA MMIO device directly.  Its parent must
provide four groups of services:

1. **Discovery and resources**: auxiliary device, UBASE capabilities, UDMA
   resource ranges, QoS layout and doorbell/resource-space mappings.
2. **Control path**: firmware command input/output, mailbox context commands,
   control-queue requests, activation and reset callbacks.
3. **Memory translation**: IOMMU SVA/KSVA enablement, UMMU bind/unbind, TID
   allocation, grant/revoke and invalidation.
4. **Events and data path**: CEQ/AEQ/CRQ callbacks, USI interrupts, EID/link
   notifications, queue doorbells and DMA.

`contract.tsv` is generated from the pinned kernel source by
`analyze_contract.sh`.  It is an inventory, not a claim that a service is
implemented.

The current source inventory contains 40 external calls: 6 discovery/resource,
7 control, 13 event and 14 translation services.

## Kernel configuration

`kernel.fragment` enables the official modules needed for the first build
gate.  It deliberately leaves CDMA, OBMM and Sentry disabled until G6 is
stable.  The fragment is merged into the container's case-sensitive OLK tree;
the macOS checkout must not be used for a kernel build.

Run the reproducible targeted build inside the existing ARM64 build container:

```sh
docker exec openurma-repro-20260909 \
  bash /workspace/openurma-gem5-lab/official-udma/build_modules.sh
```

The script links the kernel image first because UBUS and part of UMMU are
built-in, then performs module `modpost` in dependency order.  It does not edit
the official driver sources.

`hisi_ubus.ko` is also built from its unmodified source.  That source uses IRQ
APIs without including `linux/interrupt.h`, so the isolated build injects the
missing kernel header with `KCFLAGS=-include`; no official source file is
patched.

## Runtime bring-up status (2026-09-17)

The `--official-udma-discovery` gem5 mode now provides an evidence-driven
subset of the real UBC hardware contract:

- UBIOS root and UBC tables exposed at the modeled MMIO aperture;
- official `ubfi.ko` discovery, resource creation and IRQ registration;
- official `hisi_ubus.ko` SQ/RQ/CQ DMA queues;
- four successful config-message reads of the root-controller GUID;
- topology-query TLVs for an integrated root controller and one
  `CC08:A001` URMA endpoint;
- successful NA_CFG responses for both entities;
- token query and three 1 MiB endpoint resource windows;
- unchanged `ummu-core.ko`, `ubase.ko`, and `udma.ko` module loading.

The latest verified trace is in
`run-official-resource-v3-20260916/`.  Both devices appear under
`/sys/bus/ub/devices`, and the endpoint resources are allocated at
`0x2d100000`, `0x2d200000`, and `0x2d300000`.

The UBASE command queue is now modeled: the unchanged driver completes its
firmware query and reports version `1.0.0.0`.  The next integration boundary
is the USI interrupt parent.  gem5's VExpress platform contains GICv2m
hardware, but its kernel driver publishes only PCI and platform MSI domains;
UBUS intentionally requests a separate `DOMAIN_BUS_UB_MSI` domain.

`official-udma/ub_v2m_bridge/` is simulation-only integration glue that
publishes that missing domain over the existing GICv2m parent.  It does not
change the official UBUS, UBASE or UDMA sources.  The generated device tree
advertises the existing GICv2m frame and connects the UBC through
`msi-parent`.  This bridge must load after `ubus.ko` and before `ubase.ko`.

The validated bridge and device-model trace now reaches:

```text
ubase 00002: (pid 86) The firmware version is 1.0.0.0
ub_msi_domain_set_desc, arg->hwirq: 0
openurma: emitted UE2UE CtrlQ response service=4 opcode=0x2
openurma: raised Type-1 MSI address=0x2c1c0040 data=0x100
ubase 00002: failed to alloc iova slot, cmd = 0x0, size = 262144
```

The CtrlQ evidence is in `run-official-ctrlq-v6-20260917/`.  Mailbox status,
EQC, QoS discovery, the parent GIC interrupt path, and control-plane
notification have completed.

The unmodified full `ummu.ko` now runs against the architecture-generic
register/queue model. In `run-official-ummu-v16b-20260917/`, the complete
official lower stack probes, `udma0` reports its configured EID and an ACTIVE
400-Gb/s port, and stock UMDK creates two Jettys. In
`run-official-ummu-v18-20260917/`, the model also returns TP lists and activates
both process-scoped TPs. The next observed boundary is the official provider's
mmap-backed SQ doorbell/WQE data path. Earlier UMMU bring-up evidence included:

```text
ummu ummu.0: features 0x002381ac, options 0x00000000.
ummu ummu.0: ummu register to ummu core successful!
```

UBUS entity attachment, default BI/decoder discovery, UBASE auxiliary-device
creation and official UDMA probe are now passed gates. The existing functional
data-plane baseline remains unchanged throughout this bring-up.

The untagged OpenURMA implementation snapshot `1f58c45` decodes the official
JFC/JFR/JFS/Jetty mailbox contexts, reconstructs their queue and doorbell IOVAs,
and routes official queue DMA through UMMU into the existing UDMA execution
engine. It also provides an `atomic_fast` CPU mode for functional bring-up.
This is compiled implementation, not a passed gate: G6 remains pending until
an official-provider SQ doorbell is observed and the corresponding CQE is
consumed by `urma_perftest`.
