# READ512: switch residence, not a stopped output

This stage diagnoses the remaining native READ burst failure after patch0014.
It does not change RTO, retries, scheduling, window size or modeled latency.
Patch0015 adds opt-in `UBSIM_CTP_RESPONSE_TRACE` events at request wire send,
target dispatch, response enqueue/receive/accept, port RX/WIRE, and ordinary
CBFC blocked-state transitions. Trace is disabled by default.

## Evidence and independent review

Evidence directory: `experiments/native-read-e2e-20261009`, especially
`n512-wirebytes`, `trace-off-n512`, and `independent-n512`.
The workload remains two peers each submitting 512 × 64 KiB READ WQEs,400G ports,
25.6 us RTO and7 retries. Each WQE is16 TAs. Correction after subsequent
source audit: the native Jetty's in-flight segment limit is512, whereas the
`(srcEntity,dstNode,dstEntity,VL)` context ACK bitmap capacity is2048. The observed
512 high-water mark was previously misidentified here as context capacity;
it is neither that capacity nor512 simultaneously active WQEs.
The previously unmaintained `ctp_native_window_blocked=0` statistic is removed;
the high-water mark now explicitly declares `per_context_ta` scope.

First failed WQE: task363/request108, source node1, target node0.

| Event | Simulated time (us) |
| --- | ---: |
| TA1712 original request leaves source |169.448640|
| Same request reaches switch ingress3 |169.449520|
| First corresponding request leaves switch egress1 |349.120560|
| Request reaches target |349.121440|
| TA1722 final retry actual send |350.435760|
| TA1712 response received and accepted at source |371.977360|
| WQE terminal failure (last send +25.6 us) |376.035760|
| TA1722 response arrives, no longer matches live WQE |376.164920|

TA1712 itself succeeds. It is the first TA of the first failed WQE, not the
first failed TA. TA1722's final-send timestamp plus RTO exactly matches the
terminal timestamp; its response arrives129.16 ns later. This is one traced
failure, not proof that every remaining failure has the same detailed cause.

Packet copies preserve UID. Independent review paired ingress and egress
events using FIFO occurrences, task, path and UID, rather than assuming a UID
uniquely identifies one retry. The association relies on this unchanged path,
same VOQ and zero runtime drops; it is not a general rerouting trace identity.

## What the switch was doing

The179.671040 us interval includes switch input processing, VOQ scheduling and
egress residence, not just one queue. Egress1 sends4153 frames in that interval:
2076 requests of44 bytes and2077 responses of4124 bytes. Total8,656,892 wire
bytes require173.137840 us at400G: approximately96.364% serialization occupancy,
estimated from frames starting within this interval. Maximum
successive send gap is87.480 ns. There is no long output-idle/credit-stuck
interval explaining this request's wait.

Independent event matching further establishes:

- All2076 requests come from ingress3 and arrived before the traced request.
- Responses come from ingress2;93 arrived earlier and1984 later.
- VOQs differ by ingress port. Packet round-robin alternates a small request
  with a large response, approximately1:1 in packets, not in bytes.
- These2077 responses are first sends on this output, not replay copies.
  The preceding request queue includes retransmissions.

Source inspection independently excludes a logical-length billing error:
READ request carrier payload is zero, its4096-byte requested length stays in
transaction metadata, and switch queues, credit cells and port serialization
all use actual packet size. Responses inherit the request priority; there is
no observed default-VL0 priority bug. CNP marks/rate cuts/wire pacing waits are
zero in this run.

Thus the traced delay is explained by existing request backlog plus packet-RR
sharing with large responses. This does not establish that the selected
scheduler, timeout or window configuration matches a particular physical UB
implementation. Do not silently replace the scheduler or enlarge RTO to label
this default configuration a successful high-concurrency test.

## Regression and wall time

Final trace build1.836 s; wire-byte traced run6.946 s (whole7.097 s).
Trace-off runs reproduce pre-instrumentation counts exactly:

| Submitted WQEs per peer | Completed/total | Failed | Actual retries | Experiment wall(s) |
| --- | --- | ---: | ---: | ---: |
|1|2/2|0|0|0.104|
|128|256/256|0|4263|0.295|
|512|968/1024|56|18216|1.051|

Parent independent N512 repeat: same968/1024 and56 failures,18216 retries;
1.199 s including cleanup, command1.23 s, exit1. Scope metadata and workload
geometry are present. All processes exit normally, but workload_success=false
and work_drained=null remain distinct from process completion.

All15 patches apply twice after restoring the pinned source into a clean
temporary tree (0.57 s); maintained restore manifest test passes (0.03 s).
Twenty harness/diagnostic unit tests pass in1.64 s. An initial invocation
omitted PYTHONPATH and failed three imports; rerun with PYTHONPATH=tools passes.
The existing ten real-port retry cases also pass with the final library (0.01 s).
No guest or gem5 cold boot was used; no new full-system acceptance is claimed.

## Next bounded work

Preserve the failing default case. Establish protocol/configuration grounds
for the TA window, retry timing and switch scheduling before choosing a repair
or an explicitly labelled parameter-sensitivity experiment. In particular,
native adapter retry settings are not automatically evidence that official
perftest user-TP retry/ack-timeout arguments control CTP. A changed-parameter
pass must not overwrite this failure or count as fixing this configuration.
Only then rerun the previously failing synchronized official-driver READ trial.
