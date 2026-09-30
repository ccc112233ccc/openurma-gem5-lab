# Official openEuler UDMA stack

This directory contains build inputs and retained evidence for running the
unmodified openEuler UB stack in the full-system guest.

## Software boundary

- OLK 6.6 supplies UBUS, UMMU, UBASE/UBCORE, UBURMA and UDMA kernel code.
- UMDK supplies `liburma`, `urma_admin`, `urma_perftest`, and the stock UDMA
  provider.
- The lab supplies only the absent hardware: discovery resources, MMIO
  registers, firmware/control queues, DMA, interrupts, queue execution and the
  UB network.
- `ub_v2m_bridge/` is simulation-only integration glue that exposes the
  required UB MSI domain over gem5's GICv2m parent. It does not modify an
  official driver source file.

The executable path has no dependency on the retired prototype repository and
no fallback provider or kernel module.

## Validated gates

The retained evidence covers:

1. official module build and device discovery;
2. `udma.ko` probe, `udma0`, `/dev/uburma/udma0`, EID, and ACTIVE ports;
3. context, token, segment, JFC/JFS/JFR/Jetty, TP and aggregation resources;
4. two-node CTP SEND/RECV;
5. RMA READ/WRITE and bidirectional bandwidth paths;
6. multi-port TP-context routing;
7. modular gem5/QEMU host adapters, standalone UDMA devices, and ns-3 fabric.

See `contract.tsv` for the driver-to-hardware service inventory and the
`*-evidence.md` files for captured commands and output. Passing module loading
alone is never counted as a data-plane result.

## Build and run

```sh
./lab setup --jobs 8
./lab build initramfs
./lab start --nodes 2 --profile fast --provider official
./lab status
./lab attach 0
```

Inside the guest:

```sh
urma_admin show
```

The initramfs is produced as `out/official-udma.cpio.gz`. Source revisions are
recorded in `SOURCE_REVISIONS.md`; the official drivers and provider are built
from `oe66`, `deps/ummu`, and `sources/umdk` respectively.
