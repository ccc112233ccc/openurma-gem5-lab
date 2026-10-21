# READ bandwidth and latency against the supplied physical curves (2026-10-09)

## Reference and acceptance rule

The 26 physical rows are transcribed verbatim in
[`results/physical-read-bw-20261009.csv`](../../results/physical-read-bw-20261009.csv).
The supplied run is single-direction CTP/RM READ, one Jetty and one port.
For this first calibration gate, a common message size passes when both
bandwidth and message rate differ by at most 20% from the physical row. Coverage
of the full 26-size curve is reported separately. The 26 physical READ-latency
rows, including p99, p99.9, p99.99 and maximum, are transcribed in
[`results/physical-read-lat-20261009.csv`](../../results/physical-read-lat-20261009.csv).
They permit a separate latency comparison at matching message sizes and
iteration count.

The physical curve reaches about 23 Mpps below 2 KiB and plateaus near
45,602 MiB/s (47.82 decimal GB/s) from 8 KiB upward. Its 4 KiB result is
45,172.39 MiB/s. In the follow-up the user confirmed one 400-Gbit/s port per
endpoint and two nodes connected through one switch chip. The user clarified
that about 500 ns is the switch delay for one pass. The newer complete physical
`read_lat` dataset supersedes the earlier preliminary 1.27 us figure: mean
latency is 3.38 us at 128 B, 3.75 us at 4 KiB and 5.08 us at 64 KiB. A READ
request and response each cross the switch, so the current 500 ns switch model
contributes about 1 us to a small READ round trip.

## Current-code QEMU trial

Evidence is in `experiments/read-physical-params-3sizes-20261009/` (manifest,
commands, complete UART transcripts, process logs and `report.json`). The run
used the official provider and `urma_perftest`, two QEMU TCG guests, a modular
ns-3 fabric, required synchronization, two modeled 400-Gbit/s ports per guest,
and 1 Cortex-A72 guest CPU each. READ used `-n 16384 -l 256 -Q 1 -J 1` at
2 B, 4 KiB and 64 KiB, without `-B`. Both guests report maximum JFS/JFR depth
1024 and JFC depth 4096, so requested `-R8192 -T8192 -C524288` were capped to
`-R1024 -T1024 -C4096`. The pinned perftest has no `--page_size 65536`.
The given `--retry_num` and `--ack_timeout` are user-TP options that do not set
this CTP path's timing; the modular run has retransmission disabled under its
lossless-lower-layer assumption. These and the guest CPU/topology differences
make this a partial parameter match, not a physical machine replica.

All three perftest command pairs returned zero and printed matching complete
single-direction result rows. Post-run live-work drain was observed. The output
has 49,152 source WQEs and 49,152 fabric completions with zero fabric failures,
retransmissions or runtime drops. The source posted 192 SQ doorbells, exactly
64 batches for each of the three lengths; the UDMA profile reports a pending
RMA peak of 732. Thus the 256-entry posting batch was exercised, but a peak
outstanding count alone does not identify the throughput limiter. The output
is transcribed in
[`results/qemu-read-physical-params-3sizes-20261009.csv`](../../results/qemu-read-physical-params-3sizes-20261009.csv)
and can be compared with `python3 tools/compare_read_curve.py
results/qemu-read-physical-params-3sizes-20261009.csv`.

| Size | Physical average MiB/s | QEMU average MiB/s | Error | Physical Mpps | QEMU Mpps |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 37.92 | 0.94 | 97.52% | 19.882 | 0.493 |
| 4 KiB | 45,172.39 | 1,925.15 | 95.74% | 11.564 | 0.493 |
| 64 KiB | 45,588.18 | 30,801.31 | 32.44% | 0.729 | 0.493 |

Thus 0/3 sampled sizes pass the provisional ±20% gate, with 3/26 physical
sizes covered. QEMU remains near 0.493 Mpps across these sizes. This is direct
evidence of an operation-rate-limited region in the current experiment; it
does not isolate CPU execution, SQ posting, CQ polling, DMA, or fabric latency
as the sole cause. The 64 KiB point is also below the physical plateau.

