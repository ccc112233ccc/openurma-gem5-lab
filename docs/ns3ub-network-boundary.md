# gem5 / ns-3-UB physical-port boundary

## Scope

The integration boundary is the cable-facing side of each standalone UDMA
device process. The host simulator provides CPU, memory and interrupts through
UB-HOST; device behavior belongs to `udma-device-sim`; the switched fabric
belongs to the ns-3-UB process.

This rule is normative: a delay, queue, state machine, or fault is modeled by
exactly one side of the boundary.

## Ownership

| Behaviour | Owner |
| --- | --- |
| Official UMDK provider and kernel drivers | guest software in gem5 |
| Doorbells, SQ/RQ/CQ, WQE decoding and CQE generation | standalone UDMA device |
| DMA, address translation, token checks and IOTLB | standalone UDMA device |
| TP activation and TP-to-physical-port selection | standalone UDMA device |
| Endpoint packetization/reassembly and RMA completion semantics | standalone UDMA device |
| Host NIC egress queue and port selection | standalone UDMA device |
| Endpoint-to-fabric propagation and lookahead | UB-NET boundary configured by fabric adapter |
| Switch ingress processing and VOQ admission | ns-3-UB |
| Switch ingress/egress queues, arbitration and forwarding | ns-3-UB |
| Switch egress-port serialization | ns-3-UB |
| Fabric routing, flow control, congestion and link faults | ns-3-UB |

UB-NET transports one wire-visible transaction segment plus forwarding
metadata. WQEs, host DMA requests and WQE completion state never cross this
boundary.  The adapter converts the simulator-neutral UDMA segment shim into
the real ns-3-UB CTP/UPI/EID/TA header stack before the packet enters the
fabric, and reverses that encoding at the destination endpoint.

## CTP transaction segmentation

The endpoint side of the standalone device owns CTP segmentation.  SEND is a
single-packet operation and is rejected above the 4-KiB transport MTU.  WRITE
and READ are split into at most 4-KiB transaction segments.  Every request
segment receives a monotonically increasing TASSN; WRITE TAACK and READ
response packets retain the request TASSN and transaction offset.

The remote UDMA performs address translation and DMA independently for every
segment.  The initiating UDMA de-duplicates returned TASSNs, accumulates the
completed byte count, and emits the WQE CQE only after the complete byte range
has finished.  IPC ring fragmentation is therefore no longer confused with a
CTP transaction segment.

Compact EIDs are registered as native ns-3-UB CTP Entities.  Both physical
ports are Entity members, and the switch resolves a wildcard destination CNA
through the Entity registry and load-balance field.  The switch does not
segment WQEs, allocate TASSNs, access guest memory, or decide WQE completion.

This is the first protocol-correct external-endpoint stage.  It uses native
ns-3-UB wire headers and Entity routing, while admission state remains in the
external endpoint.  Moving that admission state behind
`UbCtpTransportService` is a later internal refactor, not a change to the
UDMA/fabric process boundary.

## Process topology

For two hosts the target timed system has five processes:

```text
gem5/QEMU 0 <=> UDMA 0 <=> ns-3-UB fabric <=> UDMA 1 <=> gem5/QEMU 1
```

An N-host run uses N host-simulator processes, N UDMA device processes and one
ns-3-UB fabric process. Each physical port has independent link and queue state.

## Adapter contract

The target implementation uses simulator-neutral UB-NET v1 over the pinned
SimBricks shared-memory transport. Each fixed-size record has a 64-byte header
followed by either:

* `FRAME`: an opaque wire-visible UB frame and its route metadata;
* SimBricks `SYNC`: a null-message promise for conservative synchronization; or
* `LINK_STATE`: reserved control information.

EIDs choose the destination endpoint.  Physical port identifiers choose the
source and destination port within that endpoint.  IP addresses remain part
of the userspace resource-exchange control path and never route UB data in the
fabric process.

The positive UB-NET boundary latency represents endpoint-to-fabric propagation
and provides conservative lookahead. ns-3 owns port serialization, switch VOQ
admission, arbitration and queueing. Internal `UbLink` propagation is zero in
the adapter so the boundary delay is not charged twice.

The complete process and protocol implementation is reviewable in
`integrations/ns3ub/ub-net-adapter.cc` and `protocol/ub_net/`. The public
ns-3-UB source tree is a pinned generated dependency, not the owner of this
lab-specific ABI.

## Virtual time

FRAME and SYNC share the same per-port FIFO. A SYNC record promises that the
sender will not later publish an earlier `receive_tick` on that FIFO.  The
positive host-link propagation delay is the conservative lookahead.

The ns-3-UB process may advance only to the minimum safe horizon advertised by
every physical ingress link. It must execute this synchronization in C++ and must
not return to Python for every lookahead interval.  Wall-clock scheduling is
never used as simulated latency.

Synchronization starts with the simulator and uses one absolute virtual-time
axis. SYNC terminates at the adjacent fabric adapter and carries no EID, TP,
pair, workload or ROI state. EIDs route FRAME only. This lets independent flows
start at different times without creating pair-specific epochs or barriers.

## Current and future coverage

The native fabric and full-system launcher cutover are complete. Contract
tests cover synchronized and asynchronous execution, while full-system runs
cover multiple endpoints and physical ports. Native flow control, congestion
feedback, link faults, larger-scale MTP profiling and optional MPI
partitioning inside ns-3-UB remain future work. Transaction-layer ownership
stays in the UDMA device model.
