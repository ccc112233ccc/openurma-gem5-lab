# Target RMA errors are transaction failures, not successful ACKs

## Protocol basis and limited scope

Read-only inspection of the UnifiedBus Base Specification2.0 Chinese PDF
(`/tmp/ubsim-base-spec-2.0-zh.pdf`, SHA256 recorded in the full-TA validation)
covered complete pages203–206,214,229–230; page205 was also rendered and
visually verified. The document is published via
[UnifiedBus](https://www.unifiedbus.com/zh), with the inspected
[PDF mirror](https://github.com/codehubcloud/UnifiedBus/blob/master/UB-Base-Specification-2.0-zh.pdf).

Full ATAH uses RSPST3 for Completer Error and RSPINFO2 for Remote Abort.
Page214 distinguishes terminal errors from retryable RNR/Page Fault. READ
errors remain READ_RESPONSE (pages229–230), carrying status rather than being
converted into successful TAACK. Current UB-NET `RmaError` has no syndrome
detail, so this model maps generic execution failure to Remote Abort; it does
not invent a Page Fault, RNR timer, permission subtype or recovery policy.

## Implementation

The adapter validates the pending request identity/range and zero error payload,
then reports FAILED/REMOTE_ABORT through the real target completion callback.
Patch0011 carries response status/information on the native segment and full
ATAH. WRITE sends error TAACK; READ sends error READ_RESPONSE with no data.
The target caches the completed error response and can replay it without
executing DMA again. Null responses are not used to represent this error.

An active initiator verifies operation, requester context, task association
and response format before terminating the WQE once. Its retry records are
cleared; the error does not pass through successful segment completion/byte
accounting. Already-terminal initiators ignore late error responses rather than
recreating state. The existing adapter→UDMA RmaError→error-CQE path is reused.
Other nonzero statuses remain unsupported, and this implementation is gated to
CTP; the unimplemented RTP status encoder must not accidentally send success.

## Acceptance

Before implementation, four native process counterexamples (READ/WRITE while
source active or after source terminal) all failed specifically with
`invalid external target DMA completion`. Wall times0.610/0.077/0.079/0.086 s.
The active case failed on target reply without a valid source result; late
cases had already exhausted retries before the target replied.

Ten native process profiles pass (four existing plus six error profiles),
archived in `experiments/ctp-remote-abort-native-20261009`:

| Error profile | Wall seconds | Required result |
| --- | ---: | --- |
| READ active |0.077| One failure at0.7 us, zero retries |
| WRITE active |0.067| One failure at0.7 us, zero retries |
| READ after source terminal |0.065| Late error drains, no duplicate source result |
| WRITE after source terminal |0.079| Late error drains, no duplicate source result |
| Lost READ error response |0.070| Target executed once, one retry and one error replay |
| Lost WRITE error response |0.070| Target executed once, one retry and one error replay |

Lost-response cases deliver one source failure at26 us, not a timeout after
seven retries; exhausted count stays zero. Every error profile requires one
target error, no successful WQE completion and no pending target DMA. Existing
late WRITE/READ/first-arrival/malformed-length cases remain passing.

Independent review found an additional wrong-peer/source-group association
gap. The final patch verifies reverse node and Entity identity using the
prepared request route, without binding replies to a physical port. Wrong-node
and wrong-EID RemoteAbort packets must not fail an unrelated WQE. Three native
READ runs (context, no context, true unbound source-group) total51 cases pass
in0.47 s, retaining wire and packet-context checks. Incremental builds2.61 and
1.34 s. Initial complete build/contracts17.45 s.

These native contracts do not run the official guest driver, and an
intentionally failed WQE is not a successful bandwidth experiment. Full-system
results follow after the bounded repeat.

Final clean restore/reapply build and native contracts passed in16.89 s.
An independent final-binary rerun accepted10/10 profiles in0.724 s of script
measurement (excluding interpreter startup/cleanup), preserved under
`experiments/ctp-remote-abort-final-20261009`. Nine well-formed cases drain
pending target DMA; the malformed-length fail-stop case is deliberately not
claimed to drain. Multi-fragment partial-success/error interleaving remains
an additional counterexample to cover.

## Official-driver full-system repeat

The no-sync QEMU suite passed12/12 cases, including SEND latency/bandwidth,
READ/WRITE latency,4 KiB/64 KiB/1 MiB bandwidth and the2 B–1 MiB WRITE sweep.
Suite wall time29.592 s; launch, suite and stop35.61 s. Evidence:
`experiments/qemu-remote-abort-functional-20261009`. These are functional
acceptance results, not synchronized bandwidth measurements.

The first timing launch was rejected while a prior UDMA process was exiting
(0.057 s). A subsequent sandboxed attempt failed to bind its Unix socket
(12.716 s). Neither reached a guest workload; neither is counted as a model
test failure or performance result. The permitted socket-enabled repeat is
recorded separately below.

The socket-enabled synchronized1 MiB WRITE repeat reached both guest shells
in45.645 s. Both official perftest processes returned status255 with
`Failed CR status 9`, instead of the old adapter completion exception. Command
wall time85.937 s; whole launch/workload/stop133.978 s. No bandwidth row was
produced. Evidence: `experiments/qemu-write-1m-remote-abort-socket-20261009`.

At shutdown the fabric reported128 WQEs:12 successful,116 failed;212659
retransmissions and116 retry exhaustions. Target errors40867 were processed
without the previous crash. Both SQs retired64/64, but fabric pending DMA25
and node1 remote services11 remained at the runner's stop snapshot; node1
correctly refused a checkpoint. This snapshot is not proof of a permanent
drain failure, nor proof of complete drain. Large-message synchronized
bandwidth and error teardown therefore remain unaccepted.

## Next bounded repair

Independent source review confirms `ArmRetransmission` runs at `EnqueueData`,
before source-port serialization, credit and queue waiting. `UbCtpTxQueue`
dequeue is also too early. Existing `UbPort::TraComEventNotify` occurs after
successful `TransmitStart`; its current virtual time is a usable actual-send
boundary (the completion timestamp includes extra link timing).

Use explicit queued/sent attempts with unique generation and attempt tags:
only a matching actual-send callback arms a timer; a timeout may enqueue one
retry but must not arm again until it actually sends. ACK/error/terminal state
invalidates queued callbacks and old timers. Move request-loss injection to
the send boundary so dropped-on-wire tests still start their timeout. Required
counterexamples include long local queue/credit delay, delayed retry departure,
ACK while retry queued, terminal cleanup, actual wire loss and stale callback.
Do not simply enlarge RTO. This design is not implemented by patch0011.
