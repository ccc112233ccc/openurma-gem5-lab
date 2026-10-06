# CTP external-endpoint segmentation validation

## Implemented boundary

The standalone device endpoint sends one complete WQE envelope over UB-NET
wire ABI v3. Large WRITE/SEND payloads can span several IPC-ring messages, but
that fragmentation is transport-neutral. The ns-3 adapter reassembles the WQE
and submits it through a prepared native `UbJetty`; the external UDMA no
longer performs WQE-to-MTU segmentation.

Each endpoint instantiates a native `UbController`, `UbFunction`,
`UbTransaction`, `UbCtpTransportService`, and `UbCtpTransactionContext`.
Together they own 4-KiB segmentation, monotonically increasing TASSNs, the
2,048-entry sliding window, ordering, CTP Entity member-port selection, compact
CTP/UPI/EID/TA headers, and response processing. Native CBFC supplies
network-owned backpressure for simultaneous multi-segment bursts.

The adapter connects `UbTargetExecutor` to the standalone target UDMA, which
performs the actual memory read or write for each native segment. WRITE TAACK
and READ response packets carry the request TASSN and byte offset back to the
initiator. READ request carrier size (one encoded byte) is kept distinct from
its logical response length (up to 4 KiB). Duplicate TASSNs are rejected,
completed bytes are accumulated, and one WQE CQE is produced only after the
entire transaction has completed. CTP SEND remains a single packet and is
rejected above 4 KiB.

## QEMU functional regression

The dual-QEMU functional suite completed in 29.248 seconds on 2026-10-06. All
of the following cases returned zero:

- 128-byte `send_lat` and `send_bw` with SQ wrap;
- 4-KiB SEND;
- 128-byte WRITE and READ latency;
- 4-KiB WRITE and READ with 16 posted WQEs;
- 64-KiB and 1-MiB bidirectional WRITE and READ;
- bidirectional WRITE scan from 2 bytes through 1 MiB.

The run produced 2,768 and 2,936 completed SQ WQEs at the two endpoints with
zero UDMA errors. The complete per-case UART output and machine-readable
report were written to `/tmp/ubsim-native-jetty-regression-8` on the
validation host; that directory is intentionally not a repository artifact.

The fabric submitted and completed exactly 5,704 native WQEs. It observed
145,376 admitted native request segments and exactly 145,376 native
completions, with no TASSN discontinuity, trace-size mismatch, ns-3 packet
drop, or window block. Peak native outstanding depth was 256. Of the
completions, 200 were SEND target deliveries; the rest were 139,408 WRITE
TAACKs and 5,768 READ responses. This distinction is intentional: the
external SEND boundary has no target-DMA completion message, whereas WRITE
and READ retain remote-completion semantics.

## Official placement-order propagation

The official UDMA provider writes `urma_jfs_wr_t.flag.bs.place_order` into the
two-bit `place_odr` field in every hardware SQE. The standalone UDMA decodes
that field without changing the provider or kernel driver and carries its
numeric value through UB-NET wire flags. The values map directly onto native
ns-3-UB `OrderType`: NO=0, RO=1 and SO=2. The adapter validates the range,
sets the native WQE order, and preserves it on target-DMA and completion
messages.

The stock `urma_perftest` cases currently leave per-WQE `place_order` at its
zero-initialized NO value; its `--order_type` option configures Jetty transport
ordering and is not this SQE field. Coverage therefore has two complementary
layers rather than a private UMDK patch:

- `udma_model_test` feeds official-layout WRITE and READ SQEs with RO and SO
  bits and checks the emitted UDMA frames;
- `order_contract.sh` submits three complete UB-NET WQEs with NO, RO and SO to
  the independent adapter process. The native Jetty submits and completes all
  three, the target observes all three order values, and SO is not allowed to
  overtake the previously submitted RO.

The contract reports `native_order_no=1`, `native_order_relax=1`,
`native_order_strong=1`, three submitted/completed native WQEs, and zero ns-3
runtime drops. This test also caught and fixed an adapter repackaging error
that had preserved the decoded value in the wrong flag bits on target-bound
segments.

## Focused 1-MiB proof

One bidirectional `write_bw`, five iterations per endpoint at 1 MiB, produced:

```text
ctp_request_segments=2560
ctp_taacks=2560
ctp_read_responses=0
ctp_max_payload=4096
ctp_tassn_discontinuities=0
forwarded=5120
delivered=5120
```

The count is exact: `2 endpoints * 5 WQEs * (1 MiB / 4 KiB) = 2560` request
segments, followed by 2,560 TAACKs.  Each endpoint reported five submitted and
five completed SQ WQEs with zero UDMA errors.  This distinguishes protocol
segmentation from the old IPC ring fragmentation path.

The measured 189.65 MB/s is a QEMU functional-mode wall-clock result, not a
virtual-time bandwidth claim.

## Remaining work

The production path now uses the native Jetty and CTP transaction engine for
segmentation, TASSN/window state, Entity routing and response generation,
while target memory access remains in the standalone UDMA hardware model.
The full-system performance matrix exercises the current NO path and proves
lossless CBFC under the included large bidirectional bursts. Focused process
coverage now validates NO/RO/SO propagation and the RO-before-SO constraint.
Remaining protocol validation should add CNP/congestion and injected-loss/
retransmission cases; those behaviors are no longer blocked on the process
boundary, but are not claimed by this functional matrix.
