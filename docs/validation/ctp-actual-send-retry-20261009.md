# CTP retry timers start at actual port transmission

## Scope and mechanism

Patch0012 removes RTO accounting from CTP enqueue. A queued packet has a
generation/attempt identity but no timeout. The source port's existing
`TraComEventNotify`, called after successful `UbLink::TransmitStart`, marks the
attempt sent and starts its RTO at the current virtual time. The trace's
completion-time argument includes link overhead and is not used as the start.

A timeout can enqueue one next attempt, which remains untimed until it leaves
the port. Only actual active retry transmissions increment the retry counter.
Record generation, attempt, sent state and live outstanding membership reject
stale callbacks. ACK/error retirement invalidates the record: an already queued
old packet can still physically leave, but cannot rearm or resurrect its WQE.
This is not global cancellation of all queued packets.

Request-loss injection marks the packet at the real send callback and is
consumed at receiver CTP ingress. It therefore consumes actual link time and
starts the timeout, unlike the old enqueue-time injection. Packet metadata is
simulator-only and does not increase modeled wire bytes. No RTO value or
physical delay was increased to hide retries.

## Native acceptance

Complete restore/reapply build and native contracts passed in17.98 s (user
88.91 s, system6.64 s). New `ctp-wire-retry-contract` uses actual host/switch/
port/link/CTP objects and five scenarios, not a separate mock retry machine:

1. Initial source port blocked49 us with RTO10 us and retry limit2: zero retry
   or failure before release at50 us; eventual success.
2. Retry queued while source port blocked, then delayed success response:
   response retires the WQE; release does not rearm old work.
3. The same interleaving with Remote Abort: exactly one failed completion.
4. Actual first request loss: one retry, one target execution, one success.
5. Every actual request lost: two retries, one terminal failure, no execution.

Tests continue to150 us to expose old timer resurrection. Existing51 exact
READ wire cases, NO/RO/SO ordering, request/response loss, retry exhaustion,
congestion recovery, feedback and pacing contracts also pass.

Independent source review accepted the ordinary fabric path but identified
uncovered link-fault boundaries: rerouting an attempt to a different output
port may not match the registered callback, and a retained link-recovery copy
created before loss injection does not necessarily inherit its drop marker.
This stage does not claim link-fault recovery or arbitrary failover acceptance.

## Bounded drain observation

The adapter now supports read-only SIGUSR1 snapshots; the timing runner's
optional `--drain-timeout` collects fresh snapshots from both UDMA processes
and the fabric before stop. Two consecutive idle work snapshots are required.
Stale logs and incomplete lines cannot count as replies. Four observer parsing
tests pass in1.44 s (the incomplete-line case was added after live observation).
The observer never clears model state or advances simulated time and is not
a coordinated checkpoint barrier or proof that all native event queues empty.

## Official-driver repeat and independent error contracts

The no-sync official-driver QEMU regression passes12/12, including1 MiB
READ/WRITE fragmentation and the2 B–1 MiB WRITE sweep. Suite wall29.297 s;
launch/suite/stop35.31 s. Evidence:
`experiments/qemu-wire-retry-functional-20261009`.

Independent final-binary late-DMA/error replay contracts pass10/10 with no
assertion relaxation (0.945 s script measurement excluding interpreter startup
and cleanup). Evidence: `experiments/ctp-wire-retry-native-20261009`.
Actual-send accounting moves the timeout-exhaustion error from205.1 to205.2 us;
active-error and lost-error-response results remain0.7 and26 us. Malformed
length is fail-stop rejection, not successful pending-state drain.

## Synchronized1 MiB WRITE success

With400 Gbit/s per configured port and unchanged25.6 us RTO, the same official
`write_bw -B -s 1048576 -J 1 -I 64 -T 64 -n 64 -l 16 -Q 16` now returns success
on both guests. Perftest prints peak88528.51 and average88476.59; despite the
`MB/sec` label its source uses MiB/s. Average is92.77442883584 decimal GB/s.
The `-B` result already combines directions; the identical endpoint reports
are not additive. Actual per-port utilization is not measured here.

Fabric128/128 WQEs and32768/32768 TA requests/ACKs complete, transferring
134217728 payload bytes. Zero retries, retry exhaustion, target DMA errors,
runtime drops, trace-size mismatches and late post-terminal target operations.
Both UDMAs retire64/64 SQ entries with zero pending host DMA or remote service;
both save their device state. Two live work snapshots and final logs agree on
empty workload state. This is not a coordinated whole-system checkpoint test.

Wall time: shell46.966 s, command82.301 s, live drain observation0.261 s,
whole experiment131.962 s. Evidence and independent acceptance:
`experiments/qemu-write-1m-wire-retry-20261009`.

This closes the reproduced1 MiB enqueue-time retry storm on this configuration
(previously12/128 successes,212659 retries). It does not establish long-run
saturation, all size/operation combinations, fault recovery or full performance
acceptance. The command warns about large messages with CQ moderation; retain
that warning when interpreting this short64-iteration-per-direction sample.

## 200G sensitivity control and remaining queued duplicates

Changing only `UBSIM_PEER_LINK_RATE_GBPS=200` repeats the same synchronized
command successfully: peak44532.14 MiB/s, average36611.45 MiB/s
(38.3898877952 GB/s). The average is41.38% of the400G reading, not exactly
half. Both endpoint reports are still one exchanged bidirectional result.
Command85.270 s, whole trial134.261 s; live drain0.252 s.
Evidence: `experiments/qemu-write-1m-wire-retry-200g-20261009`.

All128 WQEs and32768 original TA requests complete, with zero target error,
failed WQE, runtime drop or pending DMA. Both UDMAs retire64/64, report idle,
and save state. However,11251 duplicate WRITE requests and TAACK replays were
observed, with11251 late responses. Active-record retransmission count is zero
because these retries left the source after their original ACK had already
retired the record. Do not describe this as zero physical retransmission or
clean linear bandwidth scaling. The implementation explicitly permits queued
stale packets to depart; target duplicate suppression avoids a second DMA.

Next bounded repair: invalidate or discard stale queued attempts before
physical departure, while preserving queue/allocator/credit accounting and
not cancelling genuinely in-flight packets. Add distinct counters for queued,
active-sent and stale-sent retries and repeat the200G control. Do not increase
RTO or suppress target duplicate accounting to manufacture a better result.
