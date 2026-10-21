# 64-KiB READ/WRITE versus Jetty count, one 400-Gbit/s port

Date: 2026-10-10. Restored the `shell-ready-modern-ddr5-jetty47-perjetty-20261010`
checkpoint and passed `./lab sync` (63.02 s wall). The ns3ub adapter uses
one native CTP identity per guest-created Jetty. Both hosts use the modern
ArmO3/DDR5 profile, one UB port, a single switch with 500 ns per pass, and
the same 400-Gbit/s link. The workload uses `urma_perftest` one-way CTP RM
READ/WRITE bandwidth, size 65,536 B, 4,096 *total* operations, JFS/JFR
depth 8,192, JFC depth 524,288, post list 256, and Q=1. Only Jetty count
changes; iterations per Jetty are 4,096 or 2,048. Both guests' matching
rows, zero exits, and lifecycle fences were checked by the driver script.

| Operation | Jettys | Average MiB/s | Mpps | Host wall time |
|---|---:|---:|---:|---:|
| READ | 1 | 46,714.22 | 0.747427 | 135.43 s |
| READ | 2 | 47,183.27 | 0.754932 | 175.43 s |
| WRITE | 1 | 46,976.50 | 0.751624 | 170.27 s |
| WRITE | 2 | 46,968.62 | 0.751498 | 181.40 s |

The 400-Gbit/s port corresponds to 47,683.72 MiB/s before protocol
overheads. The J=1 READ and WRITE rows reach 98.0% and 98.5% of that
ceiling. Raising Jettys from one to two changes READ by +1.0% and WRITE
by -0.02%. Thus this large-message, single-port configuration is already
line-rate limited with one Jetty, and extra Jettys do not significantly
increase aggregate bandwidth. The supplied physical 64-KiB one-Jetty READ
reference is 45,588.18 MiB/s; the J=1 gem5 result is +2.47% higher, but
the physical run used 16,384 operations whereas this trend run used 4,096.

Raw CSV, UART transcripts, lifecycle/fence logs and the full machine manifest
are under `experiments/gem5-jetty47-64k-sweep-20261010/`. The original
driver requested J=4 and a J=1 repeat as well. Those optional cases were
stopped after the four completed J=1/2 cases established that both
operations were within 2% of the port ceiling; partial J=4 files are not
measurements. The measured cases used 662.52 s of host time, and the
driver was terminated after 712.46 s overall. The simulators were stopped.

This result does not prove that one Jetty always suffices. With a smaller
per-Jetty issue window or higher path latency, one Jetty may fail to hold
enough bytes in flight to fill the port. In that case, more Jettys can raise
throughput until a shared port, DMA, memory or completion bottleneck is
reached. Here the model exposes 8,192 JFS entries and 256 device READ
issue credits, so one Jetty has ample concurrency for 64-KiB traffic.

The small-message ceiling is a different regime. Prior 128-B READ source
CPU counters showed about 320 modeled cycles per operation with substantial
load-queue and rename pressure, and no fabric pacing or loss. That locates
pressure in per-operation processing, but does not isolate CPU/cache from
the provider, device doorbell, DMA and completion pipeline. The 128-B
physical fidelity gap remains open.
