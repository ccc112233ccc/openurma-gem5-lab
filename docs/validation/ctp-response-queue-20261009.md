# READ response queue coalescing: accepted partial repair

## Reproduced cause, unchanged timing configuration

This continues the strict native counterexample in
[READ/SEND timing](read-send-timing-20261009.md), rather than repeating a guest
boot to rediscover the failure.400G port rate,25.6 us RTO and7 retries are
unchanged. No official driver/provider or perftest formula is modified.

Observation-only evidence: `experiments/native-read-response-trace-20261009`.
N128 still produced18 failed WQEs and8026 retransmissions, matching the baseline.
5376 duplicates arrived while a response for that transaction was already
queued, reaching7 queued copies. Maximum enqueue-to-wire residence was
319.400640 us; the old response-cache TTL was204.8 us.220 cache entries expired
while their responses were still queued. This is concrete queue/lifetime
evidence, not a hypothesis based solely on retry counts.

## Implementation and safety boundary

Maintained patch: `integrations/ns3ub/patches/0014-ctp-response-queue-coalescing.patch`,
applied after0013 by the normal adapter builder. Generated source matches the
patch. The adapter exposes `ctp_response_replays_coalesced` in final stats.

Parent review found that the builder's restore list initially omitted the two
newly patched files `ub-port.h` and `ub-fault.cc`, which would break a repeated
build. Both are now included. A manifest regression checks every patched file
is restored and every patch is wired (0.003 s); all14 patches applied cleanly
twice to an isolated tree from the pinned revision (0.64 s), without resetting
the active source tree.

- Track the actual response packet until the port starts its transmission.
- Coalesce duplicate requests only while a matching response remains queued:
  no extra packet, buffer reservation or flow-control credit is allocated.
- After transmission, a new duplicate can still replay the cached response,
  preserving recovery after real response loss. In-flight packets are not
  cancelled. Queued records do not expire; actual send and duplicate receipt
  refresh the existing cache retention interval.
- On ordinary channel transmit rejection or explicit fault drop, release the
  queued-response ownership without fabricating a successful wire event. The
  pre-existing exact flow-control cancellation/refund path remains in place.
  Duplicate PacketUID insertion is asserted before incrementing ownership.

This is **not** a complete exactly-once or fault-lifecycle solution. With no
queued response, an old request delayed beyond TTL can still execute again.
16-bit TASSN generation/tombstone retirement needs a protocol-grounded design.
Independent review also identified `TransmitPacketDetached`'s failed-transmit
branch, which does not yet notify discard; delayed/rerouted fault paths can
retain ownership. Do not claim general fault/reconnect correctness from the
ordinary-path rejection test. These limits remain explicit follow-up work.

## Accepted tests and retained failure

Final native evidence: `experiments/native-read-coalesced-final-20261009`.
Each endpoint submits N64 KiB READs. Strict peer checks identities, offsets and
data; measured target reply dwell/backpressure are0. No loss is injected in
these burst runs. `work_drained=null` remains unknown, not a global checkpoint.

Here N counts submitted WQEs per endpoint, not simultaneously admitted TAs.
Each64 KiB WQE uses16 TAs; the native512-entry window belongs to a
`(srcEntity,dstNode,dstEntity,VL)` context and counts TAs. The reported
`ctp_native_max_outstanding` is the largest observed count of one context, not
an aggregate fabric count or proof of512 active WQEs. The former
`ctp_native_window_blocked=0` field was never incremented and is not evidence
that admission never blocked; it has been removed from subsequent output.

| N per endpoint | Completed / submitted | Failed | Actual retries | Response replays | Coalesced | Experiment wall(s) |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 2/2 | 0 | 0 | 0 | 0 | 0.490 |
| 16 | 32/32 | 0 | 0 | 0 | 0 | 0.110 |
| 64 | 128/128 | 0 | 1159 | 570 | 589 | 0.150 |
| 128 | 256/256 | 0 | 4263 | 1281 | 2982 | 0.305 |
| 512 | 968/1024 | 56 | 18216 | 6450 | 11766 | 1.042 |

For these immediate-target workloads, duplicate requests equal actual response
replays plus coalesced requests. N128's18 failures are eliminated. N512 **still
fails**, returns nonzero and is not accepted512-outstanding READ support. Its
observed maximum response residence falls to79.97 us, queued copies to1 and
queued cache expirations to0, but end-to-end request/response delay still needs
tracing; do not silence remaining in-flight replay or increase RTO to force a
pass. The previous duplicate target execution was not reproduced in this N512
run; that does not prove the broader old-request TTL problem is closed.

Ten real-port tests pass, independently rerun in0.01 s. New cases hold the
original READ response in a real output queue through two retries, then release
it; and reject the first real `UbLink::TransmitStart`, then require the next
retry to recover with one target execution. Existing READ-response loss and
TAACK-loss contracts also pass. Final native rebuild14.678 s; initial compile
attempts found a private-method declaration and were corrected before tests.
The parent additionally reran current recovery/read-length/feedback contracts
successfully (0.00/0.01/0.02 s); feedback default mode is not a new all-mode
congestion acceptance claim.

## Full-system functional regression

`experiments/qemu-response-coalescing-functional-20261009`: official perftest
12/12 passed in29.620 s, whole launch/test/stop wrapper35.50 s. This uses the
normal image and **no-sync**, so it is functional regression only, not timing
or evidence that the native512 failure disappeared. SEND/READ/WRITE, ring wrap,
fragmentation and repeated resource reuse are exercised.1 MiB cases retain
their earlier classification: actual transfers beyond the current driver's
advertised64 KiB maximum, not declared official capability.

A first standalone launch command exited before the next tool invocation and
the regression rejected a non-running QEMU process (launch0.69 s, check0.03 s).
The successful attempt kept startup, regression and teardown in one supervised
command. No gem5 cold boot was used. All five simulation processes were stopped
after evidence collection; native model logs are saved with the experiment.

## Next bounded step

Keep the accepted partial repair. Use N512 to correlate source request actual
send, switch residence, response enqueue/actual send and source completion per
TA before changing another policy. Only after native high-concurrency success
should the previously failed synchronized full-system READ512 trial be rerun.
The separately diagnosed SEND peak/average issue is an audit-semantics fix,
not another packet-timing defect; see the runtime report record.
