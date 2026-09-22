# gem5 / ns-3-UB physical-port boundary

## Scope

The integration boundary is the external side of each modeled UDMA physical
port.  Software-visible device behaviour remains in gem5.  Link and switched
fabric behaviour belongs to the ns-3-UB process.

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
| Host-port serialization and propagation | ns-3-UB |
| Switch ingress/egress queues, arbitration and forwarding | ns-3-UB |
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

The initial implementation retains adapter protocol version 3 so the current
gem5 endpoint can be tested without changing the official software path.
Each fixed-size record has a 64-byte header followed by either:

* `DATA`: the opaque UDMA transaction and optional payload;
* `SYNC`: a null-message promise for conservative synchronization; or
* `LINK_STATE`: reserved control information.

EIDs choose the destination endpoint.  Physical port identifiers choose the
source and destination port within that endpoint.  IP addresses remain part
of the userspace resource-exchange control path and never route UB data in the
fabric process.

Version 3 currently presents `receive_tick` to the fabric after gem5 has
charged host-port serialization and ingress-link propagation.  This is a
compatibility mode for the first executable bridge only.  The production
`ns3-adapter` mode must publish DATA at the NIC egress tick and move those two
physical terms into ns-3-UB.  The old `switch-adapter` mode keeps its existing
timing and remains the A/B reference.

## Virtual time

DATA and SYNC share the same per-port FIFO.  A SYNC record promises that the
sender will not later publish an earlier `receive_tick` on that FIFO.  The
positive host-link propagation delay is the conservative lookahead.

The ns-3-UB process may advance only to the minimum safe horizon advertised by
its active neighbours.  It must execute this synchronization in C++ and must
not return to Python for every lookahead interval.  Wall-clock scheduling is
never used as simulated latency.

## Delivery stages

1. `compatibility bridge`: consume the current version-3 ring, execute fabric
   events in an ns-3 process, and reproduce the existing switch timing test.
2. `physical ownership`: add the gem5 `ns3-adapter` mode and move ingress-link
   serialization/propagation out of gem5.
3. `native fabric`: inject an ns-3-UB packet representation through external
   endpoint ports and use native switch queues, routing and flow control.
4. `scale`: multiple physical ports, more than two hosts, MTP profiling and
   optional MPI partitioning inside ns-3-UB.

Each stage must retain deterministic unit tests and an A/B test against the
preceding stage.  Native transport or transaction-layer ownership is outside
this plan; moving it later requires a new boundary decision.
