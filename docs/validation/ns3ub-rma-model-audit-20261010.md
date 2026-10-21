# ns3ub / fabric audit for small RMA and multi-Jetty fidelity

## Boundary and observed 128-B result

The standalone UDMA handles SQ, host DMA, target DMA and CQ; ns3ub receives a
complete WQE envelope, segments it into CTP packets, forwards it through one
switch, and returns the protocol completion. This ownership is specified in
`docs/ns3ub-network-boundary.md` and implemented in
`integrations/ns3ub/ub-net-adapter.cc`.

The matched one-port 128-B run in
`experiments/gem5-read-write-128-profile2-20261010/` measured 9.862 Mpps
READ and 4.853 Mpps non-inline WRITE. The fabric's final counter line has
38,229 requests, 16,384 READ responses and 21,845 WRITE ACKs (the latter
includes an unfinished exploratory inline probe), no runtime drops, no CTP
retransmissions, no congestion rate cuts, no pacing waits, no WQE failures,
and a 344-B peak fabric queue. The earlier complete 12-case run had
196,608 requests, all completed, with zero drops, retries and pacing waits;
`ctp_native_max_outstanding=192`. These observations exclude a *measured*
fabric queue, retry, or CTP pacing bottleneck in that experiment. They do not
establish that the fabric model is physically accurate.

## Protocol and timing path

* A READ request has zero wire payload and encodes the logical remote length;
  its response carries that payload. WRITE sends payload in the request and
  receives a payload-free TAACK. This is explicit in
  `sources/ns-3-ub/src/unified-bus/model/protocol/ub-ctp.cc:158-172` and
  `:2524-2579`, and in the adapter's target completion mapping at
  `integrations/ns3ub/ub-net-adapter.cc:1090-1217`. The target UDMA callback
  runs before ns3ub generates the corresponding response. Both directions
  traverse the same single switch and port; the packet sizes and where the
  DMA occurs differ.
* The modern launcher sets one 400-Gbit/s port and a 500-ns switch service
  delay per packet traversal (`scripts/run/run-dual.sh:765-799`); it passes
  those to the fabric at `:1885-1901`. The adapter applies 500 ns as the
  switch's **non-blocking ingress processing delay** before VOQ admission,
  not a global one-packet-at-a-time switch throttle
  (`integrations/ns3ub/ub-net-adapter.cc:740-749`,
  `sources/ns-3-ub/src/unified-bus/model/ub-switch.cc:450-474`). Source and
  switch `UbPort`s are independently set to 400 Gbit/s; `UbPort` serializes
  each packet's encoded bytes at that rate
  (`integrations/ns3ub/ub-net-adapter.cc:677-704,745-755`,
  `sources/ns-3-ub/src/unified-bus/model/ub-port.cc:823-840`). The default
  100-ns endpoint-to-fabric boundary delay supplies lookahead; native
  `UbLink` delay is zero to avoid double counting
  (`scripts/run/run-dual.sh:931`, `integrations/ns3ub/ub-net-adapter.cc:645-652,752-755`).
* The CTP service drains pending segments until admission, ordering, or
  pacing blocks them (`ub-ctp.cc:1503-1585`). It has a 2,048-segment
  Entity-key TA window (`ub-transaction.h:56`, `ub-datatype.h:36`,
  `ub-transaction.cc:75-96,1001-1013`) and the native Jetty separately has
  a 512-segment inflight ceiling
  (`ub-function.h:345-347,434`, `ub-function.cc:1025-1030,1117-1130`).
  Since the measured maximum was 192, neither ceiling was reached in the
  128-B scan. The `native_order_no` counter denotes per-WQE placement order,
  **not** the guest Jetty's ROI/RM mode.

Thus ns3ub has no rule requiring the source to wait for each WRITE payload
DMA before the next WQE. The observed one-Jetty WRITE ceiling coincides with
the standalone UDMA's per-Jetty `fetching` gate and ~205-ns payload DMA, as
shown in `gem5-read-write-asymmetry-20261010.md`. Reworking that gate is
reasonable, subject to preserving ordered emission and failure handling.

## Important multi-Jetty model mismatch

`SubmitNativeWqe()` maps **every guest Jetty on one endpoint to native
Jetty 1** (`integrations/ns3ub/ub-net-adapter.cc:938-952`). It keeps guest
Jetty IDs only in the WQE envelope/task metadata, and restores them when
building completion frames (`:1250-1274`). This can pass functional RMA and
CQ tests while losing native per-Jetty behavior:

1. All guest Jettys share one native WQE vector, inflight-512 ceiling,
   send/ack TA sequence and completion state. See
   `sources/ns-3-ub/src/unified-bus/model/protocol/ub-function.cc:1117-1230`
   and `ub-function.h:345-347,434`. This can cause cross-Jetty head-of-line
   effects and makes the effective transport limit independent of guest
   Jetty count once the combined outstanding segment count is high.
