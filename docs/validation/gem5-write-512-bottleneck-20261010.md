# 512-B WRITE single-port bottleneck diagnosis (2026-10-10)

**Follow-up interpretation (2026-10-10):** The first/repeat difference below
is largely a simulator memory-mode transition. The checkpoint starts in
`atomic_noncaching`, while returning from the first ArmO3 ROI selects the
boot `AtomicSimpleCPU`'s `atomic` mode. A fresh restore followed only by an
Atomic-to-O3-to-Atomic toggle, without URMA traffic, made its first WRITE
reach 4619.17 MiB/s. Therefore the cache/IPC/SQ-full statistics below are
valid observations, but a preceding URMA workload is not required to produce
the high-bandwidth state. See
`docs/validation/gem5-write-512-warmup-20261010.md` for the controlled test.

## Reproduction

The existing `shell-ready-modern-ddr5-multijetty-write` checkpoint was restored
with the same `modern` machine manifest: one physical UB port per endpoint,
400 Gbit/s, one switch at 500 ns per pass, ArmO3 at 3.2 GHz, DDR5-6400,
and the same UDMA queue settings. Both trials used one posting process,
`write_bw -s 512 -J 4 --jettys_pre_jfr 1 -I 0 -T 8192 -R 8192
-C 32768 -n 4096 -l 256 -Q 1 -p 0`, or 16,384 WRITEs per trial.
The setup TCP port changes between successive trials as in the original
sweep; UB data still uses only physical `port=0`. The sole new runtime switch,
`UBSIM_PROFILE_LIFECYCLE_STATS=1`, resets and dumps gem5 statistics at the
existing synchronized ROI boundaries. UDMA virtual-time stage counters also
observe this path without adding simulated latency or changing capacities.

Raw files are under
`experiments/gem5-write-512-bottleneck-stats-20261010/`:
`cases/results.csv`, both UART transcripts, `run-dual/node1/stats.txt`,
`run-dual/udma-node{0,1}/udma.log`, and the fabric log. Both command pairs
exited zero, all four ROI lifecycle generations completed, and all simulators
were stopped. A preceding independent J=4/J=4 diagnostic run measured
2,278.30/4,603.01 MiB/s, confirming the large first-run effect.

| Same J=4 workload | First after restore | Immediate repeat |
| --- | ---: | ---: |
| Payload throughput | 2,296.52 MiB/s | 4,779.72 MiB/s |
| Message rate | 4.703270 Mpps | 9.788867 Mpps |
| Main ArmO3 core | CPU 2 | CPU 1 |
| Main core cycles / WRITE | 690 | 333 |
| Committed instructions / WRITE | 423 | 422 |
| Main core IPC | 0.613 | 1.268 |
| O3 rename `SQFullEvents` | 5,905,966 | 987,451 |
| Main core L1D average miss latency | 76.5 ns | 17.6 ns |
| Shared L3 misses / accesses | 54,538 / 55,160 (98.9%) | 18,408 / 35,866 (51.3%) |

The benchmark thread moved from CPU 2 to CPU 1 between trials. Both cores
have the same modeled parameters; core migration is a confounder for the
first-versus-second attribution. The shared L3 was much warmer on the second
trial, while the per-WRITE instruction count stayed almost constant. The
large reduction in L1D miss service time and SQ-full events tracks the IPC
increase and roughly doubled throughput. `SQFullEvents` counts O3 rename
stall events, not completed WQEs or an independently measured fraction of
runtime. The modeled O3 store queue has 32 entries. The evidence supports
guest memory/store-queue pressure as the leading **finite-resource** limit,
especially in the cold trial; it does not prove that all of the warmed
trial's residual cycles are caused by the store queue.

## Controlled SQ prefetch correction

The original device performed 15,340 then 15,305 SQ prefetch DMA requests
for 16,384 WRITEs per trial, or about 1.07 WQEBBs per request despite a
16-WQEBB burst setting. `FillSqCache` refilled as soon as a 64-entry cache
freed *one* slot, turning most subsequent reads into 64-byte transactions.
It now waits for burst-sized free space while the needed head is cached, and
still fetches a short tail when fewer than one burst remains. A descriptor
pipeline regression covers this behavior. The model build passed all 12
device tests.

