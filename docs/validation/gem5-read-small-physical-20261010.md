# Small-message READ bandwidth against the supplied physical curve (2026-10-10)

## Matched simulated sweep

The only physical READ measurements available are the user's existing 26-size
`read_bw` results in `results/physical-read-bw-20261009.csv`. No new URMA
measurement was requested from the physical host. The current gem5 checkpoint
`shell-ready-modern-ddr5-multijetty-write` was restored once with one
400-Gbit/s UB port per node and one switch at 500 ns per pass. After `lab
sync`, both guests performed only a paired Atomic-to-ArmO3-to-Atomic toggle.
This normalized the memory mode to the post-switch `atomic` condition before
the first READ without consuming a READ benchmark as a warm-up.

Every READ used the official CTP/RM provider, one Jetty, 16,384 iterations,
`-T 8192 -R 8192 -C 524288 -l 256 -Q 1 -I 0 -p 0`, one posting process,
and synchronized ArmO3 ROI. `-I 0` is the pinned perftest's default for READ.
The test harness changed the TCP *control* port across independent sizes;
all UB data used the one physical port. Both peers exited zero and printed
matching rows for every case. Raw CSV, UART transcripts, component logs,
machine manifest, and per-ROI gem5 stats are under
`experiments/gem5-read-small-warm-mode-20261010/`.

| READ size | Physical MiB/s | gem5 MiB/s | Error | Physical Mpps | gem5 Mpps |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 37.92 | 18.78 | -50.5% | 19.882 | 9.848 |
| 64 B | 1407.54 | 613.79 | -56.4% | 23.061 | 10.056 |
| 128 B | 2819.64 | 1188.64 | -57.8% | 23.098 | 9.737 |
| 512 B | 11230.64 | 4827.74 | -57.0% | 23.000 | 9.887 |
| 1024 B | 22562.06 | 9967.40 | -55.8% | 23.104 | 10.207 |
| 2048 B | 43935.38 | 19851.27 | -54.8% | 22.495 | 10.164 |
| 4096 B | 45172.39 | 39134.34 | -13.4% | 11.564 | 10.018 |

The last repeated 2-B point was 18.82 MiB/s and 9.870 Mpps, 0.2% above
the first 2-B rate, so the approximately 10-Mpps plateau is stable within
this sequence. Only the 4-KiB size meets the provisional ±20% agreement
criterion. The physical 2–2048-B curve is an approximately 20–23-Mpps
operation-rate plateau; the gem5 curve is approximately 10 Mpps. At 4 KiB
the physical point is already near the 400-Gbit/s payload limit, so its
message rate falls and the bandwidth gap narrows.

The earlier matched-profile 128-B `read_lat` mean was 2.30 us against the
physical 3.38 us, while this READ bandwidth run is much *slower* in Mpps.
The small-message throughput shortfall therefore cannot be repaired by
simply increasing the modeled switch delay to match latency; the operation
pipeline and latency distribution need separate calibration.

## What the current counters establish

On the first 2-B source ROI, the posting core (modeled CPU 2) committed
6,182,855 instructions and ran 5,439,617 cycles for 16,384 READs: about
377 instructions and 332 cycles per READ at 3.2 GHz, with IPC 1.137. Across
all seven sizes the same workload costs about 377 instructions and
320–338 cycles per READ. `rename.LQFullEvents` was about 0.9–1.1 million
per ROI, while `rename.IQFullEvents` was about 295,000. Those are stall-event
counts, not a measured fraction of time or proof that increasing LQ entries
alone would double bandwidth. The stable CPU work and operation rate across
sizes point to a per-READ service/processing ceiling rather than a byte-rate
ceiling in the small-message region.

The source device completed all 131,072 READ WQEs over eight cases, with no
SQ depth rejects or UBASE errors. Its READ issue window reached 256 credits
and recorded 18,841 stalls across the sequence. Completion publication
slots reached their configured peak of 64/64, but `cqe_wait` accumulated
zero time in every case and the JFC pending peak was one. A momentary slot
peak does not establish sustained CQ backpressure. The fabric delivered all
262,144 request/response frames with zero drops, retransmissions, congestion
cuts, queue marks, or wire-pacing waits. Thus the 400-Gbit/s link is not the
small-message ceiling. To attribute the remaining roughly 2.3x operation
rate gap, the next simulation work is a phase-level source CPU posting/polling
profile and completion-slot occupancy over time, not an assumption that CPU
frequency alone explains it.

The first 2-B source ROI spent 5,439,617 modeled cycles on its posting core.
Rename recorded 1,775,597 blocked cycles and 829,438 serializing-instruction
stall cycles, plus 1,092,614 LQ-full events. Across all small sizes the
blocked-cycle count stayed about 1.76–1.79 million and the operation rate
stayed near 10 Mpps. These counters locate substantial pressure in the
guest CPU path, but stall categories can overlap and do not yet attribute
time to exact provider or perftest functions. Eliminating LQ-full events
alone would not automatically close a 2.3x throughput gap.

