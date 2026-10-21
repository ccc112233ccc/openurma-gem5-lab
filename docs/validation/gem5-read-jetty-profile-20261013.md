# 128-B READ / multi-Jetty profiling (2026-10-13)

> **2026-10-14 correction:** This 32768-total-operation sweep changed
> `urma_perftest`'s automatic peak-timing mode across Jetty counts: J=1
> had 32768 iterations per Jetty and automatically set `no_peak`, while
> J=2/4 retained per-operation timestamping. The J=1/2/4 slowdown and
> additional source-core instructions reported below are therefore confounded
> by benchmark bookkeeping. The physical test used `-n 16384`, so the 47%
> absolute comparison below is also not parameter matched. See
> `gem5-read-loop-profile-20261014.md` for corrected measurements.

## Question and controlled setup

Does raising Jetty count improve the simulated small-message READ rate, and
where does the remaining gap to the supplied physical 128-B point occur?
This is a throughput profile of the **current model**, not a claim that the
physical machine has the same multi-Jetty trend. Only the physical J=1 curve
was supplied.

The existing modern checkpoint `shell-ready-modern-ddr5-jetty47-perjetty-20261010`
was restored once, with ROI synchronization and ArmO3 only inside each
`urma_perftest` measurement. A paired Atomic→ArmO3→Atomic transition without
URMA traffic normalized the checkpoint before the first case. All runs used
official UMDK/UDMA CTP/RM READ, 128 B, one posting process, one 400-Gbit/s UB
port (`-p 0`), one 500-ns-per-pass switch, `-C 524288 -l 256 -Q 1 -I 0`,
and 32768 total operations per case. `-P` changed only the TCP control port.
The modeled CPU is 3.2 GHz ArmO3, not the physical Kunpeng 950 at 2.3 GHz.
Both guests printed identical bandwidth rows and exited zero in every case;
all five ROI fence participants agreed. The driver is
`tools/profile_gem5_read_jettys.py`.

## Results

| Sweep | Jettys | JFS/JFR depth per Jetty | Average MiB/s | Mops/s | Source core cycles/READ |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fixed per-Jetty depth | 1 | 8192 | 1340.75 | 10.983 | 295.6 |
| Fixed per-Jetty depth | 2 | 8192 | 1182.77 | 9.689 | 332.9 |
| Fixed per-Jetty depth | 4 | 8192 | 955.79 | 7.830 | 410.8 |
| Fixed per-Jetty depth, J=1 repeat | 1 | 8192 | 1348.80 | 11.049 | 292.9 |
| Fixed aggregate depth 8192 | 1 | 8192 | 1321.77 | 10.828 | 298.0 |
| Fixed aggregate depth 8192 | 2 | 4096 | 1111.77 | 9.108 | 355.1 |
| Fixed aggregate depth 8192 | 4 | 2048 | 1102.48 | 9.032 | 360.0 |
| Fixed aggregate depth, J=1 repeat | 1 | 8192 | 1339.55 | 10.974 | 294.6 |

The J=1 start/end rates differ by 0.6% and 1.3% in the two sweeps. Increasing
Jettys does not improve READ throughput under either depth convention. The
fixed-aggregate-depth J=4 point is 15% faster than the fixed-per-Jetty-depth
J=4 point, but still about 17% slower than its J=1 baseline.

The depth convention matters because upstream `run_once_bw` in
`sources/umdk/src/urma/tools/urma_perftest/perftest_run_test.c` iterates over
Jettys, filling each JFS window before polling the shared send JFC. At J=4,
8192 iterations per Jetty and 8192 depth, the entire 32768-operation test can
be posted before its first completion-drain phase. This is a burst/drain
measurement, not a steady-state multi-Jetty gain estimate. Holding aggregate
depth at 8192 removes that particular confounder and still shows no gain.

## Counter localization

The READ initiator is node1; its busy ArmO3 core is `switch_cpus3` in these
ROI files. In the fixed-aggregate-depth sweep, J=1→J=2→J=4 committed
353.0→376.4→378.6 instructions per READ and spent
298.0→355.1→360.0 cycles per READ. Rename serializing-stall cycles rose
from 0.309 million at J=1 to 1.706/1.725 million at J=2/J=4. The number
of committed memory barriers was 37767/39414/39426, respectively. This
is strong evidence that additional Jettys increase execution cost in the
single posting/polling core. The provider posts 256 linked WRs per doorbell
with `dmb st`, while every successfully polled CQE executes `dmb ld`
(`udma_u_jfs.c`, `udma_u_jfc.c`). The rise in stall cycles cannot yet be
assigned uniquely to those barriers, a particular provider function, or
cache/LSQ interaction: gem5's aggregate stall categories can overlap.

The source UDMA completed all **262144 READ WQEs** across the eight cases.
Its final profile reports `read_window_stalls=0`, `sq_depth_rejects=0`,
`sq_decode_rejects=0`, `ubase_errors=0`, and `rma_pending_peak=256`. The
64 completion publication slots reached their peak, but CQ wait accumulated
zero modeled time and JFC pending peaked at one; a peak alone does not prove
that the slots limit this rate. The ns3UB path delivered all **524288**
request/response frames with zero drops, retransmissions, congestion cuts,
queue marks, pacing waits, or output backpressure. Its port-0 request and
response byte counts match 262144 operations. The 400-Gbit/s payload ceiling
is about 47684 MiB/s, over 35 times the measured J=1 128-B payload rate;
wire overhead does not close that gap. These counters rule out a sustained
link or configured 256-credit READ-window bottleneck for this workload.

The supplied physical single-Jetty 128-B point is 2819.64 MiB/s and
23.098464 Mops/s. This run's stable simulated J=1 rate is about
10.8–11.0 Mops/s, or **47%** of that operation rate, outside the desired
±20% band. The physical command used a 64-KiB base-page environment and a
size sweep in one process; the guest has 4-KiB pages and these are separate
processes. No physical multi-Jetty READ measurement exists to validate the
J=2/J=4 trend. The present profile narrows the problem to the initiator
execution path, but cannot yet apportion the 2.1× gap between UMDK work,
CPU/cache timing, and device overlap. The next discriminating measurement is
per-phase ROI timing/counters for `urma_post_jetty_send_wr`, JFC polling,
and fence/barrier waits, followed by a single-factor ArmO3 sensitivity run.

## Reproduction and evidence

Run `./lab start-ready --profile modern --provider official
--network-backend modular-ns3ub --roi-sync --ub-port-count 1` followed by
`./lab sync` and the paired no-traffic CPU mode transition before either
sweep. The two driver invocations used `--jettys 1 2 4 1 --total-ops 32768`;
the second added `--aggregate-depth 8192`. Keep the control port unique and
the UB data port at `-p 0`. `./lab stop` flushed final device/network
counters after both sweeps.

- Fixed per-Jetty evidence: `experiments/gem5-read-jetty-profile-20261013/`.
- Fixed aggregate evidence: `experiments/gem5-read-jetty-fixed-depth-20261013/`.
- Each directory contains result CSV, UART transcript, five participant fence
  deltas, per-node ROI stats, and run manifest. The fixed-aggregate directory
  also contains final source/target UDMA and network logs. Driver compilation
  and both full guest sweeps passed.
