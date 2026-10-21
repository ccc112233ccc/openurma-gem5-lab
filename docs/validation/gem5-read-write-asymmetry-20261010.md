# 128-B READ/WRITE asymmetry: source-side WRITE DMA serialization

## Matched measurement

The `modern` ready checkpoint was restored with one 400-Gbit/s UB port per
node, one 500-ns-per-pass switch, 3.2-GHz ArmO3 ROI, and DDR5-6400. Both
guests completed a no-traffic Atomic → O3 → Atomic toggle first. READ and
WRITE then each used 128 B, one Jetty, one posting thread, one shared JFC,
16,384 operations, `-T 8192 -R 8192 -C 524288 -l 256 -Q 1 -I 0 -p 0`.
Lifecycle synchronization and CPU statistics were enabled. Both command pairs
exited zero and passed five-participant lifecycle fence checks. Raw data are
under `experiments/gem5-read-write-128-profile2-20261010/`; the paired rows
are in `cases/results.csv`.

| Same workload | READ | non-inline WRITE |
| --- | ---: | ---: |
| Average payload throughput | 1,203.85 MiB/s | 592.38 MiB/s |
| Message rate | 9.861916 Mpps | 4.852785 Mpps |
| Reciprocal message interval | 101.40 ns | 206.07 ns |
| Initiator main O3 core cycles/operation | 331.3 | 667.2 |
| Initiator main O3 core committed instructions/operation | 376.8 | 848.4 |
| Initiator main O3 core IPC | 1.137 | 1.271 |
| Initiator main O3 `SQFullEvents` | 63,197 | 1,091,739 |

The benchmark thread used core 2 for READ and core 3 for WRITE. These cores
have the same model configuration, but migration and the differing polling
work mean the instruction counters alone do not establish the initial cause.
WRITE's extra cycles and polling are consistent with slower device progress.
The earlier independent 12-case sweep found one-Jetty READ 9.765 and 10.181
Mpps, and WRITE 4.853 and 4.846 Mpps, so the twofold gap is repeatable in
the warmed mode; see `gem5-rma-128-jetty-compare-20261010.md`.

## Causal path in this model

`SqPipeline::fetching` is per Jetty. `ProcessSqOnce()` refuses to handle the
next WQE while this flag is true. For non-inline WRITE, `HandleSqWqe()` starts
a source payload `ReadToken()` and retains `fetching` until the asynchronous
read callback invokes `SubmitSqPayload()`. Only after the frame is sent does
`SubmitSqPayload()` call `ReleaseSqFetch()` to clear the flag. In contrast,
READ reserves its WQE, sends a small request, and immediately calls
`ReleaseSqFetch()`. Its remote read and local response DMA can overlap across
many requests. This is queue issue serialization at the model, not a 400-Gbit/s
wire limit or an inherent RDMA ordering rule.

In the paired run, the source WRITE payload stage completed 16,384 reads at
an average **204.69 ns** from start to callback, with peak concurrency **one**.
The observed WRITE interval was **206.07 ns**: `1 / 204.69 ns = 4.885 Mpps`,
only 0.7% above measured 4.853 Mpps. Target WRITE DMA averaged 205.14 ns but
reached eight concurrent writes, so its per-operation latency does not imply
the same throughput cap. WRITE ACK averaged 1.689 us with 15 concurrent
transactions; these overlapping stages must not be added as a serialized
path. READ CQ publication averaged 1.231 us but reached 64 concurrent
publications. The source READ window had zero stalls; the network had no
drop, pacing or congestion evidence in the earlier full sweep.

The earlier Jetty scan reinforces the mechanism: one-Jetty WRITE source DMA
peak concurrency was one, two Jettys raised it to two, and four Jettys to
four, at essentially unchanged mean 205 ns. WRITE rate rose from 4.85 to
7.98 to 9.47 Mpps, while READ stayed near 9–10 Mpps. Beyond four Jettys,
guest posting and queue overhead dominate; this experiment does not assign
all of that remaining ceiling to one resource.

An exploratory 128-B `-I 128` WRITE used an inline WQE and the source payload
DMA counter did not increase over 5,461 WQEs handled by the model. However,
the guest reported `Couldn't post jfs ... status:22` after about 5,376
submissions before any valid bandwidth row. The simulator was stopped. This
is evidence that inline bypassed source DMA, **not** a valid inline bandwidth
comparison. The provider's queue-capacity check counts 64-B WQEBBs, while
`run_once_bw()`'s posting window counts WQEs. A 128-B inline WRITE consumes
three WQEBBs versus one for the non-inline case; the model recorded exactly
16,383 additional WQEBBs (5,461 completed inline WRs) before this provider
post failure. The benchmark's fixed `-T 8192` therefore does not make the
inline and non-inline SQ footprints equivalent. This is a separate workload/
queue-sizing mismatch, not evidence of the non-inline DMA bottleneck.

## Interpretation and next model change

The current twofold single-Jetty difference is a **performance-model
limitation**, with a specific per-Jetty non-inline WRITE DMA issue gate. It is
not a demonstrated URMA or physical NIC feature and not a functional transfer
failure. A realistic model needs a bounded number of payload DMA reads in
flight per Jetty, plus ordered frame emission and correct queue teardown;
simply clearing `fetching` early without ownership/order tracking would be
unsafe. The actual per-Jetty hardware capacity is unknown and should remain
a calibration parameter until measured or documented. A future fix should
compare J=1/2/4 READ/WRITE with fixed `-I 0` and verify ordering, errors,
DMA concurrency, and throughput. No physical WRITE curve was supplied, so
physical READ-versus-WRITE direction or ratio cannot be inferred.

Update, 2026-10-10: the bounded source-payload pipeline was implemented and
verified with window 1 versus 8 and J=1/4 full-system runs. See
`gem5-write-payload-pipeline-20261010.md` for the current results. The
paragraph above records the diagnosis and plan at the time of this trace.
