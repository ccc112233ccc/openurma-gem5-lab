# RMA WRITE source-payload pipeline: model audit and full-system check

Date: 2026-10-10. This report evaluates the one-Jetty READ/WRITE asymmetry
in the one-port modern gem5 + standalone UDMA + ns3ub configuration. It is
not a measurement of Kunpeng 950 DMA-engine depth.

## Why the previous 2:1 result was a model artifact

The prior matched 128-B test measured 9.862 Mpps READ and 4.853 Mpps WRITE.
The UDMA model kept each Jetty's SQ fetch gate occupied until a non-inline
WRITE source-payload DMA completed. The measured fetch averaged 204.69 ns
and peaked at one in flight, predicting 4.885 Mpps from its reciprocal.
READ released the same gate after issuing its request. The fabric had no
runtime drops, retransmissions, congestion cuts, or pacing waits. This
identifies the simulated source fetch policy as the dominant reason for that
WRITE ceiling; it does not establish a physical READ/WRITE ratio.

The [openEuler URMA API guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)
defines separate `place_order`, `comp_order` and `fence` flags. The test's
zero-initialized WQEs do not request those flags. The
[URMA user guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20User%20Guide.ch.html)
describes asynchronous WR submission and later JFC completion, and lists
JFS queue depth separately from device capabilities. Neither guide specifies
one source DMA per Jetty or a numeric DMA-slot count. The evidence and source
locations are in `urma-rma-public-evidence-20261010.md`.

## Change

Non-inline WRITEs may now fetch source buffers concurrently within a
configurable **per-Jetty** `write_payload_read_window`, default 8. Each WQE
reserves its SQ completion position before fetch. A bounded prepared-WRITE
queue holds the fetched payload and emits frames in WQE order even if DMA
callbacks return out of order. A subsequent READ, SEND, or inline WRITE
cannot pass a prepared WRITE. Existing strong-order and fence gating,
remote ACK, CQE retirement, DMA error, destroy and checkpoint paths remain
accounted for. `--write-payload-read-window 1` reproduces the old source
fetch policy; the launcher records the choice in its manifest. The ready
checkpoint is compatible because the queue is empty at the checkpoint.

This 8-slot default is a **sensitivity assumption**, not a calibrated UDMA
hardware property. `dma_max_outstanding` controls the separate per-WQE DMA
chunk issue path and is not a source-WRITE queue-depth measurement.

## Matched full-system comparison

Both runs restored `shell-ready-modern-ddr5-multijetty-write`, synchronized
the two guests, and performed the same no-traffic Atomic→O3→Atomic toggle
before measurements. One 400-Gbit/s port per node, one switch with 500 ns
service per traversal, official CTP/RM provider, ArmO3 only during ROI,
128-B messages, `-T/-R 8192 -C 524288 -l 256 -Q 1 -I 0`, and 16,384 total
operations stayed fixed. Only the source-payload window changed between
the two fresh restores. The four-case window-8 sequence includes READ and
WRITE at J=1 and J=4; each J=4 Jetty ran 4,096 iterations. The comparison
is not a statistical confidence interval from repeated randomized runs.

| Window | Operation | Jettys | Average MiB/s | Mpps | Wall time |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | READ | 1 | 1,183.72 | 9.697 | 52.2 s |
| 1 | WRITE | 1 | 592.72 | 4.856 | 79.7 s |
| 8 | READ | 1 | 1,175.45 | 9.629 | 52.3 s |
| 8 | WRITE | 1 | 1,182.94 | 9.691 | 57.4 s |
| 8 | READ | 4 | 1,199.69 | 9.828 | 57.4 s |
| 8 | WRITE | 4 | 1,157.77 | 9.484 | 60.0 s |

With one Jetty, widening the source fetch window raised WRITE throughput
by **1.996×** while READ changed by -0.7%. The window-1 WRITE source
payload stage had 16,384 completed fetches, mean 204.7 ns and peak
concurrency 1. Window 8 had 16,384 completed fetches, mean 209.1 ns and
peak concurrency 8. Thus overlap changed, not individual DMA latency. The
J=4 result does not show further throughput gain after J=1 reaches roughly
the same 9–10-Mpps operation-rate region as READ. The older window-1 J=4
WRITE run measured 9.467 Mpps, but came from a separate restored sequence;
it is supporting context rather than a strict paired delta.

All four window-8 command pairs exited successfully. Source UDMA retired
65,536 WQEs with zero depth/decode rejects, READ-window stalls and UBASE
errors. ns3ub submitted and completed all 65,536 native WQEs, delivered all
131,072 frames, and reported zero drops, retransmissions, congestion cuts,
queue marks and wire-pacing waits; the largest CTP TA outstanding count was
192, below the native 512 segment limit. CQ publication slots touched
64/64 but `cqe_wait` was zero and JFC pending peaked at one, so that peak
alone is not evidence of sustained CQ backpressure.

Raw artifacts and resolved launch settings:

- Window 1: `experiments/gem5-write-payload-window1-20261010/cases/results.csv`
  and `run-dual/run-manifest.txt`.
- Window 8: `experiments/gem5-write-payload-window8-20261010/cases/results.csv`
  and `run-dual/run-manifest.txt`.
- Both experiment trees retain UART, per-case UDMA/fabric fence logs, final
  component logs and gem5 ROI statistics. Both custom simulators were stopped.

## Remaining fidelity limits

The supplied physical 128-B READ result is 23.098 Mpps; the current J=1
simulated READ remains 9.63 Mpps, about 58% low. This WRITE fix does not
close that gap. The known read-side evidence points to substantial guest
CPU/posting pressure, but does not yet assign its exact share versus model
device latency. The physical 64-KiB base page versus guest 4-KiB page,
the 384-logical-CPU/2.3-GHz physical host versus four modeled 3.2-GHz
cores, and unknown UDMA microarchitecture further limit calibration. The
present result supports the mechanism and its sensitivity, not an 80%
physical accuracy claim. No physical WRITE curve was supplied to select a
hardware-faithful source window.

The native ns3ub fabric currently maps all guest Jettys on an endpoint to
one native Jetty. The separate audit `ns3ub-rma-model-audit-20261010.md`
explains why directly assigning distinct native IDs deadlocks the Entity
TA sequence state and can collide in replay/ACK maps. Thus the J=4 curve
must not be interpreted as an independent-native-Jetty scaling law. The
single-port wire serialization and switch delay are shared as intended.

## Verification

Native UDMA component: 9/9 tests pass, including out-of-order fetch return,
window 1, mixed READ, strong order, failure, destroy and quiescence.
Docker device build: 12/12 tests pass. CLI Python tests: 16/16 pass;
shell syntax, Python compilation and `git diff --check` pass. The matched
full-system runs above check actual provider, guest, UDMA and ns3ub behavior.

The opt-in native ns3ub two-Jetty contract has a passing single-Jetty
control and an expected failing split-Jetty counterexample: the second
request cannot pass Entity TA admission (`sendNext=1` while that Jetty
starts at local TASSN 0). The unchanged default ns3ub suite passed on its
complete rerun (22.14 s). An earlier complete run saw a timing-sensitive
TAACK-loss assertion count two retransmissions rather than one; its isolated
rerun and the complete rerun passed. The counterexample is outside the
default passing suite. Exact commands and observations are in
`integrations/ns3ub/README.md`.

Next calibration should measure or infer the source CPU posting/polling
interval and completion-service distribution for 128-B READ, then test one
factor at a time against the supplied physical curve. Treat the native
Jetty identity repair as a separate transport state-machine change with
an explicit cross-Jetty sequence/ACK/replay contract.
