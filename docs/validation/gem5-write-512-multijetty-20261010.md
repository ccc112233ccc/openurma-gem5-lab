# 512-B WRITE bandwidth versus Jetty count (2026-10-10)

**Measurement-state correction:** A later no-traffic CPU-mode control showed
that this checkpoint's first timed run begins after `atomic_noncaching` setup,
whereas subsequent setup runs in cache-enabled `atomic` mode. Sequential
first-run Jetty points and their later repeats therefore do not all share
the same memory-mode history. See
`docs/validation/gem5-write-512-warmup-20261010.md`; normalize the mode
before using this sweep for precise Jetty scaling ratios.

## Why synchronized bandwidth previously required one Jetty

The lab's pinned `urma_perftest` has a distributed virtual-time extension.
Its `perftest_dist_time_rendezvous()` rejected `-J > 1` for every synchronized
test. That guard was a conservative scope limit of the timing extension, not
a UDMA capability or a physical Jetty limit. The iteration-mode WRITE/READ
bandwidth loop posts through all Jettys from one process and has one pair of
process-level lifecycle fences around the complete traffic loop. The guard
now permits multiple Jettys for READ/WRITE bandwidth with one endpoint pair;
the latency paths and SEND bandwidth retain their prior single-Jetty limit.
The patch is
[`patches/source/umdk/0012-synchronize-unidirectional-rma-bandwidth.patch`](../../patches/source/umdk/0012-synchronize-unidirectional-rma-bandwidth.patch).
The official provider subtree was not modified.

Rebuilding the initramfs changes the guest program in checkpointed RAM. The
`start-ready` selector now compares the current image SHA-256 with checkpoint
metadata for the modern profile. It rejected the old checkpoint after the
rebuild. One cold boot, timer/OOB validation and a 101-s coordinated save
created `checkpoints/shell-ready-modern-ddr5-multijetty-write`; the guest
`urma_perftest` SHA-256 was
`076f4055d22bad90d7dfeea460db1494a38e0f77a32748c6c27d22dedcf6ee84`
on both nodes. All performance runs restored this checkpoint.

## Method

One-direction CTP/RM `write_bw`, 512 B, one Jetty group per process, one
400-Gbit/s UB port per endpoint and one switch with 500 ns delay **per pass**.
The modern ROI uses 3.2-GHz ArmO3 and modeled DDR5-6400. Each case completes
16,384 WRITEs in total; `-n` is 16,384 divided by the Jetty count. Both sweeps
use `-l 256 -Q 1 -I 0 --jettys_pre_jfr 1 -C 32768`. The per-Jetty JFS/JFR
requested depth is either 512 or 8192. `urma_perftest` clips the effective
JFS depth to `-n` in iteration mode: with requested 8192, it prints effective
depths 8192/8192/4096/2048/1024 for J=1/2/4/8/16 respectively. The
8192-request sweep matches the user's earlier queue-depth argument but not
its much larger JFC depth, which is unnecessary
for this bounded workload. The 32,768-entry JFC safely covers the maximum
aggregate initial post window. The test driver is
[`tools/run_gem5_write_jetty_sweep.py`](../../tools/run_gem5_write_jetty_sweep.py).

The first attempt used `-C 4096`. J=1/2/4/8 completed, while J=16 stalled
with 16 Jettys able to fill a larger aggregate send window than the shared
JFC. The source UDMA reported 44 UBASE errors and unretired SQ completions.
That attempt is diagnostic only; its logs remain under
`experiments/gem5-write-512-jetty-sweep-20261010/`. The two accepted sweeps
restarted from the same checkpoint and used `-C 32768` for **every** Jetty
count. Complete UART transcripts and per-case five-participant fence logs
are under `experiments/gem5-write-512-jetty-sweep-jfc32768-20261010/` and
`experiments/gem5-write-512-jetty-sweep-depth8192-20261010/`. The machine
readable combined table is
[`results/gem5-write-512-jetty-sweep-20261010.csv`](../../results/gem5-write-512-jetty-sweep-20261010.csv).

