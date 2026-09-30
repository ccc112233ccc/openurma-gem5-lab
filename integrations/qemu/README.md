# QEMU UB-HOST adapter

The QEMU integration is intentionally a host adapter only.  It translates a
16 MiB SysBus MMIO aperture, guest-physical DMA, and three GIC interrupt lines
to the same UB-HOST v1 process protocol used by gem5.  WQE decoding, queue
state, RMA, packetization, port selection, and ns-3 timing stay in the existing
standalone processes.

The first supported host is Apple Silicon macOS with QEMU 11.1.1. TCG is the
reference mode. Functional mode runs without a common virtual clock; timing
mode combines QEMU icount with pairwise timestamp/SYNC messages on every
QEMU↔UDMA and UDMA↔ns-3 boundary.

Build and launch the verified single-node probe with:

```bash
./lab build qemu
./lab start-qemu --help
```

`start-qemu` uses TCG, GICv2, a Cortex-A72 CPU model, and the unchanged
official openEuler OLK and UMDK userspace/kernel stack. The command launches a
standalone UDMA process and terminates its network interface with the contract
peer; it is meant to verify discovery/MMIO/DMA/driver probe interactively.
The expected terminal evidence is `/dev/uburma/udma0` plus four ACTIVE EIDs in
`urma_admin show`.

The two-node functional path reuses exactly the same UB-HOST device adapter and
connects each QEMU guest to its own UDMA process.  Both UDMA network sides join
one native ns-3 UB-NET fabric, while a separate QEMU socket network provides
the TCP setup/control channel:

```bash
./lab --runtime native build udma-device
./lab --runtime native build ns3ub
./lab start-qemu-dual
./lab status-qemu
./lab attach-qemu 0
./lab attach-qemu 1
```

This path has been validated with the unchanged official stack: both guests
report `udma0` with four ACTIVE EIDs, the OOB network passes bidirectional
traffic, and a two-sided 128-byte CTP/RM `urma_perftest send_lat` completes
through the standalone UDMA and ns-3 processes.  Synchronization is off, so
the reported microseconds are host-scheduling observations and are not a
modeled latency result.

For deterministic timing, synchronize all five processes from virtual time
zero:

```bash
UBSIM_PEER_LATENCY_NS=500 ./lab start-qemu-dual --timing
```

`--timing-strict` is an alias for compatibility. Timed MMIO reads suspend the
TCG vCPU, allow the conservative clock to advance to the device completion,
then restore and retry the exact guest load instruction. The MMIO region opts
out of QEMU's normal reentrancy guard because this protocol deliberately exits
the callback through TCG's longjmp path, matching the SimBricks QEMU adapter.

The 500-ns reference run boots both unchanged official UDMA stacks and scans
bidirectional `write_bw` from 2 bytes through 1 MiB. It measured about 58 us
for 128-byte `send_lat`, 315 MB/s at 4 KiB, and a 553 MB/s large-message
plateau. The plateau is currently set by serialized external UDMA fragments,
host DMA transactions, and adapter crossings rather than the configured
400-Gbit/s ns-3 link. Treat it as a deterministic model baseline and trend,
not as the final device-throughput calibration.

Apple HVF itself is available in the same QEMU binary, but QEMU HVF only
supports GICv3.  The current simulation glue publishes the official UBUS MSI
domain over GICv2m, so the official UDMA path intentionally remains on TCG
until the bridge has an ITS-backed implementation.  This is an interrupt
topology limitation, not a UB-HOST transport limitation.
