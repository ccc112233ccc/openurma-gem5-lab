# QEMU UB-HOST adapter

The QEMU integration is intentionally a host adapter only.  It translates a
16 MiB SysBus MMIO aperture, guest-physical DMA, and three GIC interrupt lines
to the same UB-HOST v1 process protocol used by gem5.  WQE decoding, queue
state, RMA, packetization, port selection, and ns-3 timing stay in the existing
standalone processes.

The first supported host is Apple Silicon macOS with QEMU 11.1.1.  TCG is the
reference mode; HVF uses the same device and is an optional acceleration mode.
The initial adapter deliberately runs UB-HOST synchronization disabled.  It
does not yet implement QEMU lifecycle-fence control or coordinated snapshots.

Build and launch the verified single-node probe with:

```bash
./lab build qemu
./lab start-qemu --help
```

`start-qemu` uses TCG, GICv2, a Cortex-A72 CPU model, and the unchanged
official OLK/OpenURMA userspace and kernel stack.  The command launches a
standalone UDMA process and terminates its network interface with the contract
peer; it is meant to verify discovery/MMIO/DMA/driver probe interactively.
The expected terminal evidence is `/dev/uburma/udma0` plus four ACTIVE EIDs in
`urma_admin show`.

Apple HVF itself is available in the same QEMU binary, but QEMU HVF only
supports GICv3.  The current simulation glue publishes the official UBUS MSI
domain over GICv2m, so the official UDMA path intentionally remains on TCG
until the bridge has an ITS-backed implementation.  This is an interrupt
topology limitation, not a UB-HOST transport limitation.
