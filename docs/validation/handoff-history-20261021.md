# New-session handoff — 2026-10-09

This is a historical handoff. For current host-only build and run commands,
see [build-and-run.md](../build-and-run.md). The project Docker runtime and its
container were removed on 2026-10-20.

## 2026-10-14 128B READ 包率定位与修复

正确 DMB LD 解码器下，官方 provider 的单端口、单 Jetty、128B READ 预热包率约 16.5–17.0 Mops/s；实机为 23.098464。设备 READ 窗口、队列、网络 pacing/丢包/重传未触顶。发起端 CQE/SQE 缓存访问和 ArmO3 的 LQ/SQ 资源背压是主要可修复差距。独立优化 provider 对 CQE 提前 4 项读预取、SQE 提前 8 项写预取，只将两个热路径对象编译为 `-O3`，保留所有屏障；`./lab build optimized-initramfs` 产出独立镜像。正式 CPU ROI 配置为 LQ64/SQ64、IQ64、`stride_exclusive`，从 `shell-ready-modern-ddr5-optimized-lq64-sq64-20261014` 恢复。正式恢复首跑 11.725，后续三跑 19.037/19.679/18.595 Mops/s，均超过实机 80%。2/32/128/512/2048/4096B 六点带宽与实机分别为 95.5/82.2/81.8/84.7/86.0/103.4%，呈现小包包率平台与 4KiB 链路线速转折。随后将端点到交换芯片的每段传播延迟从 100ns 调为 375ns，交换每次 500ns 不变；128B READ 延迟从 2.29µs 到 3.43µs，实机 3.38µs；128B 三次带宽均值 18.938 Mops/s，实机的 82.0%。详见 `docs/validation/gem5-read-128b-bottleneck-20261014.md` 与 `docs/validation/gem5-read-network-delay-calibration-20261015.md`。一次早期 DMB 生成 decoder 复用导致的旧实验无效，详情见 `docs/validation/gem5-read-prefetch-diagnosis-20261014.md`。

2026-10-15 补测：新 `shell-ready-modern-ddr5-optimized-lq64-sq64-link375-switch500-20261015` checkpoint 的单端口 Read 带宽，2/32/128/512/2048/4096/8192B 分别为实机 90.7/79.2/75.0/85.9/83.7/103.3/103.5%；128B 低跑次后立即重复三次为 19.295/19.280/20.120 Mops/s，均值为实机 84.7%。小包有跑次波动，4–8KiB 带宽略高约 3%。本轮没有重新测 16KiB 以上，详情见 `docs/validation/gem5-read-network-delay-calibration-20261015.md`。

## 2026-10-14 official provider inner profile

After correcting the perftest peak-timing confound, an isolated instrumented
`liburma-udma.so` was loaded only by the 128-B READ initiator from a private
guest library directory. The stock J=1, `-n 16384` A/B/A rates in one
checkpointed session were 9.786/10.211/10.068 Mops/s. A batch-level provider
probe showed approximately 574/596 µs of post API time in per-WQE encoding,
and 848/879 µs of poll API time in per-CQE scanning. SQ doorbell took ~18 µs
per 64 posts, CQ consumer update ~5 µs per 1024 polls, and the post `dmb st`
<1 µs total. Sampled CQE owner/address lookup was more expensive than `dmb ld`
or CQE parsing; sampled WQE `udma_set_sqe` exceeded input checks and queue
bookkeeping. The probes perturb rate by ~9–16%, and `cntvct_el0` has only
25.17-MHz resolution, so use phase ranking, not sampled nanoseconds, for
calibration. All 131072 successful READ WQEs completed with no window stalls,
depth rejects, pacing, drops or retransmissions. Detailed evidence:
`docs/validation/gem5-read-provider-inner-profile-20261014.md`.
The lab is stopped. The next unproven link is whether CQE owner loads miss in
the CPU cache because of DMA coherence, and what dominates SQE preparation.
An additional same-CQE immediate-relookup probe then ran from the same
checkpoint: stock/probe/stock 9.844/8.665/10.058 Mops/s. For 129 sampled
CQEs, first owner/address lookup totaled 126 guest timer ticks, immediate
second lookup 13 ticks, timer-pair baseline 5 ticks; Arm assembly confirms
two separate owner loads. This supports a cold cache/coherence-path cost for
the first load, but does not identify the exact cache level or DMA mechanism.
Evidence is in the relookup section of the same validation report and
`experiments/gem5-provider-cqe-relookup-20261014/`. This second run is stopped.

