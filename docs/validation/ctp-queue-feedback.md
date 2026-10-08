# Queue-triggered CTP congestion feedback

Date: 2026-10-08. Native macOS/ARM64 ns-3-UB validation; no guest cold boot.

## Ownership and implementation

1. `UbCtpQueueFeedback` uses the native switch post-enqueue hook and actual
   egress backlog. Above an explicit byte threshold it marks CNA16 FECN.
   TAACK and CNP are excluded; this is not an IPC-poll or host-time callback.
2. The receiving CTP endpoint returns a native CTP_CNP packet, reversing the
   source/destination Entity association. It uses the ingress member port,
   the CNP priority queue and the real reverse ns-3 links/switch. Repeated
   marks within the configured feedback interval are suppressed.
3. The sending CTP endpoint receives that packet and halves the shared
   destination-node/Entity/VL rate, with its configured minimum. Optional
   additive virtual-time recovery restores the line rate.
4. Pacing applies to actual DATA queue dequeue as well as new transaction
   admission. This matters because a WQE can have many fragments queued
   before the first feedback arrives. ACK/CNP bypass the data-rate gate.
   A shared wake tracks every blocked port; the native allocator arbitrates
   the different source Entity queues.

Patch 0007 also fixes CNA16 FECN serialization: the old implementation wrote
only the first 16 bits of a union containing ordinary, padded C++ structs.
FECN was outside those bits. Modes 2 and 4 now explicitly pack/unpack their
fields. The test checks encoded bytes and round trips all four FECN values.
Other header formats and mode-0 policy are not changed.

## Scope of protocol claims

The [official UB specification portal](https://www.unifiedbus.com/zh) identifies
the current specification and requires accepting its agreement for the base
document. That document was not retrieved in this stage; no claim of complete
normative conformance is made. The concrete integration uses the pinned
[ns-3-UB source](https://github.com/halfmanli/ns-3-ub/tree/d6aa9e242d5a93f5bbd1ad54f39b1620c1b8757b),
including its CNA16 FECN fields, CTP_CNP envelope, Entity keys, allocator and
control queues. In particular the native compact CNP envelope is not claimed
as a complete standard CNP/CETPH wire implementation.

Threshold marking, feedback suppression, multiplicative decrease and additive
recovery are explicit **reference-model policies**, not verified chip constants
or a full DCQCN/LDCP implementation. They are configurable and marking/recovery
are off by default. The circuit is functional without claiming that one chosen
algorithm is required by UB.

## Reproducible experiment

Topology: two source hosts and one destination, one native switch, one
10-Gbit/s port per host, 100-ns link propagation, native CBFC. Each source has
two source Entities and two Jetties, all sharing destination Entity 1/VL 1.
Each Jetty writes 1 MiB. The 10-Gbit/s setting is a controlled test parameter,
not a change to the lab's normal 400-Gbit/s port configuration.

Reference policy: mark above 8192 B; CNP interval 20 us; halve on CNP; minimum
1 Gbit/s; recover by 1 Gbit/s per 5 us. Rates are sampled in virtual time;
samples verify that the two Entities of each source see identical rates.

| Scenario | Peak switch backlog (B) | FECN marks | CNP sent / received cuts | Aggregate goodput (Gbit/s) |
|---|---:|---:|---:|---:|
| Two sources, feedback off | 1,929,644 | 0 | 0 / 0 | 9.90186 |
| Two sources, feedback on | 33,056 | 721 | 387 / 387 | 9.32755 |
| One source, feedback on | 4,132 | 0 | 0 / 0 | 9.89069 |

The congested case suppresses 334 redundant notifications and records 6,744
data-queue pacing checks that block transmission. Both senders reach a sampled
minimum rate of 1.25 Gbit/s and recover to 10 Gbit/s after the traffic drains.
All four WQEs complete exactly once with success. Their batch completion-rate
Jain index is 0.999993; this proves no starvation in this bounded symmetric
case, not general long-running fairness for arbitrary topologies.

Goodput is useful WRITE bytes divided by the final completion's simulated
time from submission. It includes startup and completion overhead. Queue
reduction and the modest throughput cost are measured, not fitted targets.

## Regressions and wall time

The complete patch replay/build and ten tests passed in **45.00 s**: recovery
unit test, three new topology cases, and six existing ordering/reliability/CNP
process contracts. Focused topology runs take roughly 0.01–0.95 s wall time
on this host, including process/library startup variation. No Linux guest
boot was required.

```sh
UBSIM_EXECUTION_MODE=native ./scripts/build-ns3ub-adapter.sh
# Individual topology cases, after building:
sources/ns-3-ub/build-macos/scratch/ns3.44-ctp-feedback-contract
sources/ns-3-ub/build-macos/scratch/ns3.44-ctp-feedback-contract enabled
sources/ns-3-ub/build-macos/scratch/ns3.44-ctp-feedback-contract enabled single
```

Adapter controls (all times in ps, rates in bit/s):

```text
--ctp-mark-threshold-bytes 8192 --ctp-cnp-interval-ps 20000000
--ctp-recovery-interval-ps 5000000 --ctp-recovery-step-bps 1000000000
```

Adapter shutdown counters expose marks, peak backlog, CNP generation,
suppression, received rate cuts and wire pacing waits. Larger/asymmetric
fairness sweeps and full-system URMA congestion tests belong to the later
performance/regression matrix; they are not implied by this native contract.