Wall time was 176.61 s: shell ready at 50.37 s, 2 B command 93.08 s (including
initial setup), 4 KiB 6.57 s, 64 KiB 20.72 s. No processes from this QEMU run
were left active.

## One-port, high-depth QEMU follow-up

`experiments/read-high-depth-single-switch-20261009/` contains the manifest,
exact guest commands, UART output, capability query, component logs, drain
snapshot and `report.json`. This run used one 400-Gbit/s port per guest, two
nodes through one modeled switch, 500 ns switch service delay, 100 ns UB link
delay, 500 ns host link delay and the same 16,384-iteration `read_bw` command
with `-R 8192 -T 8192 -C 524288 -l 256 -Q 1`. The guest capability query
reported `max_jfs_depth=8192`, `max_jfr_depth=8192`, `max_jfc_depth=524288`
and active 400-Gbit/s ports on both ends. The UDMA firmware caps are now
per-run device parameters; the legacy 1024/1024/4096 defaults remain for old
checkpoints. This particular trial preceded that parameterization and used an
equivalent high-capability device binary, as recorded by its executable hash
and the guest query. The guest test still cannot pass the physical command's
`--page_size 65536` option.

All three READ command pairs completed and drained. The additional 128 B,
128-iteration `read_lat` averaged 7.30 us. The newer physical 128 B mean is
3.38 us, but the iteration counts differ. The READ results are in
[`results/qemu-read-high-depth-single-switch-20261009.csv`](../../results/qemu-read-high-depth-single-switch-20261009.csv).

| Size | Physical average MiB/s | QEMU average MiB/s | Error | Physical Mpps | QEMU Mpps |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 37.92 | 0.92 | 97.57% | 19.882 | 0.480 |
| 4 KiB | 45,172.39 | 1,874.72 | 95.85% | 11.564 | 0.480 |
| 64 KiB | 45,588.18 | 29,994.44 | 34.21% | 0.729 | 0.480 |

This still passes 0/3 sampled sizes at ±20%. The matched queue limits and
single-port topology did not remove the operation-rate ceiling. The 187.51 s
wall time includes 44.45 s to shell-ready and 84.15 s for the latency case.

## gem5 repair and modern machine profile

The prior synchronized latency trial stalled when an early finisher entered a
lifecycle fence while the peer still needed a final host DMA event. The adapter
previously descheduled polling and spun on `serviceDevice(UINT64_MAX)`, which
also skipped the normal finite conservative horizons. The repair pauses guest
CPU execution at PREPARE, keeps gem5 device/DMA events running at finite
horizons, exits the sim loop on the real COMMIT, then resumes the CPUs. It
checks that every guest CPU's instruction count remains unchanged during the
fence. The synchronized 1-CPU timing checkpoint passed SEND, WRITE and READ
latency plus SEND and READ bandwidth (5/5, 41.38 s), with matching enter/leave
fence generations in both gem5 hosts, both UDMA processes and the fabric. A
separate 1,024-WQE SEND bandwidth suite passed at 128/1024/4096 B (3/3,
32.37 s). The final 5-case run used the rebuilt adapter with the all-CPU
instruction-count check; the SEND suite preceded that extra assertion.

The new `modern` gem5 profile is a generic ArmO3 server slice at 3.2 GHz with
4 modeled cores, 64-KiB L1I/L1D, 1-MiB L2, 32-MiB L3, DDR5-6400 across 16
32-bit subchannels (eight dual-subchannel DIMM channels), 32 GiB modeled
capacity, 2 GiB Linux-visible RAM and a 400-Gbit/s UB link. These are explicit
model assumptions, not a claim about one commercial processor. The first
`modern` cold boot retained cache accesses with AtomicSimpleCPU. After roughly
12 minutes, node0
had advanced to only 0.4485 s simulated time / 337.8 million instructions,
the guest UART had not presented a usable shell on a later 20 s probe, and
coordinated checkpointing rejected the guests because `udma0` was not ACTIVE.
The attempt was stopped and its manifest, UART logs and stats are preserved in
`experiments/modern-cold-boot-incomplete-20261009/`. A `sync.ready` marker was
present but only attests timer/OOB setup; it cannot replace the ACTIVE-device
check.

