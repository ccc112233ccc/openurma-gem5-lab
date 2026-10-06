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
| SQE decoding and complete-WQE UB-NET envelopes | standalone UDMA device |
| CTP segmentation, TASSN/window, WQE ordering and request retransmission | native ns-3-UB endpoint/Jetty |
| Target DMA and initiator CQE generation | standalone UDMA device |
| Host NIC egress queue and port selection | standalone UDMA device |
| Endpoint-to-fabric propagation and lookahead | UB-NET boundary configured by fabric adapter |
| Switch ingress processing and VOQ admission | ns-3-UB |
| Switch ingress/egress queues, arbitration and forwarding | ns-3-UB |
| Switch egress-port serialization | ns-3-UB |
| Fabric routing, flow control, congestion and link faults | ns-3-UB |

UB-NET transports one complete WQE envelope from UDMA to the native ns-3-UB
endpoint. Large envelopes may be split into IPC-ring chunks, but those chunks
are not transport segments. Host DMA requests and WQE completion state never
cross this boundary. The native Jetty creates the real CTP/UPI/EID/TA packet
stream; the adapter invokes the target UDMA for each resulting DMA operation.

## CTP transaction segmentation

The native ns-3-UB endpoint owns CTP segmentation. SEND is a single-packet
operation and is rejected above the 4-KiB transport MTU. WRITE and READ are
split into at most 4-KiB transaction segments. Every request segment receives
a monotonically increasing TASSN; WRITE TAACK and READ response packets retain
the request TASSN and transaction offset.

The remote UDMA performs address translation and DMA independently for every
segment. The native CTP transaction context retires TASSNs and completes the
WQE only after the complete byte range has finished; the initiating UDMA then
emits the CQE. IPC ring fragmentation is therefore no longer confused with a
CTP transaction segment.

Compact EIDs are registered as native ns-3-UB CTP Entities.  Both physical
ports are Entity members, and the switch resolves a wildcard destination CNA
through the Entity registry and load-balance field.  The switch does not
segment WQEs, allocate TASSNs, access guest memory, or decide WQE completion.

Official UDMA SQE `place_odr` values propagate unchanged as NO, RO or SO into
the native WQE. Native `UbCtpTransportService` owns their admission and
ordering state. A focused process contract checks all three values and ensures
an SO WQE cannot overtake a previously submitted RO WQE.

For WRITE and READ, the same native service retains the encoded request
segment until its TAACK or READ response arrives. A virtual-time RTO requeues
that exact packet through the original native queue and physical port;
completion removes the retained segment. The fabric therefore owns timeout
and retransmission, while the target UDMA still owns the physical memory
action. Focused contracts cover both loss of one request segment and loss of
its TAACK. After a lost TAACK, the receiver suppresses the duplicate WRITE and
replays the cached TAACK without repeating the target DMA. An
exhausted retry budget terminates the native WQE and is translated by UDMA to
an official CQE with ACK-timeout status. A lost READ response follows the same
request retry path; the target caches the completed response, suppresses the
duplicate READ DMA, and rebuilds the response packet without executing memory
access again.

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
tests cover synchronized and asynchronous execution, NO/RO/SO WQE order
propagation, and one injected WRITE-request loss followed by virtual-time RTO
retransmission. Full-system runs cover multiple endpoints and physical ports.
Native flow control is active for the validated lossless runs. Request loss
and TAACK loss plus retry exhaustion now have focused recovery contracts.
READ-response loss now also has a focused replay contract. Congestion feedback,
larger-scale MTP profiling and optional MPI partitioning inside ns-3-UB remain
future work.
