# CTP external-endpoint segmentation validation

## Implemented boundary

The standalone device endpoint splits WRITE and READ transactions at the
4-KiB UB transport MTU. The ns-3 adapter now owns the endpoint transaction
state: each endpoint instantiates the native `UbController`,
`UbTransaction`, `UbCtpTransportService`, and one
`UbCtpTransactionContext` per Entity path. The native context admits each
segment, assigns its monotonically increasing TASSN, applies the 2,048-entry
sliding window, and retires it when WRITE TAACK or READ response arrives. The
adapter then encodes compact CTP, UPI, EID and TA headers; the switch resolves
the destination EID through a two-member CTP Entity.

The target UDMA performs DMA per segment.  WRITE TAACK and READ response packets
carry the request TASSN and byte offset back to the initiator.  Duplicate TASSNs
are rejected, completed bytes are accumulated, and one WQE CQE is produced only
after the entire transaction has completed.  CTP SEND remains a single packet
and is rejected above 4 KiB.

## QEMU functional regression

The dual-QEMU functional suite completed in 28.496 seconds on 2026-10-06. All
of the following cases returned zero:

- 128-byte `send_lat` and `send_bw` with SQ wrap;
- 4-KiB SEND;
- 128-byte WRITE and READ latency;
- 4-KiB WRITE and READ with 16 posted WQEs;
- 64-KiB and 1-MiB bidirectional WRITE and READ;
- bidirectional WRITE scan from 2 bytes through 1 MiB.

The run produced 1,478 and 1,646 completed SQ WQEs at the two endpoints with
zero UDMA errors. The complete per-case UART output and machine-readable
report were written to `/tmp/ubsim-native-ctp-context-regression-2` on the
validation host; that directory is intentionally not a repository artifact.

The fabric observed 73,440 admitted request segments and exactly 73,440
native completions, with no TASSN discontinuity and no window block. Peak
native outstanding depth was 256. Of the completions, 200 were SEND target
deliveries; the rest were 70,032 WRITE TAACKs and 3,208 READ responses. This
distinction is intentional: the existing external SEND ABI has no TAACK
message, whereas WRITE and READ retain remote-completion semantics.

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

This stage uses native ns-3-UB wire headers, Entity routing, transaction
contexts, admission and TAACK windows. The external UDMA process still owns
WQE-to-4-KiB segmentation, so Jetty ordering and CNP behavior are not yet on
the production path. The next stage is to carry one WQE descriptor across the
device/adapter boundary, submit it through a prepared native Jetty, and attach
the existing external DMA engine through `UbTargetExecutor`. That will let the
native service own segmentation, NO/RO/SO ordering and response generation
without moving actual host-memory access into the network model.