The revised `modern` profile now exposes 8192/8192/524288 JFS/JFR/JFC limits,
one 400-Gbit/s UB port and a provisional 500 ns switch service delay. Its boot
stage uses `AtomicSimpleCPU` with `atomic_noncaching`; the measured region
switches to ArmO3 and timing memory mode. With cache accesses bypassed during
boot, both guests reached ACTIVE devices and shells in about four minutes.
The current guest image contains the tracked one-sided RMA bandwidth timing
patch `patches/source/umdk/0012-synchronize-unidirectional-rma-bandwidth.patch`;
the UMDK build applies it idempotently and checks that the stock provider
subtree is unchanged. The v4 guest image SHA-256 is
`ba261aa3623f94ff157d1ebff8ceb0ffa6d3fc3c72db30963c74a77e2122c3f7`.
The initial compatible coordinated checkpoint was
`checkpoints/shell-ready-modern-ddr5-unidir-1port/` (163 MiB, 117 s save
wall time). The current 2-MiB default selects the newer checkpoint described
below.

One ArmO3 lifecycle bug appeared on the first latency attempt: `switchCpus()`
left gem5 drained, and the next simulation resume reactivated O3 before the
adapter's pause could hold it. Resuming at the current tick before pausing
fixed this; the instruction-count assertion then held through every tested
fence. The v4 128 B READ latency run completed 128 iterations, excluded five
synchronized warm-up deltas, and measured 122 samples: minimum 2.46 us,
median 2.58 us, average 2.57 us and maximum 2.86 us. The later supplied
physical 128 B mean is 3.38 us; the full-length latency run below supersedes
this short provisional comparison.

## Modern gem5 single-direction READ evidence

`experiments/gem5-modern-unidir-read-20261009/` contains the image hash,
manifest, exact commands, UART output, wall times and complete component logs.
The one-sided guest timing patch keeps the non-posting server in conservative
sync until the client completes its final RMA operation. The run used one port
per node, 400 Gbit/s, one switch at a provisional 500 ns per crossing, and
official CTP/RM `urma_perftest` with `-R 8192 -T 8192 -C 524288 -Q 1 -l 256`.
The 16,384-iteration cases explicitly reported JFS depth 8192 and JFC depth
524288. Both endpoints printed matching result rows and exited zero. After
seven bandwidth cases, the source UDMA reported 37,888/37,888 WQEs completed;
the fabric reported 37,888/37,888 completions with no failures or pending
tasks. Fourteen enter/leave fence events matched across both gem5 hosts,
both UDMA processes and the network.

The same-iteration physical comparison is in
[`results/gem5-modern-read-16384-20261009.csv`](../../results/gem5-modern-read-16384-20261009.csv):

| Size | Physical average MiB/s | gem5 average MiB/s | Error | Physical Mpps | gem5 Mpps | Wall time |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 37.92 | 3.50 | 90.77% | 19.882 | 1.836 | 84.14 s |
| 4 KiB | 45,172.39 | 9,220.84 | 79.59% | 11.564 | 2.361 | 203.30 s |

The shorter 1,024-iteration sweep is in
[`results/gem5-modern-read-1024-20261009.csv`](../../results/gem5-modern-read-1024-20261009.csv):

| Size | Physical average MiB/s | gem5 average MiB/s | Error | Physical Mpps | gem5 Mpps |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 37.92 | 3.94 | 89.61% | 19.882 | 2.066 |
| 4 KiB | 45,172.39 | 8,632.48 | 80.89% | 11.564 | 2.210 |
| 64 KiB | 45,588.18 | 46,691.19 | 2.42% | 0.729 | 0.747 |

