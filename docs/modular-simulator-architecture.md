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

## First executable contract

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

## Migration sequence

1. Freeze and test `UB-HOST`/`UB-NET` layouts and the simulator-neutral core.
2. Run the core as an independent Linux process over SimBricks shared-memory
   queues using mock host and network peers.
3. Replace one gem5 control path at a time: discovery/MMIO, DMA, interrupt,
   SEND, then READ/WRITE and receive queues.
4. Connect ns-3-UB at the frame boundary and remove transaction shortcuts.
5. Add a QEMU adapter that implements the same `UB-HOST` protocol; the device
   and network processes remain unchanged.
6. Remove the duplicated hardware behavior from `NICTopologySC` only after
   official-driver equivalence tests pass.

This ordering keeps every checkpoint bootable and makes regressions attributable
to one boundary at a time.
