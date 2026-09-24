# gem5 / ns-3-UB physical-port boundary

## Scope

The integration boundary is the cable-facing side of each modeled UDMA host
port. Software-visible device and host-NIC behaviour remains in gem5. The
switched fabric belongs to the ns-3-UB process.

This rule is normative: a delay, queue, state machine, or fault is modeled by
exactly one side of the boundary.

## Ownership

| Behaviour | Owner |
| --- | --- |
| Official UMDK provider and kernel drivers | guest software in gem5 |
| Doorbells, SQ/RQ/CQ, WQE decoding and CQE generation | gem5 UDMA device |
| DMA, address translation, token checks and IOTLB | gem5 UDMA device |
| TP activation and TP-to-physical-port selection | gem5 UDMA device |
| Endpoint packetization/reassembly and RMA completion semantics | gem5 UDMA device |
| Host NIC egress queue, port selection and source-port serialization | gem5 UDMA device |
| Host-to-switch Adapter lookahead | co-simulation boundary |
| Switch ingress processing and VOQ admission | ns-3-UB |
| Switch ingress/egress queues, arbitration and forwarding | ns-3-UB |
| Switch egress-port serialization and switch-to-host propagation | ns-3-UB |
| Fabric routing, flow control, congestion and link faults | ns-3-UB |

The first integration phase transports the existing 40-byte modeled UDMA
transaction plus its optional payload.  That transaction is opaque to the
network except for adapter metadata needed for forwarding and accounting.
It is not an ns-3-UB transaction-layer request.

## Process topology

For two hosts the timed system has three processes:

```text
gem5 node 0 <=> adapter <=> ns-3-UB fabric <=> adapter <=> gem5 node 1
```

An N-host run uses N gem5 processes and one ns-3-UB fabric process.  Each
physical port has independent ingress and egress FIFO state.

## Adapter contract

The checked-in implementation uses adapter protocol version 4 so the current
gem5 endpoint can be tested without changing the official software path.
Each fixed-size record has a 64-byte header followed by either:

* `DATA`: the opaque UDMA transaction and optional payload;
* `SYNC`: a null-message promise for conservative synchronization; or
* `LINK_STATE`: reserved control information.

EIDs choose the destination endpoint.  Physical port identifiers choose the
source and destination port within that endpoint.  IP addresses remain part
of the userspace resource-exchange control path and never route UB data in the
fabric process.

Version 4 presents `receive_tick` at the switch ingress after gem5 has charged
the host NIC's source-port serialization and the positive Adapter lookahead.
This makes the boundary timestamp directly usable as a native `UbSwitch`
ingress event. The `ns3ub-compat` mode retains arithmetic switch/egress timing;
`ns3ub-native` replaces that arithmetic with the production `UbSwitch`, VOQ,
allocator, `UbPort`, and `UbLink` path.

The complete process and protocol implementation is reviewable in
`integrations/ns3ub/ub-gem5-adapter.cc` and
`integrations/ns3ub/ub-external-adapter-protocol.h`. The public ns-3-UB source
tree is a pinned generated dependency, not the owner of this lab-specific ABI.

## Virtual time

DATA and SYNC share the same per-port FIFO.  A SYNC record promises that the
sender will not later publish an earlier `receive_tick` on that FIFO.  The
positive host-link propagation delay is the conservative lookahead.

The ns-3-UB process may advance only to the minimum safe horizon advertised by
every physical ingress link. It must execute this synchronization in C++ and must
not return to Python for every lookahead interval.  Wall-clock scheduling is
never used as simulated latency.

Synchronization starts with the simulator and uses one absolute virtual-time
axis. SYNC terminates at the adjacent fabric adapter and carries no EID, TP,
pair, workload or ROI state. EIDs route DATA only. This lets independent flows
start at different times without creating pair-specific epochs or barriers.

## Delivery stages

1. `compatibility bridge`: consume the current version-4 ring, execute fabric
   events in an ns-3 process, and reproduce the existing switch timing test.
2. `native fabric`: translate the opaque endpoint carrier to an ns-3-UB frame
   at switch ingress and use native switch queues, routing, egress ports, and
   links. The first milestone disables flow control while validating the
   lossless base path.
3. `fabric features`: enable native flow control, congestion feedback, link
   faults, and topology-driven routing without moving transaction semantics.
4. `scale`: the adapter/switch ABI is validated for two and four endpoints and
   one and two physical ports. Larger full-system runs, MTP profiling, and
   optional MPI partitioning inside ns-3-UB remain future work.

Each stage must retain deterministic unit tests and an A/B test against the
preceding stage.  Native transport or transaction-layer ownership is outside
this plan; moving it later requires a new boundary decision.