The same checkpoint, one-port manifest, J=4 workload, two-run order and
O3 ROI stats were repeated after this code correction. The only changed
simulator behavior was SQ prefetch batching:

| 16,384-WRITE trial | Original first | Fixed first | Original repeat | Fixed repeat |
| --- | ---: | ---: | ---: | ---: |
| SQ prefetch DMA requests | 15,340 | 1,024 | 15,305 | 1,024 |
| Throughput (MiB/s) | 2,296.52 | 2,267.55 | 4,779.72 | 4,744.24 |
| Message rate (Mpps) | 4.703 | 4.644 | 9.789 | 9.716 |

The DMA transaction count fell about 15-fold, but throughput changed less
than 1.3%. The descriptor prefetch inefficiency was real but **not** the
binding throughput resource. In the fixed runs, the posting thread stayed on
CPU 2 both times; the cold-to-warm doubling remained. L3 misses fell from
55,281 to 18,134, IPC rose from 0.604 to 1.261, and SQ-full rename events
fell from 5.98M to 1.02M. This repeat removes CPU migration as the primary
explanation for the cold-to-warm effect. The fixed run logs are under
`experiments/gem5-write-512-prefetch-fix-20261010/`.

## Device and network exclusion checks

The per-ROI UDMA snapshots are cumulative. The following service times are
the first snapshot and the difference between the second and first snapshots,
divided by their completed stage counts. Overlapping stages must not be added
to obtain a per-WRITE critical path.

| UDMA virtual-time stage | First mean | Repeat mean |
| --- | ---: | ---: |
| SQ prefetch DMA (about 15.3k bursts/run) | 301 ns | 301 ns |
| Source 512-B payload DMA | 211 ns | 211 ns |
| Target 512-B write DMA | 212 ns | 213 ns |
| Request send to full ACK | 1,701 ns | 1,695 ns |
| JFC queue wait | 0 ns | 0 ns |
| CQE publication | 321 ns | 450 ns |

The CQ publication slot peak was 27 of 64, and the pending JFC queue peak
was one. Source pending WRITE requests peaked at 39, with no READ window
stalls, SQ depth rejects, UBASE errors or notification failures. The SQ
prefetch peak reached its configured four per-Jetty requests at least once,
but this counter alone cannot establish sustained prefetch saturation. The
fabric delivered all 65,536 frames (32,768 requests and 32,768 ACKs), with
zero drops, retransmissions, congestion cuts, queue marks and wire pacing
waits. The physical 400-Gbit/s link is therefore not saturated in this test.

The nearly unchanged payload, target and ACK service times cannot explain
the first-to-second throughput doubling. CQ publication became *slower* on
the repeat while throughput rose. That comparison supports the source guest
execution path as the main cause of the observed plateau, rather than a
device completion or fabric capacity at these settings.

## Capacity implication and remaining uncertainty

At 512 B, 400 Gbit/s would require at least 97.66 million WRITEs/s before
protocol overhead. The repeat achieves 9.79 million/s. One ArmO3 core retires
about 422 instructions per WRITE. Even at the configured 8-wide maximum
commit rate and 3.2 GHz, that instruction count gives an idealized one-core
upper bound of about 60.7 Mpps (31.1 decimal GB/s payload), before stalls,
packet headers, DMA or ACKs. Current one-thread posting cannot reach a
50-GB/s payload rate by increasing Jetty count alone. This bound applies to
the **current guest/provider path and measured instruction count**; it is not
a claim about actual hardware or a future more efficient posting path.

To distinguish the warmed trial's remaining O3 store-queue, cache and
perftest polling costs without changing traffic parameters, the next useful
instrumentation is per-thread instruction/commit stall attribution around
`urma_post_jetty_send_wr` and `urma_poll_jfc`, plus a sustained occupancy
histogram for the 32-entry O3 store queue and the four-slot per-Jetty SQ
prefetch engine. A physical 512-B WRITE curve is still needed for the user's
80% accuracy criterion; the supplied physical curve is READ.