| Jettys | Requested T512 MiB/s | T512 Mpps | Requested T8192 MiB/s | T8192 Mpps |
| ---: | ---: | ---: | ---: | ---: |
| 1, first | 2,072.28 | 4.244 | 1,904.19 | 3.900 |
| 2, first | 4,074.04 | 8.344 | 3,799.69 | 7.782 |
| 4 | 4,239.44 | 8.682 | 4,689.76 | 9.605 |
| 8 | 4,351.50 | 8.912 | 4,585.82 | 9.392 |
| 16 | 4,359.79 | 8.929 | 4,177.94 | 8.556 |
| 2, repeat | 4,185.24 | 8.571 | — | — |
| 1, repeat | 2,303.17 | 4.717 | 2,302.19 | 4.715 |

At depth 512, the repeated J=2 run is 1.82 times the repeated J=1 run.
At depth 8192, J=4 is 2.04 times the repeated J=1 run, but J=8 and J=16 do
not improve on J=4. Increasing single-Jetty depth from 512 to 8192 did not
raise its first-run rate. The repeated J=1 result is higher than the first
run (11% at depth 512; 21% at depth 8192), so order/warmth is material and
the ratios should not be read as exact hardware scaling. `urma_perftest`
traverses the Jettys in one posting thread here; the improvement does not
demonstrate parallel application threads.

All 13 accepted command pairs exited zero and printed two matching rows per
case. Their lifecycle enter/leave generations matched across both gem5
hosts, both UDMA processes and the ns-3 fabric. The depth-512 run completed
114,688/114,688 source SQ WQEs and the depth-8192 run 98,304/98,304, with
zero UBASE errors. Both fabrics delivered every frame and reported zero
runtime drops, retransmissions and native WQE failures. Case wall times were
36.6–70.0 s at depth 512 (318.8 s total) and 42.4–78.4 s at depth 8192
(307.9 s total). Simulator processes were stopped after each sweep.

These are WRITE results; the supplied physical curve is for READ, so this
experiment tests Jetty scaling rather than WRITE accuracy against hardware.
The best 512-B WRITE result here is 4,689.76 MiB/s, far below the nominal
400-Gbit/s link rate. Further work can instrument per-Jetty SQ/CQ service and
guest posting to explain the plateau, and compare against physical 512-B
WRITE data if available.

## Single-port ceiling and current bottleneck evidence

The depth-8192 run manifest sets `ub_port_count=1` and
`peer_link_rate_gbps=400`. The ns-3 adapter creates one endpoint and one
switch port per side at that rate. Both UDMA profiles contain only `port=0`;
the source has 98,304 transmitted WRITE frames and 98,304 received ACKs
across the six cases. Increasing `-J` changes the number of logical Jettys,
not physical UB ports.

400 Gbit/s is 50 **decimal** GB/s, or 47.68 GiB/s, of nominal link capacity.
At the best J=4 point, the measured 4,689.76 MiB/s is 4.918 decimal GB/s
or 39.34 Gbit/s of application payload: 9.84% of the nominal link bit rate.
The measured 9.604623 Mpps is one 512-B WRITE about every 104 ns; filling
400 Gbit/s with 512-B payloads alone would require 97.66 Mpps, before
protocol overhead. The UDMA `tx_wire_bytes` counter records 576 bytes per
WRITE and 64 per ACK in the host/fabric adapter framing. Those are **adapter
frame bytes**, not a verified ns-3 physical-port serialization count, so
they cannot by themselves determine an exact on-wire payload ceiling.

The fabric reports `ctp_wire_pacing_waits=0`, `ctp_queue_marks=0`,
`ctp_congestion_rate_cuts=0`, zero drops/retransmissions, and a small
`ctp_peak_queue_bytes=556` over this sweep. This is evidence against a
400-Gbit/s link or switch congestion ceiling. The source completed every
WQE, with no SQ-depth rejects or UBASE errors. The current ceiling is the
*end-to-end operation rate* before the link fills. Each WRITE requires
guest posting, SQ descriptor and payload DMA, target DMA, an ACK, CQE DMA,
and guest polling; one `urma_perftest` thread loops over all Jettys and
polls one shared JFC with `-Q 1`. At 3.2 GHz, 9.604623 Mpps corresponds to
about 333 CPU cycles per completed WRITE if expressed as a single-core
budget; it is **not** a measured CPU CPI or proof that the CPU alone is the
bottleneck. The present aggregate counters do not isolate posting, DMA,
ACK service, or CQ publication as the dominant stage. The 500-ns switch
processing delay applies per traversal, but concurrent outstanding WRITEs
can overlap it. More Jettys improve J=1 to J=4 and then plateau/decline;
the test does not show a port-rate limit.
