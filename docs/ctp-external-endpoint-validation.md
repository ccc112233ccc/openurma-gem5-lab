# CTP external-endpoint segmentation validation

## Implemented boundary

The standalone device endpoint now splits WRITE and READ transactions at the
4-KiB UB transport MTU and assigns one monotonically increasing TASSN to each
request segment.  The ns-3 adapter encodes compact CTP, UPI, EID and TA headers;
the switch resolves the destination EID through a two-member CTP Entity.

The target UDMA performs DMA per segment.  WRITE TAACK and READ response packets
carry the request TASSN and byte offset back to the initiator.  Duplicate TASSNs
are rejected, completed bytes are accumulated, and one WQE CQE is produced only
after the entire transaction has completed.  CTP SEND remains a single packet
and is rejected above 4 KiB.

## QEMU functional regression

The dual-QEMU functional suite completed in 28.712 seconds on 2026-10-06.  All
of the following cases returned zero:

- 128-byte `send_lat` and `send_bw` with SQ wrap;
- 4-KiB SEND;
- 128-byte WRITE and READ latency;
- 4-KiB WRITE and READ with 16 posted WQEs;
- 64-KiB and 1-MiB bidirectional WRITE and READ;
- bidirectional WRITE scan from 2 bytes through 1 MiB.

The run produced 1,478 and 1,646 completed SQ WQEs at the two endpoints with
zero UDMA errors.  The complete per-case UART output and machine-readable
report were written to `/tmp/ubsim-ctp-segment-regression-2` on the validation
host; that directory is intentionally not a repository artifact.

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

This stage uses native ns-3-UB wire headers, Entity routing and switch/fabric
behavior, but the external endpoint still owns admission and TASSN allocation.
It does not yet instantiate ns-3-UB `UbCtpTransportService` for the external
Jetty.  The next stage is to move the same endpoint state behind that service,
including its Jetty inflight window, ordering rules, CNP handling and TAACK
window, without changing the UDMA/fabric process boundary.