The short sweep is a trend check, not an iteration-matched substitute for the
physical curve. Two further 2 B repeats at 1,024 iterations yielded 2.332
and 2.337 Mpps; their raw UART output is retained. The 64 KiB point is close
to line rate, while both smaller sizes are operation-rate limited. No gem5
64 KiB run at 16,384 iterations or full 26-size sweep has been completed.

### One-shot JFC notification correction and controlled rerun

The official provider arms a JFC only for event mode. The polling READ
bandwidth path does not arm it, yet the prior UDMA model published a CEQ entry
and MSI for every CQE. The modern profile now recognizes the official JFC
arm doorbell at resource 1 offset `0x40` and consumes each arm after one
notification. CQE DMA and polling remain active without an arm. The legacy
model mode is retained for existing experiments; model state v4 can restore
the earlier v3 shell checkpoint. Native UDMA model tests passed 9/9, and the
Linux device build passed 12/12 tests. The modern v4 checkpoint restored and
both guest READ commands exited zero with all lifecycle fences completing.

The same 1,024-iteration 2 B and 4 KiB tests are in
[`results/gem5-modern-read-oneshot-1024-20261009.csv`](../../results/gem5-modern-read-oneshot-1024-20261009.csv).
The UART transcripts and final component logs are in
`experiments/gem5-modern-unidir-read-20261009/read-bw-*-oneshot.txt` and
`oneshot-run-dual/`.

| Size | Earlier MiB/s | Corrected MiB/s | Corrected Mpps | Physical MiB/s | Corrected error |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 3.94 | 3.60 | 1.887 | 37.92 | 90.51% |
| 4 KiB | 8,632.48 | 7,895.16 | 2.021 | 45,172.39 | 82.52% |

The source UDMA completed 2,048/2,048 WQEs and published **zero** CEQ events
in this polling-only rerun; the prior run published one per WQE. Thus the
unnecessary notifications are corrected but were not the dominant cause of
the roughly 2-Mpps ceiling. The source SQ retirement path admits one CQ
publication at a time, and the per-JFC path waits for a consumer-index DMA
read and CQE DMA write before admitting the next. These serialized paths are
concrete candidates for the remaining operation-rate bottleneck; their
relative contributions have not yet been measured. A hardware-like solution
needs safe producer reservation, ordered SQ retirement and CQE visibility,
and multiple in-flight CQ DMAs,
then a repeat of the 16,384-iteration comparison. The short rerun does not
establish that latency or 64-KiB bandwidth changed.

## Pipelined completions, lifecycle repair and physical latency comparison

The modern UDMA path now reserves distinct JFC ring slots before asynchronous
CQE DMA, admits multiple CQ publications in parallel and retires only a
contiguous successful SQ prefix. The polling JFC consumer index is cached
while producer reservations remain inside the ring; it is refreshed near full.
Descriptor-level tests include out-of-order CQ DMA callbacks and a READ issue
window that resumes queued descriptors after a response. The guest-visible
JFS/JFR/JFC capacities remain 8192/8192/524288. An independent device READ
credit limit is exposed as `--max-read-outstanding` and defaults to 256 in the
modern profile. The shell-ready checkpoint is compatible with this ROI-only
setting because it contains no outstanding READs.

A 16,384-iteration latency attempt exposed an intermittent gem5 lifecycle
failure: a timer/interrupt woke a suspended O3 context before COMMIT and one
node executed 2,650 guest instructions during the fence. The pinned gem5
`System::Threads::activationDeferred()` now records wakeups during the
external fence; the adapter suspends every active context, keeps device/DMA
events running at finite horizons, and replays requested wakeups after COMMIT.
The change is in
[`patches/source/gem5/0002-Defer-thread-activation-during-external-fences.patch`](../../patches/source/gem5/0002-Defer-thread-activation-during-external-fences.patch),
applied and checked by the gem5 build script. A later mixed run completed six
command pairs and 12 lifecycle generations on both nodes, including the full
latency test, with no instruction-count failure. The failed logs and the
successful run logs are retained under
`experiments/gem5-modern-unidir-read-20261009/`.

