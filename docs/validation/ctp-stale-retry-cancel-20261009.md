# Retire stale queued CTP attempts before physical transmission

## Reproduced problem and scope

The preceding actual-send timer repair passed synchronized1 MiB WRITE at400G
and200G. At200G the latter still sent11251 queued retry copies after their
original ACK had retired the request. Target duplicate suppression prevented
duplicate DMA, but their wire time and replay ACKs affected performance.
The active retry count alone did not expose this traffic. Baseline average:
36611.45 MiB/s (38.3898877952 GB/s); full wall time134.261 s.

This stage targets only local, not-yet-transmitted stale attempts. Already
transmitted packets must remain in flight and be handled by normal duplicate
suppression. RTO, physical delays and target counters must not be tuned or
silenced to make the result look better.

## Implementation and acceptance plan

The cancellation boundary is source egress dequeue before `TransmitStart`.
Normal dequeue updates buffer accounting. A cancelled packet must return its
actual locally reserved flow-control credit and preserve pending piggyback
credit returns; it must not return upstream credit a second time. The port
must resume READY state and scheduling so subsequent valid WQEs make progress.
Dedicated and shared credit pools require separate exact refund accounting.

Independent tests strengthen the earlier queued-ACK/error examples: blocking
the source immediately after the original transmit can leave a retry upstream
of egress. The new cases block only after the retry's real egress enqueue and
verify nonzero egress occupancy before retirement. They must also check
physical transmission counts, later valid WQEs, and an already-in-flight retry.
Native tests are not official guest-driver evidence.

The timing runner now accepts `--write-iterations`, `--write-depth`,
`--write-post-list` and `--write-cq-mod` for later longer/512-depth experiments.
Defaults preserve the prior command exactly. Invalid batch/depth combinations
fail before launching any process. Three command-generation tests pass in0.03 s;
four live-observer parsing tests pass in1.44 s. This only validates the harness,
not actual512-outstanding official-driver performance.

## Native and official-driver acceptance

Patch0013 adds general flow-control egress cancellation/transmission hooks.
CBFC remembers each packet's actual dedicated/shared debit and newly attached
piggyback return. Cancellation erases that reservation first, refunds it once,
and schedules return control traffic when required. Successful transmission
retires the reservation without refund. CTP validity is checked only at the
source boundary, never at a downstream switch after the packet is in flight.

The full restore/reapply build and native contracts passed in18.11 s (user
89.15 s, system6.75 s). Eight actual-port scenarios pass: previous loss/timeout
and initial-wait cases, queued ACK/Abort with dedicated and shared credit,
and an already-in-flight retry. The queued cases use real reverse READ traffic
to put a CCI on the cancelled packet. Before/after balances prove exact debit
and pending-return refunds, including no second refund on another wakeup;
32 subsequent valid WQEs execute/complete/transmit once and egress drains.
The in-flight case still physically transmits twice and executes once.

Independent review found no blocker in this ordinary fabric path. Additional
precise queued/cancelled/stale-sent counter assertions were rebuilt in2.06 s;
all eight scenarios passed again in0.41 s. Fault park/reroute reservation
lifetime is not covered: do not infer arbitrary link-failure recovery.

Official-driver no-sync QEMU regression passes12/12, including1 MiB READ/WRITE
and the2 B–1 MiB WRITE sweep. Suite29.605 s, launch/suite/stop35.63 s.
Evidence: `experiments/qemu-stale-cancel-functional-20261009`.

New fabric counters distinguish cumulative queued attempts, active sent retries,
stale sent retries and cancelled retries. A queued attempt is not a physical
transmission; after full drain the categories can be checked separately rather
than treating zero active retransmissions as zero duplicate wire traffic.

Independent late-DMA/error replay contracts pass10/10 without assertion
changes (0.979 s script wall time, excluding interpreter startup and cleanup).
Nine well-formed cases preserve one target execution, one source failure,
no success and no pending DMA. The malformed-length case remains explicit
fail-stop rejection rather than a drain claim. Evidence:
`experiments/ctp-stale-cancel-native-20261009`.

## Synchronized200G before/after result

The exact same manifest and official1 MiB bidirectional command as the previous
200G control pass. Average45420.94 MiB/s =47.62730758144 decimal GB/s, versus
36611.45 MiB/s =38.3898877952 GB/s before cancellation (+24.06%). Peak45429.84
MiB/s. These are the tool's already-combined `-B` outputs, not the sum of its
identical endpoint reports. RTO remains25.6 us and no physical timing constant
was changed. Evidence: `experiments/qemu-write-1m-stale-cancel-200g-20261009`.

The queue statistics reconcile exactly:11254 retries queued =11104 cancelled
+150 actively transmitted +0 stale transmitted. The150 real retries remain in
flight and produce150 target duplicate suppressions/ACK replays;143 responses
arrive after WQE termination. Do not claim zero physical retransmissions.
Duplicate requests fall from11251 to150 (98.67% fewer), without removing
target-side accounting or cancelling packets already on the wire.

128/128 WQEs and32768 original TA requests complete; zero failed WQEs,
retry exhaustion, target DMA errors, trace mismatch, runtime drop or pending
target DMA. Each UDMA retires64/64 SQ entries, reports no host DMA/remote
service/receive/completion work, and saves device state. Two fresh live work
snapshots also show drain. This is not coordinated full-system checkpoint
acceptance or proof that native maintenance event queues are empty.

Wall time: shell46.475 s, command84.812 s, drain0.261 s, total133.930 s.
This closes the reproduced stale-source-queue traffic issue. The result is
still a64-iteration-per-direction short test with large-message/CQ-moderation
warnings, not proof of steady-state saturation or all-operation acceptance.
Next performance work is longer counter-backed runs and actual512-depth
concurrency/parameter sensitivity, retaining earlier functional depth512
evidence rather than repeating it from zero.

Independent timing review confirmed byte-identical manifests and identical
command arrays against the earlier200G control, conversions, retry-counter
conservation and drain evidence; its `acceptance.txt` is in the experiment
directory. Native admission peak512 counts TA/segments, not512 outstanding
official WQEs, and must not be used to claim the latter.
