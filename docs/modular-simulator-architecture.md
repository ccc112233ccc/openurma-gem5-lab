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

`components/ub-switch-sim/` is the simulator-neutral reference fabric. It is
a third process, connects any number of UB-NET endpoints, publishes physical
port link state, routes frames by destination EID, and retains queued frames
under output backpressure. Its contract test uses two independent listening
endpoint processes and proves request/reply traffic crosses the switch in both
directions:

```bash
./lab --runtime native build ub-switch
```

The ns-3-UB backend must implement this same UB-NET boundary. The old mmap
ring-v4 adapter remains a compatibility path only and is not the target API.

When SimBricks synchronization is negotiated, both the device process and the
reference switch advance to the minimum of the next input timestamp and the
next required outbound SYNC timestamp. They do not increment virtual time from
host loop iterations. The switch test suite exercises this with a second
three-process `sync=required` contract.

The device core receives time explicitly through `AdvanceTime(picoseconds)`.
JFC completion-period moderation therefore uses the official encoded
0/4/16/64/256/1024/4096/16384 microsecond periods in virtual time, independent
of host scheduling speed. The official MODIFY_JFC context/mask operation can
change count and period without recreating the queue.

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

## Migration status

The standalone production-mode device now owns the first official-driver
bootstrap slice:

- UBIOS root, UBC, and UMMU firmware discovery tables, with addresses derived
  from the MMIO base supplied by the host handshake;
- the complete 16 MiB official device aperture advertised to the host;
- architecture-generic UMMU capability registers, CR0/ACK, GBPA update, and
  MCMDQ producer/consumer handshakes;
- retained UBIOS and UBASE management queue registers;
- asynchronous UBIOS SQ payload DMA, root/endpoint enumeration, configuration
  and token responses, RQ response DMA, CQE DMA, and index advancement.
- asynchronous UBASE CSQ consumption over I/O-virtual DMA, multi-descriptor
  response writes, firmware/resource/port capability queries, and hardware
  head/tail publication. The UB-HOST DMA message explicitly distinguishes
  guest-physical and I/O-virtual addresses.
- asynchronous UBASE mailbox DMA now captures the official driver's AEQ, CEQ,
  JFC, JFR, JFS, and Jetty contexts, handles context query/destruction, and
  publishes mailbox completion through the architected AEQ before vector 1.
- UE2UE CtrlQ requests are assembled from the official multi-descriptor CSQ
  format; QoS, SEID and TP lifecycle responses are published through CRQ.
  Each TP is assigned to a physical port at allocation and retains that route.
- the first official data-plane slice consumes direct SQE or ring doorbells,
  decodes the stock SEND/SEND_IMM WQE, performs payload DMA, emits a typed UDMA
  wire envelope through UB-NET, and produces the stock CQE plus CEQ interrupt.
- the receive slice resolves the destination Jetty/JFR, consumes the official
  RQ producer and index rings, scatters payload over posted SGEs, advances the
  hardware consumer, and creates a receive CQE with peer identity metadata.
- WRITE completion is held until a remote-DMA ACK returns; READ retains its
  local SGE and token until the matching response is DMA-written. RMA packets
  carry transaction IDs, remote segment tokens, addresses, and lengths in the
  simulator-neutral UDMA wire header.
- queue and payload token DMA now resolves the official TECT/TCT entry and
  walks the current ARM64 guest page tables inside the device process. Invalid
  or absent tokens fail before host memory is touched; MAPT access permissions
  remain a separate migration item.
- once the official UBUS code programs its Type-1 MSI tuple through UBIOS,
  interrupt delivery translates the MSI IOVA through UMMU and issues the
  programmed data as a host-bus write. Logical pin pulses are bootstrap-only.
- UB-NET link-state messages update per-port hardware state. A TP whose port
  goes down is rebound to an available modeled port; WQEs cannot transmit on a
  route whose port is down.

The temporary descriptor used by the original process contract is now behind
the explicit `--test-abi` switch. A normal `udma-device-sim` process starts in
official-aperture mode; test-only registers no longer overlap production MMIO.
The feature-by-feature extraction status is tracked in
[`device-model-migration.md`](device-model-migration.md); “migrated” there
means the behavior is executed by the standalone model through UB-HOST, not by
the legacy in-gem5 implementation.