## 128-B device READ-window sensitivity

After the user confirmed the original physical initiator and target had the
same hardware specification, a targeted gem5 test restored the same modern
checkpoint, performed the same no-traffic Atomic-to-O3-to-Atomic toggle,
and changed only the device READ issue ceiling from 256 to 512. CPU, DDR,
single 400-Gbit/s port, single Jetty, 500-ns switch pass, 128-B payload,
16,384 iterations, `-l 256 -Q 1`, and guest JFS/JFR/JFC sizes stayed fixed.
The previous normalized sweep's 128-B result was 9.737315 Mpps / 1,188.64
MiB/s; the new 512-credit run was **9.565851 Mpps / 1,167.71 MiB/s**
(-1.76% relative to that baseline, -58.59% relative to physical 23.098464
Mpps). Both guests reported one Jetty, identical results, and exit status
zero. The comparison uses a fresh restore and one standalone size against
the third size in the earlier sweep, so the small decrease is not a precise
effect estimate. It rules out a large improvement from this one change.

With the ceiling at 512, source device outstanding READs peaked at 256,
`read_window_stalls=0`, and all 16,384 WQEs retired. CQ queue wait remained
zero; 64 publication slots were momentarily used but did not create pending
CQ work. The fabric delivered all 32,768 request/response frames, with zero
drops, retransmissions, queue marks, or pacing waits. This weakens the
hypothesis that the configured 256-credit *device* ceiling is the 128-B
throughput limit in this workload. Raw UART, manifest, component logs, and
gem5 logs are under `experiments/gem5-read-window512-20261010/`. The gem5
`stats.txt` files for this new run were empty after stop, so no new O3 cycle
breakdown is claimed. The custom run-root simulators were stopped.

## Physical-host information that would narrow the model

The user supplied a passive inventory from **k1 (141.61.80.71), the physical
READ target**. It reports a Kunpeng 950 (7592C), two sockets, 96 cores per
socket, SMT2 (384 logical CPUs), and a 2.3-GHz maximum cpufreq policy under
the `performance` governor. There are four NUMA nodes, with distance 10
within a node, 15 between nodes on the same socket, and 20 across sockets.
The machine has 24 × 96-GB DDR5-6400 dual-rank DIMMs (2,304 GB total),
distributed over the two sockets. Its UB controllers appear at
`/sys/devices/ub_bus_controller0/1`, not as PCI Ethernet NICs. The supplied
`lscpu` cache summary aggregates totals and instance counts; it is not enough
to set the gem5 L3 sharing topology or latency confidently.

The current gem5 manifest models four ArmO3 cores at 3.2 GHz, 64-KiB L1I,
64-KiB L1D, private 1-MiB L2, shared 32-MiB L3, 16 DDR5-6400 subchannels,
and a one-port 400-Gbit/s switch path. The 2–2048-B gem5 plateau of roughly
10 Mpps therefore corresponds to about 320 *modeled* core cycles per READ.
The physical 23-Mpps plateau corresponds to about 100 **2.3-GHz clock
periods** per READ. This latter conversion is only a throughput equivalent:
the original initiator's actual clock, instruction count, CPU occupation,
and overlap with the UB controller were not measured. Nevertheless, lowering
the simulated 3.2-GHz core to k1's 2.3 GHz cannot by itself repair a
simulation that is already about 2.3 times slower in operations per second.

The user subsequently confirmed that the original `read_bw` initiator and
target had the same hardware specification as k1. A second machine inventory
is therefore unnecessary for CPU model, nominal frequency, memory generation,
and cache-capacity comparison. This does not establish the original initiator's
runtime clock, process affinity, NUMA allocation, or which UB controller it
used. Historical binding metadata would help if already available, but no new
`urma_perftest` run is required.

The physical command used `--page_size 65536`, which the pinned guest
perftest does not accept. The physical `-a23` size sweep also reuses one
perftest process, whereas these gem5 size points are separate commands.
These remain parameter mismatches even after matching the one port, Jetty,
queue depths, completion moderation, and iteration count.

The gem5 guest kernel bundle confirms `CONFIG_ARM64_PAGE_SHIFT=12` and
`CONFIG_ARM64_4K_PAGES=y`. The user subsequently ran `getconf PAGESIZE` on
k1 and reported **65536 bytes**, confirming the target's 64-KiB kernel base
page. This confirms the OS page-size difference; it does not independently
establish how the physical perftest implements `--page_size`. In the first
2-B simulated ROI, the posting core recorded 6,095 DTLB misses across about
2.85 million DTLB accesses and 573 completed data page walks for 16,384
operations. Page-size mismatch is real, but these
counts do not establish that translation accounts for the roughly 2.3-fold
small-message throughput gap. A controlled sensitivity run and a source
CPU/device phase breakdown are needed before changing the kernel page size.
