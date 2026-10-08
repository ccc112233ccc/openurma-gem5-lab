# Longer synchronized WRITE and observed WQE concurrency

## Preserved evidence and new gate

The existing no-sync depth512 SEND/READ/WRITE suite already passed. This stage
does not repeat that as new evidence. It checks a longer synchronized WRITE
workload, actual device-local pending RMA high watermarks and SQ wrap counters.
The native CTP admission peak512 counts TA/segments, not512 official WQEs.

`rma_pending_peak` is the model-wide local `pending_rma_` high watermark. For
WRITE it begins after payload fetch and SQ reservation, before network issue;
it ends after all responses complete, before ordered SQ completion retirement.
It excludes WQEs still fetching payload, remote target service and SEND. In
this single-Jetty WRITE trial it is evidence of actual issued/uncompleted RMA
WQE concurrency, not merely configured queue capacity. Application `-T512`
limits unretired WRs; it does not force all512 to overlap in this phase.

## Harness error found and repaired

Independent source review caught a parameter coupling: the runner previously
used requested depth for both `-T` (SQ depth) and `-I` (maximum inline bytes).
Defaults both64 hid the problem. The first512-depth diagnostic used `-I512`
and both official tools rejected it because device max inline is256. Fabric
submitted0 WQEs; no bandwidth was measured, even though the idle system drained.
Command82.070 s, total130.322 s. Evidence:
`experiments/qemu-write-1m-depth512-20261009`.

The runner now keeps `--write-inline` independent, default64, with separate
`--write-depth`. It also reads both guests' actual JFS depth/inline capabilities
before future WRITE commands and rejects unsupported settings without quietly
clamping them. Five command/capability parsing tests pass in0.05 s. This
preflight was added after the already-running corrected trial began; it is
not claimed to have executed in that trial.

## Corrected workload

400 Gbit/s per configured port, required virtual-time synchronization,
unchanged25.6 us CTP RTO and host/fabric delays. Each endpoint executes:

```sh
urma_perftest write_bw -d udma0 --eid_idx 0 --ctp -B -s 1048576 \
  -P 21410 -J 1 -I 64 -T 512 -n 2048 -l 16 -Q 16 -p 0
# Client adds: -S 10.0.0.1
```

This is2048 operations per direction,32 times the earlier64-operation trial.
It changes both depth and iteration count relative to that short run, so any
bandwidth difference cannot be attributed solely to depth. Native/driver
completion, real concurrency and wrap evidence must be checked independently.
Large-message/CQ-moderation warnings remain part of the measurement record.

## Corrected synchronized result

Evidence: `experiments/qemu-write-1m-depth512-long-20261009`.
Both tools report `JFS depth512`, `Max inline size64`,2048 iterations,
peak89575.02 MiB/s and average89544.36 MiB/s =93.89406683136 GB/s. This is
already the combined `-B` result; the identical exchanged endpoint rows must
not be added together. It remains a bounded test, not steady-state or
cross-platform throughput certification.

Each UDMA completes2048/2048 WQEs and records an actual local RMA peak511,
not512. Each retires2048 WQEBBs and reports2 hardware SQ consumer wraps.
Fabric4096/4096 WQEs and1048576/1048576 original TA/ACKs complete, with
4294967296 payload bytes (4 GiB). The native TA admission peak512 is a separate
metric and is not the device WQE concurrency result. All retry categories,
failed WQEs, target DMA errors, trace mismatches and runtime drops are zero.
Both devices and the fabric drain work; device states are saved. No whole-system
checkpoint or restore claim follows from these device saves.

Independent review confirms these results in the experiment's `acceptance.txt`.
It also retains1183 `unknown_queue_writes` per device and the existing
`ex_tp_info` cleanup warning. Zero failed work/error counters must not be
presented as absence of all diagnostics; this stage does not resolve or waive
every driver management path.

Wall time: shell47.927 s, command145.547 s, drain0.262 s, whole196.113 s.
No model rebuild was needed in this stage: only harness/acceptance code changed.
The prior short400G trial's92.77 GB/s is context, not a depth-only comparison,
because this trial also changes iteration count and uses the newer cancellation
model. Actual511 concurrency and two wraps are the accepted new observations.

## Reusable independent result audit

`tools/check_qemu_write_trial.py EXPERIMENT_DIR` is a read-only WRITE-only audit
that checks required synchronization, command/printed SQ depth and inline,
both actual guest return codes, raw rows, original bytes/TA/WQE accounting,
device and fabric errors, pending work and host DMA. It converts the MiB/s
value once, separately reports observed RMA peak/wraps, and fails closed if
required evidence is missing. Seventeen parser counterexamples pass; fixtures
test parsing only, not hardware behavior. Historical good trials pass, while
the failed inline512 trial is rejected despite its idle/drained state.

Final parser test wall times:17 audit tests0.04 s,5 command/capability tests
0.03 s,4 observation tests1.42 s. Capability parsing was also checked against
real stored guest output (depth1024, inline256 on both endpoints).

Acceptance remains open for longer READ/SEND timing, controlled one-variable
concurrency/latency sensitivity, repeated resource reuse across timed cases,
payload-integrity evidence and multi-Jetty/multiport performance. Earlier
official functional depth512 coverage is retained; do not restart that work.