## 2026-10-14 128-B READ loop profile: corrected Jetty comparison

**Do not interpret the earlier fixed-32768-operation J=1/2/4 drop as a
Jetty resource bottleneck.** `urma_perftest` automatically enables `no_peak`
when per-Jetty `iters > 20000`: J=1 used 32768 and skipped per-operation
timestamps, whereas J=2/4 used 16384/8192 and collected them. Statement-level
instrumentation showed the CQE bookkeeping jump from about 80 to 480 µs was
the per-CQE timestamp path. Explicit `-N` gave J=1 10.56/10.69, J=2 10.34,
J=4 10.01 Mops/s with the same 32768 total operations. With 16384 total
operations (all peak-timed, matching the user's physical `-n 16384`), J=1
was 9.79/9.85, J=2 9.40, J=4 9.64 Mops/s. An uninstrumented J=1 run was
9.606 Mops/s versus physical 23.098 Mops/s, or 41.6%. The 400-Gbit/s fabric
reported no READ window stalls, drops, pacing or retransmissions. The remaining
absolute gap is mainly inside the measured post/poll API boundaries, though
those boundaries also include device/completion waits. Details, raw CSV/UART,
and an isolated diagnostic build are in
`docs/validation/gem5-read-loop-profile-20261014.md`. The lab is stopped.

## 2026-10-12 gem5 SE assessment

The pinned ARM gem5 successfully ran a static AArch64 Hello in generic SE
with `argv[1]=probe`; its config recorded `full_system=false` and output
included console text plus `stats.txt`. The lab has no integrated URMA SE
path: the only gem5 configuration is `configs/arm64_fs.py` with `ArmFsLinux`
and a platform UB-HOST adapter; official UMDK needs `/sys/class/ubcore`,
`/dev/uburma`, ioctl and mmap doorbells supplied by its OLK driver.
`docs/validation/gem5-se-assessment-20261012.md` separates currently usable
CPU-only SE input/output from the substantial device/provider bridge needed
for official two-node URMA SE. Smoke artifacts are under
`experiments/gem5-se-aarch64-smoke-20261012/`. No FS simulation was started.

## 2026-10-12 CTP MTU and small READ latency

Current ns3ub CTP treats 4096 B as a maximum segment payload, with exact
READ response bytes and only 0–3 B of CTP tail alignment; the native
17-length contract passed. The physical active MTU remains unknown. The
user's 128-B READ 23.098 Mpps on one 400-Gbit/s port independently excludes
4-KiB padding of every response, which would require 756.9 Gbit/s. A
checkpointed one-port, one-Jetty `read_lat` sweep measured gem5 means at
2/128/1024/4096/2-repeat B of 2.51/2.41/2.47/2.68/2.42 µs (4090
samples per run). Physical means at the unique sizes are 3.38/3.38/3.54/
3.75 µs. The qualitative flat-then-rising shape is right, but gem5 mean
latency is 26–30% too short, and its rise with size is too small. The five
runs used 934.62 s of host wall; all simulators were stopped. Raw evidence,
parameter differences and source references:
`docs/validation/gem5-read-lat-mtu-20261012.md`.

## 2026-10-10 large-message multi-Jetty check

Using the current 47-Jetty ready checkpoint, one 400-Gbit/s port and a
single 500-ns-per-pass switch, synchronized 64-KiB READ/WRITE bandwidth
with 4,096 total operations measured J=1/2 READ 46,714/47,183 MiB/s and
J=1/2 WRITE 46,977/46,969 MiB/s. A single Jetty already uses 98.0–98.5%
of the theoretical 47,684-MiB/s line-rate ceiling; J=2 changes READ by
+1.0% and WRITE by -0.02%. J=4 and repeat controls were stopped before
completion because the measured configurations had established the link
ceiling. Raw results and qualification are in
`docs/validation/gem5-large-jetty-bandwidth-20261010.md`.

The 128-B plateau is per-operation processing limited, with substantial
modeled CPU load-queue and rename pressure, but CPU/cache versus provider,
doorbell, DMA and completion contributions are not yet isolated. More Jettys
could help large packets only when one Jetty cannot keep enough bytes in
flight; this 64-KiB configuration already can. All simulators were stopped.

## 2026-10-10 Jetty resource limit and per-Jetty transport repair

The user confirmed a target hardware Jetty upper bound of 47. OLK UDMA
derives the reported `max_jetty` from UBASE standard count plus firmware
public/normal ranges; this pinned `urma_perftest` errors on an explicit
`-J > max_jetty`, rather than silently clamping. The modern simulator now
models 47 total identities as 46 standard + one public (split unverified
on real hardware), and the UDMA model enforces creation capacity. The
ns3ub adapter now binds each guest-created Jetty one-to-one to a native
CTP Jetty, with per-Jetty sequence, ACK, retry and deduplication state.
These are simulator representations of the same Jetty concept, not two
physical resource classes. Linux Docker UDMA 12/12 tests and ns3ub native,
process and loss/replay contracts pass.

The current modern ready checkpoint is
`checkpoints/shell-ready-modern-ddr5-jetty47-perjetty-20261010`. The old
1152-Jetty checkpoints are intentionally incompatible. Both newly booted
guests reported `max_jetty=max_jfs=47`; `-J 48` explicitly failed with
`out of range`, while `-J 47` completed READ and WRITE with one 400-Gbit/s
port. A warmed 128-B READ/WRITE scan at J=1/2/4 measured
10.960/10.930/10.505 and 10.605/10.572/9.886 Mpps respectively at
fixed 4,096 total operations. Cold first READ was 4.217 Mpps. The J=47
boundary check measured 8.029/7.714 Mpps for 12,032 operations; its 256
operations per Jetty do not establish steady-state throughput. All
simulators were stopped. Complete evidence and caveats are in
`docs/validation/jetty-hardware-limit-20261010.md`.

## 2026-10-10 WRITE DMA pipeline model repair and transport audit

The source UDMA's one-in-flight, non-inline WRITE payload fetch was a model
policy, not a requirement in the public URMA API. It is now a configurable
per-Jetty window, default 8, with ordered frame emission and safe pending
WQE completion/error/teardown handling. A same-checkpoint, one-port 128-B
full-system comparison gave J=1 READ/WRITE 9.697/4.856 Mpps at window 1
and 9.629/9.691 Mpps at window 8. The WRITE source DMA peak concurrency
changed from 1 to 8; all 16,384 WQEs per case completed and fabric error,
drop and pacing counters stayed zero. At window 8, J=4 READ/WRITE measured
9.828/9.484 Mpps. The 8-slot setting is an uncalibrated sensitivity value;
physical WRITE measurements are unavailable. The physical 128-B READ is
23.098 Mpps, so the read-side fidelity gap remains. Full source evidence,
model audit, commands and raw paths are in
`docs/validation/gem5-write-payload-pipeline-20261010.md`,
`docs/validation/urma-rma-public-evidence-20261010.md` and
`docs/validation/ns3ub-rma-model-audit-20261010.md`. Both experiment
simulator pairs were stopped.

The ns3ub adapter currently maps multiple guest Jettys to native Jetty 1.
The native transport's per-Jetty TASSN allocator conflicts with Entity-wide
admission and replay identity if guest Jettys are mapped directly to
distinct native Jettys; an opt-in two-Jetty counterexample was added under
`integrations/ns3ub/tests/`. Quantitative multi-Jetty runs still use a
shared native transport Jetty. Repair requires a coordinated sequence,
ACK/replay and lifecycle design; no unsafe partial mapping was enabled.

## 2026-10-10 128-B READ/WRITE difference located

Paired one-port, one-Jetty, 16,384-operation warmed gem5 runs with `-I 0`
measured READ 9.861916 Mpps and WRITE 4.852785 Mpps. The WRITE source
payload DMA averaged 204.69 ns and had peak concurrency one; its reciprocal
4.885 Mpps nearly equals the observed WRITE rate. Code holds each Jetty's
`SqPipeline::fetching` until a non-inline WRITE's source DMA callback and frame
send, while READ releases it directly after sending the request. Earlier
J=2/4 WRITE gains correlate with source DMA concurrency rising to 2/4.
This is a model-fidelity bottleneck, not proven physical URMA behavior or a
wire-rate ceiling. The paired O3 statistics and an incomplete inline probe
are documented in `docs/validation/gem5-read-write-asymmetry-20261010.md`.
All simulator processes are stopped. Physical WRITE results are unavailable.

## 2026-10-10 one-port 128-B multi-Jetty READ/WRITE comparison

From the then-current modern ready checkpoint, a no-traffic CPU-mode toggle
preceded an interleaved 128-B READ/WRITE scan at J=1/2/4/8/16 plus J=1
repeats. The one 400-Gbit/s port, 500-ns switch pass, 16,384 *total*
operations, `-T/-R 8192 -C 524288 -l 256 -Q 1` stayed fixed. READ Mpps
were 9.765/10.080/9.373/8.818/8.569; J=1 repeat was 10.181. WRITE Mpps
were 4.853/7.979/9.467/8.759/8.662; J=1 repeat was 4.846. READ does not
benefit materially from more Jettys and declines at J=4–16; WRITE nearly
doubles by J=4 and then declines. The supplied one-Jetty physical 128-B
READ is 23.098 Mpps; even the best simulated multi-Jetty READ is 56.4% low
against that reference, though J=2 is not a matched physical comparison.
All 12 command pairs and lifecycle fences passed. Source UDMA completed all
196,608 WQEs,
with zero READ-window stalls, CQ wait, or errors; fabric delivered all
393,216 frames without congestion or pacing. The guest perftest uses one
posting thread and one shared JFC across Jettys. Its effective per-Jetty
depth clips to iterations/J, a caveat at J≥4. Full method, raw artifacts,
and CSV are in `docs/validation/gem5-rma-128-jetty-compare-20261010.md`.
All custom run-root simulators are stopped.

## 2026-10-10 physical target k1 inventory

The user supplied passive hardware data from k1 (141.61.80.71), the original
READ target: Kunpeng 950 (7592C), 2 sockets × 96 cores × SMT2, cpufreq policy
maximum 2.3 GHz, 4 NUMA nodes (distances 10/15/20), and 24 × 96-GB
DDR5-6400 DIMMs. UB controllers are non-PCI devices. The gem5 ROI instead
uses 4 ArmO3 cores at 3.2 GHz; its ~10-Mpps small-READ plateau costs
~320 modeled cycles/operation, while the physical ~23-Mpps rate equals
~100 periods at 2.3 GHz. The latter is not a measurement of actual
initiator cycles. Lowering gem5 frequency cannot explain the shortfall.
Gem5 kernel config confirms 4-KiB base pages, while the physical command
specified 64 KiB and k1 `getconf PAGESIZE` returned 65536 bytes, confirming
the target's 64-KiB kernel base page.
First 2-B ROI had 6,095 DTLB misses and 573 completed data walks across
16,384 READs; translation has not been isolated as the cause. The user
subsequently confirmed the original READ initiator and target had the same
hardware specification as k1. A second CPU inventory is unnecessary; runtime
CPU affinity/NUMA placement remains unknown. No new physical URMA run is
possible.
Full comparison is in `docs/validation/gem5-read-small-physical-20261010.md`.

A 128-B gem5 sensitivity run restored the same ready checkpoint and changed
only the device READ issue ceiling from 256 to 512, keeping one port/Jetty
and all guest workload settings fixed. It measured 9.565851 Mpps against
9.737315 Mpps in the earlier normalized sweep (-1.76%); physical is
23.098464 Mpps. At ceiling 512 the source peaked at 256 outstanding READs,
had zero read-window stalls and zero CQ queue wait, and retired all 16,384
WQEs. The fabric had zero drops/pacing. The small difference is subject to
run-order effects, but no large gain appeared, weakening the device-credit
limit hypothesis for 128 B. Raw artifacts:
`experiments/gem5-read-window512-20261010/`. The new gem5 stats files were
empty after stop; use the earlier ROI stats for CPU evidence. All custom
run-root simulators are stopped.

## 2026-10-10 normalized READ small-message comparison

After restoring the then-current modern checkpoint, both guests performed a
no-traffic Atomic-to-O3-to-Atomic toggle to normalize the memory mode. A
single-port, single-Jetty, 16,384-iteration READ sweep at 2/64/128/512/1024/
2048/4096 B measured 9.848/10.056/9.737/9.887/10.207/10.164/10.018 Mpps.
The 2-B repeat was 9.870 Mpps. The supplied physical curve is about
20–23 Mpps through 2048 B, so 2–2048 B miss the ±20% goal by 50–58%; 4 KiB
measured 39,134.34 MiB/s against 45,172.39 physical (-13.4%) and passes.
The source O3 thread commits about 377 instructions and takes 320–338 cycles
per READ across sizes. Load-queue-full events are frequent; device completion
slots touched 64/64 and READ credits touched 256, so CPU versus device stall
shares are not yet isolated. The fabric had no pacing/congestion/drops.
Full results, raw logs, and the physical-host information request are in
`docs/validation/gem5-read-small-physical-20261010.md` and
`experiments/gem5-read-small-warm-mode-20261010/`. No new physical URMA run
is available; the user can provide passive CPU/NUMA/DDR inventory from one
target machine. All simulators are stopped.

## 2026-10-10 checkpoint warm-up control

Three identical 512-B, J=4 WRITE trials after one restore measured
2259.23/4666.93/4665.31 MiB/s. The second and third differ by 0.035%.
An independent cold restore with perftest `-w` still measured 2273.98 MiB/s:
with `-l 256`, that pre-ROI warm-up sends only one WQE per Jetty while the
guests are on fast Atomic CPUs. In a third restore, a first 512-B READ
followed by WRITE gave 2318.68 then 4591.57 MiB/s. A stronger fourth
restore performed only an Atomic-to-O3-to-Atomic CPU-mode toggle, with no
URMA traffic, before the first WRITE; that WRITE reached 4619.17 MiB/s.
The source explains this: the checkpoint starts in `atomic_noncaching`, but
returning from O3 selects the boot CPU's required `atomic` mode, so following
benchmark setup can use caches. A preceding URMA run is not required for the
large throughput gain. Full methods and raw-log links are in
`docs/validation/gem5-write-512-warmup-20261010.md`. All simulator processes
are stopped.

## 2026-10-10 single-port 512-B WRITE bottleneck diagnosis

Two identical J=4, 16,384-WRITE, one-port 400-Gbit/s runs from the current
checkpoint measured 2,296.52 then 4,779.72 MiB/s. O3 lifecycle ROI stats
and UDMA virtual-time stages are in
`docs/validation/gem5-write-512-bottleneck-20261010.md`, with raw logs under
`experiments/gem5-write-512-bottleneck-stats-20261010/`. The source's main
O3 core committed about 422 instructions per WRITE in both runs, but IPC
rose 0.61 to 1.27; shared L3 misses fell 54,538 to 18,408 and O3 rename
SQ-full events fell 5.91M to 0.99M. Device SQ/payload/target/ACK service
times were stable; CQ queue wait was zero and publication slots peaked at
27/64. No fabric congestion occurred. The leading finite resource is the
single guest posting thread's O3 memory/store-queue path, with a strong
cold-to-warm cache effect; the exact residual warmed stall split is not yet
causally isolated. The benchmark migrated between modeled cores across runs.
All simulators are stopped. Device build and 12/12 tests passed.
An SQ prefetch batching fix reduced descriptor DMA requests from about
15,300 to 1,024 per 16,384-WRITE trial. Matched J=4 first/repeat throughput
remained about 2,268/4,744 MiB/s (versus 2,297/4,780 before), proving
descriptor DMA transaction count was not the binding resource. Both fixed
trials ran on O3 CPU 2; the cold/warm L3 and SQ-full pattern persisted.
The fixed-run evidence is under `experiments/gem5-write-512-prefetch-fix-20261010/`.

## 2026-10-10 multi-Jetty WRITE update

The modern ready checkpoint at this stage was
`checkpoints/shell-ready-modern-ddr5-multijetty-write`. It contains a rebuilt
`urma_perftest` whose synchronized one-direction READ/WRITE bandwidth path
accepts multiple Jettys. The latency and SEND-bandwidth synchronized paths
remain single-Jetty. `start-ready` checks the current initramfs SHA-256 against
modern checkpoint metadata, so it will not silently restore an older guest
program after rebuilding the image. One cold boot established the new
checkpoint; it saved in 101 s. Both guests have the same new perftest hash.

The completed 512-B single-direction WRITE scans used fixed 16,384 total
operations, one 400-Gbit/s port and one 500-ns-per-pass switch. At per-Jetty
JFS/JFR depth 512, J=1/2/4/8/16 measured 2,072/4,074/4,239/4,352/4,360
MiB/s, with warmed J=1/2 repeats 2,303/4,185 MiB/s. At depth 8192, the
same Jetty counts measured 1,904/3,800/4,690/4,586/4,178 MiB/s, with a
warmed J=1 repeat of 2,302 MiB/s. J=4 is the high-depth best (9.605 Mpps),
about 2.04 times warmed J=1; more Jettys do not keep raising throughput.
All 13 command pairs and five-participant lifecycle fences passed. The
shared JFC was sized to 32,768 to cover the aggregate Jetty post window.
Complete method, caveats, CSV and raw logs are in
`docs/validation/gem5-write-512-multijetty-20261010.md`. Simulator processes
are stopped. No physical WRITE curve has been supplied, so this is a scaling
result rather than an accuracy comparison.

## Current modern READ state

The `modern` default uses the 2-MiB IOCache and coordinated checkpoint
`checkpoints/shell-ready-modern-ddr5-multijetty-write`. It retains 3.2-GHz ArmO3
for the ROI, DDR5-6400, one 400-Gbit/s port per node, a single switch with
**500 ns per pass**, guest JFS/JFR/JFC limits 8192/8192/524288 and 256
internal READ issue credits. `./lab start-ready --profile modern --provider
official --network-backend modular-ns3ub --roi-sync --ub-port-count 1`
selects the new checkpoint. A single cold boot established that checkpoint;
subsequent calibration runs used restores. The earlier 1-KiB checkpoint is
retained for comparison.

In a matched-order, 16,384-iteration `read_bw` sequence (2 B, 4 KiB, 2 B),
the 2-MiB profile measured 3.980 Mpps, 38,409.19 MiB/s and 9.596 Mpps.
Further warmed repeats measured 9.841 Mpps at 2 B and 38,534.17 MiB/s at
4 KiB. Physical references are 19.882 Mpps and 45,172.39 MiB/s: warmed
4-KiB bandwidth is 14.69% low and within ±20%; warmed 2-B message rate is
50.50% low. A 1,024-iteration 64-KiB check reached 46,738.62 MiB/s versus
45,588.18 physical. A full 16,384-iteration 128-B `read_lat` run reported
mean/p99/p99.9/p99.99/max of 2.30/2.54/4.33/17.21/32.98 us, versus physical
3.38/3.52/3.64/6.72/9.33 us. Thus mean/p99 latency and rare tail do not
meet the target; the earlier 1-KiB full run had the same mean/p99. Both
endpoints exited zero. Raw rows, transcripts and copied component logs are
linked from `docs/validation/physical-read-calibration-20261009.md`.

The simulator processes were stopped after the full latency run. For the next
calibration step, measure the per-operation guest posting, CQ polling, DMA and
target service path. Full 26-size and repeated-run latency coverage is not
yet complete. The pinned guest perftest lacks `--page_size 65536`, so physical
bandwidth parameters are not exactly identical.

## Earlier READ repair and 1-KiB measured limits

The source includes pipelined SQ/JFC completion DMA with ordered CQ
slot reservation, an independent modern-profile READ issue limit of 256 and
a gem5 `System::Threads` external-fence activation patch. The latter defers
timer/interrupt wakeups until COMMIT; an earlier attempt without it allowed a
CPU to execute 2,650 instructions during the fence. The patch lives at
`patches/source/gem5/0002-Defer-thread-activation-during-external-fences.patch`
and `scripts/build/build_gem5.sh` applies/verifies it against the pinned
gem5 checkout. The latest successful gem5 build was performed in Docker.
Native UDMA model tests passed 9/9; the Linux device build passed 12/12;
CLI tests passed 15/15 and shell syntax checks passed.

The modern profile advertises JFS/JFR/JFC limits
8192/8192/524288, ArmO3 3.2 GHz, DDR5-6400, one 400-Gbit/s port and
one switch with **500 ns per pass**. The earlier checkpoint
`shell-ready-modern-ddr5-unidir-1port` restored with the new code; no cold
boot was needed. Full 16,384-iteration 4 KiB READ bandwidth was
26,045.68 MiB/s at 256 device READ credits, against 45,172.39 physical.
64/128/512 credits each gave roughly 13,000 MiB/s. Full 2 B READ was
6.641 Mpps against 19.882 physical. A 1,024-iteration 64 KiB check reached
46,734.55 MiB/s against 45,588.18 physical. The full 128 B latency test
reported 16,378 samples, mean 2.30 us, p99 2.54 us, p99.9 3.50 us,
p99.99 14.50 us; physical values are 3.38/3.52/3.64/6.72 us. The simulator
passed six command pairs and 12 lifecycle generations without a fence error.
All simulator processes were stopped after the sensitivity sweep.

The complete result tables, limitations and next calibration steps are in
`docs/validation/physical-read-calibration-20261009.md`; raw UART transcripts
and copied component logs are under
`experiments/gem5-modern-unidir-read-20261009/`. The accuracy target is met
for the large-message bandwidth plateau, not for small/medium bandwidth or
128 B mean latency. The supplied physical latency and bandwidth curves are
in `results/physical-read-lat-20261009.csv` and
`results/physical-read-bw-20261009.csv`.

## Earlier one-shot JFC correction

A later correction made JFC notifications one-shot in the modern profile.
The polling-only 1,024-iteration 2 B and 4 KiB READ cases then measured
1.887 and 2.021 Mpps; source UDMA completed 2,048 WQEs with zero CEQ events.
The performance gap remains. SQ retirement and per-JFC CQ publication are
serialized; the latter performs a consumer-index DMA read and CQE DMA write
for every completion. These are the next concrete bottleneck candidates.
See the validation document and
`results/gem5-modern-read-oneshot-1024-20261009.csv`. Model state v4 reads
the v3 ready checkpoint. The rerun environment was stopped afterward.

The user authorized further repair and simulation after the stopped-environment
handoff below. The historical evidence is in
`docs/validation/physical-read-calibration-20261009.md`. This earlier modern
configuration used ArmO3 only in the synchronized ROI, AtomicSimpleCPU with
`atomic_noncaching` for boot/idle, DDR5-6400, one 400-Gbit/s port and modeled
JFS/JFR/JFC limits 8192/8192/524288. Its ready coordinated checkpoint was
`checkpoints/shell-ready-modern-ddr5-unidir-1port` (profile revision v4).
The modern default at this stage selected the newer 2-MiB checkpoint described at
the top of this file. A v3 modern checkpoint and the two fast-profile
checkpoints below remain for historical comparison.

The v4 guest image contains the single-direction RMA bandwidth sync patch
`patches/source/umdk/0012-synchronize-unidirectional-rma-bandwidth.patch`.
`scripts/build/build_umdk.sh` applies it from the pinned source bundle. Use
`BUILD_STOCK_UDMA=enable ./lab build umdk` before `./lab build initramfs` when
rebuilding the official guest image. The UMDK source checkout currently has
the expected one-file timing patch applied. This build has been validated and
the provider subtree remains at its pinned source.

Modern gem5 `read_bw` at 16,384 iterations measured 1.836 Mpps / 3.50 MiB/s
for 2 B and 2.361 Mpps / 9,220.84 MiB/s for 4 KiB. The 1,024-iteration
64-KiB point measured 46,691.19 MiB/s. The short 128-B `read_lat` run averaged
2.57 us over 122 post-warm-up samples. The large-message bandwidth agrees
with the supplied physical data, but small/medium messages do not meet the
provisional ±20% gate. Full logs are under
`experiments/gem5-modern-unidir-read-20261009/`. All simulators were stopped
after the tests.

The sections below describe the earlier stopped-environment handoff and its
then-current checkpoints and unresolved issues.

Read `AGENTS.md` first. The user requested disk cleanup and a stopped environment
to continue in a new conversation. Do not restart experiments automatically.

## Code and runtime

- Repository: `/Users/caobo/workspace/openurma-gem5-lab`.
- Branch: `feature/ns3ub-network-backend`.
- Latest implementation commit: `63b0d0e`; previous functional gate: `3d2ca71`.
- Code is locally committed; these changes were NOT pushed. Do not push without
  authorization. Cleanup did not revert code or reset the branch.
- Current commands run directly on the host. The former Docker container and
  setup scripts have been removed; this handoff's older launch notes below are
  retained only as historical context.
- No host or container simulator processes remained before cleanup.
- Existing `ubsim` heartbeat automation was already PAUSED; keep it paused.
- Official drivers, sources, current binaries, guest image, reports and PPTs
  are preserved in the host workspace.

## Retained ready checkpoints

Only two current state-format-v3 checkpoints remain (~191MiB combined):

- `shell-ready-functional-v3-2port`: fast/official/modular-ns3ub/no-sync/2ports.
- `shell-ready-timing-v3-1port`: fast/official/modular-ns3ub/ROI-sync/1port.

Functional launch from the macOS repository, when requested:

```sh
./lab start-ready --profile fast --provider official \
  --network-backend modular-ns3ub --no-sync --ub-port-count 2
./lab sync
./lab attach 0
# A separate terminal: ./lab attach 1
```

Use `start-ready`, not routine cold boot. `./lab sync` here initializes OOB and
checks the timer; the no-sync launch remains a functional, not timing, test.

## Accepted evidence and unresolved work

- `docs/validation/gem5-functional-current-20261009.md`:27/27 official perftest
  functional cases from a restored checkpoint; not gem5 byte-integrity proof.
- `docs/validation/qemu-functional-matrix-20261009.md`: QEMU payload/queue/multi
  functional evidence. Full-JFC overflow recovery remains incomplete.
- `docs/validation/gem5-timing-1port-20261009.md`: latest bounded timing attempts.
  No synchronized bandwidth result accepted. Do not report old no-sync BW as
  valid performance or tune constants to match physical hardware.

Timing entry ABI, ns-3 background-event gate, and COMMIT/SYNC epoch handoff were
fixed. SEND now executes8 WQEs per endpoint (network16/16 complete), but the
early finisher blocks inside gem5's synchronous exit-fence wait, stops advancing
the conservative horizon, and prevents the other host's tail DMA from finishing.
Next repair is an event-driven exit drain: pause guest CPU, continue real gem5
device events and finite conservative horizons, then commit/rebase/resume only
when ownership drains. Do NOT grant infinite horizons, synthesize time or skip
pending DMA. Add asymmetric-finish/delayed-DMA evidence before claiming success.

## Cleanup performed

- Removed168 untracked experiment `*.shm` ring files,42.165GiB allocated.
  Kept the logs/JSON/manifests/results; experiments now~122MiB.
- Removed12 explicitly identified inactive container `/tmp` shared-memory
  files (~3.3GiB logical allocation). Kept temporary logs.
- Removed five obsolete checkpoints: `post-rma-verified`,
  `shell-ready-full-ummu`, `shell-ready-rma-cap-fixed`, `shell-ready-v2`,
  `shell-ready-v3` (~497MiB).
- Deleted files were disposable runtime state, not moved to Trash; the old
  snapshots cannot be restored without recreating them. Current ready snapshots
  and committed code remain. Docker disk-image space reclamation may lag deletion.
- Do not blanket-prune Docker: other CANN/HCCL images and volumes are unrelated.
- Future native experiment cleanup should unlink ring files after all peers
  exit; retain diagnostic logs, not shared-memory dumps.

## 2026-10-20 declarative gem5 topology

Both `configs/topology-gem5-two-heads.json` and
`configs/topology-gem5-asymmetric-links.json` now restore compatible modern
checkpoints via `./lab topology start FILE`; the latter checkpoint is
`shell-ready-modern-gem5-asymmetric-400-200-link375-550-20261020`.
The Docker ns-3 adapter and gem5 UB-HOST were rebuilt. The synchronized
5-case SEND/READ/WRITE smoke suites passed for uniform and asymmetric links
in 53.327 and 64.670 seconds respectively. The configured endpoint rates
and delays appeared in the ns-3 log. See
`docs/validation/topology-gem5-20261020.md`. These smoke runs do not establish
physical-machine performance accuracy.
