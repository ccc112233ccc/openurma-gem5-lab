# 512-B WRITE checkpoint warm-up experiment (2026-10-10)

## Question and controlled runs

Does a third run change the result after a checkpoint restore, and does
`urma_perftest -w` remove the cold first-run effect?

The first experiment restored `shell-ready-modern-ddr5-multijetty-write`
once, then launched three independent `urma_perftest write_bw` command pairs
in the same guests without another restore. All runs used a single physical
UB port per node, 400 Gbit/s link, a single switch with 500 ns per pass,
512-byte messages, four Jettys, 4096 iterations per Jetty (16,384 WRITEs),
`-I 0 -T 8192 -R 8192 -C 32768 -l 256 -Q 1 -p 0`, and distributed
ROI synchronization. The test harness changed only the TCP control port
21520/21521/21522 to let successive command pairs bind cleanly. The physical
UB port stayed `-p 0`. All guest commands exited zero and both peers printed
matching rows. Raw CSV, UART transcripts, per-ROI gem5 stats, and component
logs are under `experiments/gem5-write-512-third-run-20261010/`.

| Consecutive trial | Average MiB/s | Mpps | Main O3 core | Main core IPC | L3 misses/accesses | Main core L1D miss latency |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| First | 2259.23 | 4.626899 | 2 | 0.602 | 54,134/54,623 | 76.0 ns |
| Second | 4666.93 | 9.557868 | 1 | 1.241 | 18,535/39,443 | 18.7 ns |
| Third | 4665.31 | 9.554546 | 1 | 1.239 | 17,916/45,562 | 17.7 ns |

The third throughput is 0.035% below the second. Thus this particular
512-byte, four-Jetty workload is essentially stable by its second complete
run. The posting thread moved from modeled CPU 2 on the first trial to CPU 1
on the second and third; both later trials stayed on CPU 1. A preceding
same-core first/repeat experiment in
`experiments/gem5-write-512-prefetch-fix-20261010/` measured 2267.55 then
4744.24 MiB/s with the posting thread on CPU 2 both times. Core migration
does not explain the general cold-to-warm effect.

## Explicit perftest warm-up control

A separate restore of the same checkpoint ran the same first WRITE trial
with `-w` on both peers, with no other parameter changed. Its result was
2273.98 MiB/s, 4.657106 Mpps; the main O3 core was CPU 2, IPC was 0.606,
and L3 misses/accesses were 54,092/54,612. Raw UART and ROI statistics are
under `experiments/gem5-write-512-explicit-warmup-20261010/`. This is within
0.7% of the three-run experiment's first result and remains roughly half of
its warmed result.

The code explains why `-w` is insufficient here. `perform_warm_up()` runs
before `toggle_dist_sync("enable RMA bandwidth loop")`, which is the lifecycle
boundary that switches the guests from fast Atomic CPUs into ArmO3 timing
mode. With `-l 256`, `perform_warm_up()` sends exactly one WQE per Jetty,
not 256 or 4096: `warmupsession` is 256, and the loop increments by 256.
Therefore it neither executes the full WRITE loop nor warms the ArmO3 cache
hierarchy under the measured CPU mode.

The restore checkpoint starts in `atomic_noncaching`, so the shell setup
before the first ROI bypasses CPU caches. A subtle state transition occurs
at the end of that ROI: `m5.switchCpus()` changes the memory mode to the
incoming `AtomicSimpleCPU`'s required mode, which is `atomic`, *not*
`atomic_noncaching`. The switch implementation only writes back and
invalidates all caches when the incoming mode is `atomic_noncaching`.
Consequently, after the first run, guest setup for a later command occurs
in a cache-enabled Atomic mode, and cache contents can survive the O3-to-
Atomic switch. The second and third timed ROIs see far fewer shared-L3
misses and much shorter L1D miss service times. The main O3 core's
store-queue-full events fall from about 5.99 million in the first ROI to
about 1.09 million in the second. These statistics show the consequence of
the changed mode but do not identify each warmed line or address mapping.

## Different-operation control: READ before WRITE

A third restore of the same checkpoint ran a 512-byte, four-Jetty, 16,384-op
`read_bw` first, then the original 512-byte `write_bw`, on the same physical
port and with the same queue, completion, and post-list parameters. Both
peers exited zero for both commands. The first READ achieved 2318.68 MiB/s;
the subsequent WRITE achieved 4591.57 MiB/s. The WRITE was within 1.6% of
the second WRITE in the three-run experiment (4666.93 MiB/s), even though no
prior WRITE benchmark had run since restore. Its shared L3 misses/accesses
were 19,295/40,738 and its main core L1D miss latency was 18.7 ns, close to
the warmed WRITE state. Raw UART and ROI stats are under
`experiments/gem5-read-before-write-20261010/`.

The READ posting thread ran on modeled CPU 2 and the following WRITE on
CPU 1. This showed that a prior identical WRITE was unnecessary, but it
could not establish URMA-specific warming: READ also switched the simulator
through O3 and back to cache-enabled Atomic mode.

## No-traffic CPU-mode control

A fourth restore ran only `/usr/bin/ubsim-cpu-switch-op` twice in each guest:
`Atomic -> ArmO3 -> Atomic`. There was no READ, WRITE, or other URMA request
before the first measured WRITE. The direct diagnostic op does not toggle
distributed synchronization; no UB workload was active during either CPU
switch. Both guest command pairs exited zero, and gem5 logs confirm the two
switch directions. The unchanged 512-byte WRITE then achieved **4619.17
MiB/s**, 9.460063 Mpps, within 1.0% of the second WRITE in the three-run
experiment. Its main core IPC was 1.231, L1D miss latency 18.5 ns, and
shared L3 misses/accesses 19,011/40,107, all close to the warmed state.
Raw UART, gem5 ROI stats, and logs are under
`experiments/gem5-mode-toggle-before-write-20261010/`.

This control is stronger than READ-before-WRITE for attributing the large
first-run gap: an earlier URMA benchmark is **not necessary**. The CPU-mode
transition itself is sufficient to produce the high-bandwidth condition.
The most direct mechanism supported by code is cache-enabled Atomic-mode
resource setup before the first timed WRITE. The experiment does not fully
separate cache contents left by the no-work switch from cache fills during
perftest setup, or distinguish every guest mapping effect from cache effects.

For this profile and workload, report the initial noncaching result separately
from the post-switch caching result. When comparing Jetty counts, establish
the same memory mode before each case; a paired CPU-mode toggle after restore
is much cheaper than a full URMA warm-up and produced the high-bandwidth
condition here. Do not interpret the `urma_perftest` `average` field as an
average of independent trials: it divides 8 MiB of completed WRITE payload
by the elapsed simulated time within one run.