2. Native SO ordering bookkeeping is indexed by native Jetty number
   (`ub-transaction.cc:2141-2164,2203-2212`), so guest Jettys can acquire
   an unintended common order domain. The 128-B scan's WQEs all used
   placement NO, and its maximum 192 outstanding is below 512; these facts
   mean this alias does **not** explain the measured one-Jetty READ/WRITE
   twofold difference, and is not proven to bind the current multi-Jetty
   scan. It is a fidelity and future regression risk.
3. A direct replacement of `jetty_id=1` with each guest ID would **not** be
   a correct fix. `UbJetty::PushWqe()` allocates local TASSNs from its own
   cursor (`ub-function.cc:1402-1435`), while the CTP transaction context
   and `TryAdmit()` currently require the next global sequence for an Entity
   key (`ub-transaction.cc:75-96,1001-1013`); the second native Jetty's
   initial sequence can collide or block. Receiver duplicate handling also
   keys reliable requests by `(EntityKey, request TASSN)`
   (`ub-ctp.cc:1989-2034`). These identities must be reconciled before
   creating independent native Jettys.

The existing `integrations/ns3ub/tests/order_contract.sh` posts NO/RO/SO
WQEs through one test flow and verifies their order; it does not compare
independent guest Jettys. `tools/run_modular_rma_regression.py` supports a
Jetty count, but its pass condition does not prove separate native Jetty
state. The existing 128-B bandwidth scan is valuable performance evidence,
not an isolation contract.

### Minimal *correct* repair design

The adapter needs a stable map from `(endpoint, source EID, guest Jetty ID,
guest Jetty generation)` to a distinct native Jetty ID, plus an inverse map
used for response validation and cleanup. Allocate monotonically while IDs
are live; never reuse an ID while its WQEs, target DMA or responses remain.
Today the UB-NET lifecycle messages only fence simulations, not guest Jetty
create/destroy events, so the UDMA-to-fabric boundary must carry a queue
generation or explicit lifecycle notification to distinguish a perftest
destroy/recreate cycle. A per-run map without generation would retain stale
native ordering and sequence state when a guest ID is reused.

The native CTP implementation must then give each independent Jetty a
coherent TA identity, admission window, ACK/response lookup and duplicate
cache keyed by its native Jetty as well as the Entity path. Physical port
serialization, switch VOQ/CBFC, and optional congestion pacing remain
shared by all Jettys on one port. The `IniRcId` carried in native TA headers
(`ub-ctp.cc:235-251`) can serve as the response discriminator, provided
the target validates and echoes it; the receiver's replay cache and source
retirement path must both use it. An alternative is one global TA sequence
allocator with an explicit map to each Jetty's local ACK sequence, but
simply resetting several local cursors is invalid. Teardown should drain or
fail remaining work and remove registration/contexts after all callbacks;
it should not erase outstanding work based solely on a guest destroy event.

This is broader than the immediate source-DMA pipeline repair. Until it is
implemented, mark multi-Jetty quantitative results as using a shared native
CTP Jetty and avoid interpreting them as a hardware per-Jetty scaling law.

## Other calibration limits and acceptance tests

* `UbPort` defaults `PacketRateControl` to simulator-only `NONE` and zero
  interframe gap (`ub-port.cc:174-203`). Physical small-packet receive rate
  control/FEC overhead is therefore not derived from target hardware. This
  can make the *modeled wire* optimistic, although the current 128-B run is
  well below its wire rate. Do not tune these to compensate for the host
  payload gate without physical port counters or capability data.
* CTP retransmission is disabled by `auto` in the launcher
  (`scripts/run/run-dual.sh:935-937,1186-1190`); normal no-loss throughput
  is unaffected, but loss/tail tests require an explicit profile and should
  not be presented as validated hardware recovery behavior.
* For the immediate WRITE pipeline fix: in a standalone model test, issue
  two different source buffers through one Jetty with the first DMA delayed,
  verify more than one outstanding payload read **and** WQE order at the
  network boundary, byte integrity, bounded depth, ordered CQ retirement,
  rollback on DMA failure, and destroy while DMA is pending. Then repeat
  warmed one-port 128-B J=1 READ/WRITE and J=1/2/4 WRITE; compare `source_payload`
  concurrency, Mpps, UDMA errors, fabric drops/credit/pacing and sent order.
* For the separate native-Jetty repair: add an adapter contract with two
  guest Jettys using interleaved NO and SO WQEs, independent local TASSN
  starts, one delayed ACK, a destroyed/recreated guest Jetty ID, and a lost
  response replay. Assert no cross-Jetty ordering block, no duplicate-cache
  collision, correct destination Jetty on each completion, per-Jettty
  inflight accounting, shared single-port egress serialization and complete
  lifecycle drain. Then repeat the full-system J=1/2/4 scan.
