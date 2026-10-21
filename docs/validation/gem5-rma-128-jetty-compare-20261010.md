# One-port 128-B READ/WRITE scaling versus Jetty count (2026-10-10)

## Method

Restored `shell-ready-modern-ddr5-multijetty-write` once. Both guests first
performed a no-traffic Atomic-to-ArmO3-to-Atomic toggle, leaving the memory
mode in the warmed, cache-enabled state before any timed RMA. The modern ROI
used 3.2-GHz ArmO3, DDR5-6400, two nodes, one 400-Gbit/s UB port per node,
and one switch with 500 ns per pass. No additional port or application thread
was introduced.

Each case used one-direction CTP/RM `read_bw` or `write_bw`, 128 B, total
16,384 operations, `-T 8192 -R 8192 -C 524288 -l 256 -Q 1 -I 0 -p 0`,
and one shared JFC. `-J` was 1, 2, 4, 8, or 16; `-n` was `16384 / J`, so
total operations stayed constant. READ and WRITE were interleaved at each J;
J=1 for both was repeated at the end to assess order/warmth effects. The
same physical UB port carried every case; only the TCP control port number
changed between independent commands. The tool's effective per-Jetty JFS
depth is clipped to `-n`: 8192/8192/4096/2048/1024 for J=1/2/4/8/16,
despite the constant requested `-T 8192`. This workload consequence must be
considered when interpreting J=4–16.

The [driver](../../tools/run_gem5_rma_jetty_compare.py) checked both Jetty
headers, identical result rows, zero guest exit codes, and five-participant
gem5/UDMA/fabric lifecycle fence deltas for every case. The command wall
times totalled 632.6 s for 12 pairs. Raw UART, per-case fence excerpts,
machine manifest, and component logs are under
`experiments/gem5-rma-128-jetty-sweep-20261010/`. The [CSV](../../results/gem5-rma-128-jetty-compare-20261010.csv)
contains all values, including wall time and both repeats.

## Results

| Jettys | READ MiB/s | READ Mpps | WRITE MiB/s | WRITE Mpps |
| ---: | ---: | ---: | ---: | ---: |
| 1, first | 1192.02 | 9.765 | 592.46 | 4.853 |
| 2 | 1230.42 | 10.080 | 974.02 | 7.979 |
| 4 | 1144.21 | 9.373 | 1155.67 | 9.467 |
| 8 | 1076.45 | 8.818 | 1069.20 | 8.759 |
| 16 | 1045.98 | 8.569 | 1057.34 | 8.662 |
| 1, repeat | 1242.79 | 10.181 | 591.57 | 4.846 |

READ's best point was J=2, only 3.2% above its first J=1 and 1.0% below
its warmed J=1 repeat. READ J=16 was 15.8% below the repeat. WRITE rose
from 4.85 Mpps at J=1 to 9.47 Mpps at J=4, a 1.95× increase against either
J=1 measurement, then decreased to 8.66 Mpps at J=16. WRITE J=1 changed
by just -0.15% on repetition, while READ J=1 increased by 4.26%; the
READ J=2 advantage is therefore within the observed warm/order variation.
The supplied **one-Jetty** physical 128-B READ reference is 23.098 Mpps;
even the best multi-Jetty simulated READ is 56.4% lower. This is a ceiling
comparison, not a parameter-matched J=2 accuracy measurement. No physical
WRITE curve was supplied for an accuracy comparison.

## Resource evidence and interpretation

The source UDMA retired all 196,608 WQEs across the 12 cases with zero SQ
depth rejects and UBASE errors. Its global READ pending count peaked at 256,
but `read_window_stalls=0` across the entire scan; CQ publication queue
wait accumulated zero time. The fabric delivered all 393,216
request/response or WRITE/ACK frames, with zero drops, retransmissions,
queue marks, congestion cuts, or wire-pacing waits. Thus neither a port-rate
ceiling, a stalled 256-credit READ issue window, nor queued CQ publication
explains this small-message Jetty trend in these cases.

The pinned perftest's `run_once_bw()` loops over Jettys in one posting thread
and polls one shared JFC. Device READs also share one global pending-READ
budget, whereas additional WRITE Jettys create independent SQ paths. This
architecture is consistent with WRITE gaining overlap at J=2–4 and READ
gaining little before extra per-Jetty loop and resource work reduces rate.
The subsequent paired CPU/device profile located a concrete per-Jetty WRITE
source DMA issue gate: at J=1, source DMA averaged 204.69 ns with concurrency
one, corresponding to 4.885 Mpps versus measured 4.853 Mpps. READ bypasses
that gate. See `gem5-read-write-asymmetry-20261010.md` for code path and raw
evidence. The new run's gem5 `stats.txt` files were empty after stop; the
subsequent paired profile collected O3 stats. The precise J=8/16 decline
remains unassigned.

All custom run-root simulator processes were stopped after the sweep.