The common-size READ bandwidth results are in
[`results/gem5-read-window-sweep-20261009.csv`](../../results/gem5-read-window-sweep-20261009.csv).
All 4 KiB window trials used 16,384 iterations and unchanged guest
`-R 8192 -T 8192 -C 524288 -Q 1 -l 256` settings. The no-limit row used the
earlier ordered-CQE build; the 64/128/256/512 rows also include the lifecycle
wakeup repair. Every reported command pair exited zero.

| Size | Iterations | Device READ credits | Physical MiB/s | gem5 MiB/s | Error | gem5 Mpps |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 16,384 | 256 | 37.92 | 12.67 | -66.59% | 6.641 |
| 4 KiB | 16,384 | 64 | 45,172.39 | 12,819.78 | -71.62% | 3.282 |
| 4 KiB | 16,384 | 128 | 45,172.39 | 13,006.43 | -71.21% | 3.330 |
| 4 KiB | 16,384 | 256 | 45,172.39 | 26,045.68 | -42.34% | 6.668 |
| 4 KiB | 16,384 | 512 | 45,172.39 | 12,970.19 | -71.29% | 3.320 |
| 4 KiB | 16,384 | unlimited | 45,172.39 | 13,135.31 | -70.92% | 3.363 |
| 64 KiB | 1,024 | 256 | 45,588.18 | 46,734.55 | +2.51% | 0.748 |

The 64 KiB row is a shorter trend check, not iteration-matched validation.
The 256-credit mixed run had 51,328/51,328 client SQ completions, no source
UDMA errors or CEQ notifications, a peak of 256 pending RMAs and 22,406
READ-window stalls; the ns-3 fabric delivered 133,376/133,376 frames with no
drops or retransmissions. Its device wall time was 1,058 s, including the
16,384-iteration latency test. The 64/128/512 window runs each took roughly
two to three minutes including checkpoint restore, guest synchronization and
one bandwidth command. Among the tested windows, 256 was best, but it still
misses the provisional ±20% gate at 2 B and 4 KiB.

The matching 128 B `read_lat -n 16384 -I 128` row for the earlier 1-KiB
IOCache is in
[`results/gem5-modern-read-lat-20261009.csv`](../../results/gem5-modern-read-lat-20261009.csv).
Perftest excluded six warm-up samples and reported 16,378 observations.

| Metric | Physical | gem5 | Relative error |
| --- | ---: | ---: | ---: |
| Mean | 3.38 us | 2.30 us | -31.95% |
| p99 | 3.52 us | 2.54 us | -27.84% |
| p99.9 | 3.64 us | 3.50 us | -3.85% |
| p99.99 | 6.72 us | 14.50 us | +115.77% |
| Maximum | 9.33 us | 19.31 us | +106.97% |

The mean/p99 baseline is too short, while rare gem5 outliers are too long.
The physical curve's 128 B to 10 MiB means are close to a 3.3-us base plus
400-Gbit/s payload serialization; this is a calibration observation, not proof
of the individual endpoint delays. The switch remains 500 ns **per pass** in
the simulator. No artificial increase to that switch delay was made to force
the 128 B point into agreement. The pinned guest perftest still cannot accept
the physical bandwidth command's `--page_size 65536`, so bandwidth runs are
not fully parameter identical.

## DMA coherence-cache sensitivity and revised modern default

The 1-KiB ARM IOCache in the earlier modern profile is a coherence adapter
on the DMA path. A 2-MiB geometry required a different gem5 object graph, so
`start-ready` correctly found no compatible checkpoint. One cold boot reached
both shells and validated the architected timers, then a coordinated checkpoint
`shell-ready-modern-ddr5-iocache2mb` was saved in 96 s. Both profiles were
restored from their respective checkpoints with the same 256 READ credits,
JFS/JFR/JFC capacities, 3.2-GHz ArmO3 ROI, DDR5-6400 and 400-Gbit/s switch
topology. A paired command sequence controlled test order: 2 B, 4 KiB, then
2 B again, each at 16,384 iterations. Full UART rows are in
[`results/gem5-read-iocache-sweep-20261009.csv`](../../results/gem5-read-iocache-sweep-20261009.csv).

