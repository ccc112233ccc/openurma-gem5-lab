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
| G2 | A simulated UBASE auxiliary device named `ubase_core.udma` is discovered | in progress: official UBUS entities and resources pass; UBASE command queue pending |
| G3 | Unmodified `udma.ko` completes `probe()` and registers a ubcore device | pending |
| G4 | `urma_admin show` reports the official UDMA device and EID | pending |
| G5 | Context, token, segment, JFC/JFS/JFR/Jetty creation succeeds | pending |
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

## Runtime bring-up status (2026-09-16)

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

The current boundary is the UBASE device command queue.  `ubase` binds to the
endpoint and initializes its UBUS resources, but the first firmware-version
command receives no modeled completion and returns `-52`:

```text
ubase 00002: failed to query fw version, ret = -52.
ubase 00002: failed to init cmd queue, ret = -52.
ubase 00002: failed to init ubase dev, ret = -52.
```

Consequently `ubase_core.udma` is not created yet.  The official `udma.ko`
module is loaded but has no auxiliary device to probe, so G2 and G3 remain
open.  The next implementation step is the UBASE command-queue
register/descriptor/completion contract, followed by capability reporting and
auxiliary-device creation.  The existing functional data-plane baseline is
kept unchanged throughout this bring-up.
