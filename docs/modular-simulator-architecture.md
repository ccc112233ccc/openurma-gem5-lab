# Modular UB simulation architecture

## Objective

The lab is moving from a gem5-specific device implementation to the same
separation principle used by SimBricks: host simulators, device simulators, and
network simulators are independent processes joined by narrow, timestamped
interfaces. The official guest driver and UMDK remain unchanged. Hardware
behavior belongs in a reusable UDMA model rather than in a gem5 or QEMU
adapter.

```text
 official UMDK + kernel UDMA driver          official UMDK + kernel UDMA driver
                 |                                           |
       guest MMIO / DMA / IRQ                       guest MMIO / DMA / IRQ
                 |                                           |
        gem5 or QEMU adapter                        gem5 or QEMU adapter
                 | UB-HOST                                  | UB-HOST
          UDMA device process                         UDMA device process
                 | UB-NET                                   | UB-NET
                 +------------ ns-3-UB fabric ---------------+
```

This is a target architecture and a migration boundary, not a claim that the
old monolithic `NICTopologySC` implementation has already disappeared.

## Responsibility boundaries

### Host simulator adapter

The gem5 and QEMU adapters expose only generic host/device operations:

- MMIO read and write requests;
- guest physical or I/O virtual DMA reads and writes;
- interrupt raise, lower, or pulse;
- simulator time and synchronization metadata.

They must not decode UDMA WQEs, choose an egress UB port, synthesize CQEs, or
implement RMA semantics. Those are device behaviors.

The first gem5 implementation is
`integrations/gem5/ub_host_adapter/UbHostAdapter.*`. It is a native gem5
`DmaDevice`: guest PIO becomes `UB-HOST` MMIO, device-originated requests use
gem5's DMA port, and protocol interrupts drive an `ArmInterruptPin`. It has no
UDMA descriptor or packet logic. The reproducible gem5 builder overlays this
source into its generated EXTRAS tree, leaving the pinned OpenURMA checkout
unchanged.

This adapter currently represents the new functional control path and keeps
UB-HOST synchronization disabled. It is not yet selected by the default
full-system configuration: the official-driver discovery/register map still
needs to be extracted from `NICTopologySC` into the standalone model first.
This explicit gate prevents silently replacing a bootable official-driver
configuration with the temporary extraction descriptor.

### UDMA device model

`components/udma-model/` owns the hardware-visible behavior:

- BAR/register and doorbell semantics;
- WQE fetch and decode;
- queue, Jetty, TP, memory-key, and completion state;
- DMA scheduling and CQE production;
- interrupt policy;
- packetization, port selection, receive processing, and retransmission.

The core is standard C++17 and contains no gem5, QEMU, or ns-3 type. A single
device process can therefore be paired with either host simulator.

### Network simulator adapter

`UB-NET` carries wire-visible UB frames or flits, link state, credits, and
timestamps. It intentionally does not carry WQEs or convenient high-level
READ/WRITE/SEND transactions. Routing, serialization, switch arbitration,
queuing, congestion, and link errors belong to ns-3-UB or another network
model.

## Protocols

The versioned headers are under `protocol/`:

- `UB-HOST v1`: device discovery metadata, MMIO, DMA completions/requests, and
  interrupts;
- `UB-NET v1`: frames, link state, credits, EIDs, ports, and traffic class.

Both protocols use the SimBricks base queue ABI: every 64-byte control header
places the virtual timestamp at byte 48 and the ownership/type byte at byte 63.
Payload follows the header in the same queue entry. The pinned upstream
revision is recorded in `SOURCE_REVISIONS.md`.

## Executable contracts

The first extracted slice deliberately tests causality instead of performance
fitting:

```text
doorbell MMIO
  -> DMA read descriptor
  -> DMA read payload
  -> emit UB-NET frame
  -> DMA write completion entry
  -> raise interrupt
```

Run it with:

```bash
./lab --runtime native build udma-model
```

The temporary 32-byte descriptor in this test is an extraction harness, not a
new guest ABI. It will be replaced by the official UDMA WQE decoder moved out
of `NICTopologySC`. Keeping that distinction explicit prevents a test-only
format from becoming accidental architecture.

`components/udma-device-sim/` is the corresponding standalone process. It
listens on two independent SimBricks interfaces, exchanges typed introduction
records, and drives the same core through `UB-HOST` and `UB-NET`. Its contract
test launches three actual processes:

```text
mock host process <-> UDMA device process <-> mock network process
```

The host peer owns guest memory and services DMA. The network peer sees only a
frame. The test proves the complete Doorbell→DMA→frame→CQE→IRQ chain crosses
both process boundaries:

```bash
./lab --runtime native build udma-device
ctest --test-dir artifacts/udma-device-sim-build --output-on-failure
```

The small portability translation unit compiles the pinned upstream SimBricks
base implementation directly. On macOS it supplies equivalents for Linux-only
`accept4` and `MAP_POPULATE`; it does not replace the shared-memory queue,
ownership, timestamp, or synchronization implementation.

## Migration sequence

1. Freeze and test `UB-HOST`/`UB-NET` layouts and the simulator-neutral core.
2. Run the core as an independent process over SimBricks shared-memory queues
   using mock host and network peers. **Complete.**
3. Replace one gem5 control path at a time: discovery/MMIO, DMA, interrupt,
   SEND, then READ/WRITE and receive queues. The thin MMIO/DMA/IRQ adapter is
   implemented; migration of the official register/WQE behavior is in progress.
4. Connect ns-3-UB at the frame boundary and remove transaction shortcuts.
5. Add a QEMU adapter that implements the same `UB-HOST` protocol; the device
   and network processes remain unchanged.
6. Remove the duplicated hardware behavior from `NICTopologySC` only after
   official-driver equivalence tests pass.

This ordering keeps every checkpoint bootable and makes regressions attributable
to one boundary at a time.