| IOCache | 2 B first Mpps | 4 KiB second MiB/s | 2 B repeat Mpps |
| --- | ---: | ---: | ---: |
| 1 KiB | 3.321 | 25,643.02 | 6.829 |
| 2 MiB | 3.980 | 38,409.19 | 9.596 |

The 2-MiB profile's next 2-B repeat was 9.841 Mpps and 4-KiB repeat
38,534.17 MiB/s. Its 1,024-iteration 64-KiB result was 46,738.62 MiB/s,
and a 128-iteration 128-B latency check averaged 2.29 us. The 4-KiB repeated
result is 14.69% below the physical 45,172.39 MiB/s and passes the provisional
±20% gate. The warmed 2-B result remains 50.50% below the physical
19.882 Mpps; the first-run result is further away. These are actual test-order
effects, so future comparisons should state whether they use the first run or
a warmed repeat. The different IOCache capacities are geometry assumptions,
not a claim that this gem5 cache is an exact implementation of a commercial
NIC's DDIO behavior.

The `modern` IOCache default is now 2 MiB. This READ calibration restored
`shell-ready-modern-ddr5-iocache2mb`; a later, compatible checkpoint with a
newer guest perftest is documented in
[`gem5-write-512-multijetty-20261010.md`](gem5-write-512-multijetty-20261010.md).
The original 1-KiB checkpoint is retained for controlled comparisons. A full
16,384-iteration, 128-B
`read_lat` run on the new default completed at both endpoints and reported
16,378 samples. The raw UART transcript is
`experiments/gem5-modern-unidir-read-20261009/read-lat-128-16384-iocache2mb.txt`,
the final component logs are in `iocache2mb-lat-run-dual/`, and the row is
also in [`results/gem5-modern-read-lat-20261009.csv`](../../results/gem5-modern-read-lat-20261009.csv).

| Metric | Physical | 2-MiB gem5 | Relative error |
| --- | ---: | ---: | ---: |
| Mean | 3.38 us | 2.30 us | -31.95% |
| p99 | 3.52 us | 2.54 us | -27.84% |
| p99.9 | 3.64 us | 4.33 us | +18.96% |
| p99.99 | 6.72 us | 17.21 us | +156.10% |
| Maximum | 9.33 us | 32.98 us | +253.48% |

Increasing the IOCache improved bandwidth but did not change the mean or
p99 latency. The rare latency tail worsened in this one run, so its exact
quantiles need repeated-run evidence before tuning. The mean and p99 still
miss the ±20% target. The 500-ns switch delay remains one pass, with two
passes in a READ round trip.

## Calibration work still required

1. Instrument the per-operation critical path, particularly guest posting and
   CQ polling, host DMA/CQE visibility and target READ service. The sweep shows
   that changing only the device credit window does not reach 80% accuracy at
   2 B, while the 2-MiB IOCache gets a warmed 4-KiB run within ±20%. A
   cache-coherent completion model may matter, but these results do not
   isolate that cause.
2. Calibrate fixed READ latency against several packet sizes while preserving
   the confirmed 500 ns per-pass switch delay and the 400-Gbit/s serialization
   slope. Separately investigate the p99.99/max outliers.
3. Match the physical 64-KiB page-size behavior or document the difference,
   then run at least a representative full-length size sweep and repeated runs.
   CPU/NUMA placement and physical device firmware details would help choose
   between plausible endpoint models.

The project can now run the official CTP/RM READ path from a fast-boot modern
gem5 checkpoint with fine-grained ArmO3 ROI and stable lifecycle fences. The
2-MiB default reproduces the large-message link-rate plateau and brings a
warmed 4-KiB READ-bandwidth result within ±20%. The 2-B bandwidth and 128-B
mean/p99 latency **do not meet** the requested roughly 80%-accuracy target.
Only selected sizes have been simulated, so the full physical trend and tail
distributions remain unvalidated.
