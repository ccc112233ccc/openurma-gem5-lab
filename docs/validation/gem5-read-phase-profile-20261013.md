# 128-B READ source phase profile (2026-10-13)

> **2026-10-14 correction:** The fixed-32768-operation J=1/2/4 comparison
> below crossed `urma_perftest`'s automatic `no_peak` threshold of 20000
> iterations **per Jetty**. J=1 skipped per-operation timestamps, while J=2/4
> recorded them. The large between-API difference therefore does not establish
> a Jetty resource bottleneck. Matched-mode profiling and the corrected
> interpretation are in `gem5-read-loop-profile-20261014.md`.

## Scope and method

This continues `gem5-read-jetty-profile-20261013.md` with function-boundary
timing on the READ initiator. The restored modern checkpoint, official CTP/RM
stack, 128-B payload, one physical UB port (`-p 0`), 400-Gbit/s link, 500-ns
switch pass, one posting process, 32768 total operations, `-l 256 -Q 1 -I 0`,
and aggregate JFS/JFR depth 8192 were unchanged. J=1/2/4 respectively used
per-Jetty depths 8192/4096/2048. Only the TCP control port changed between
cases. Both guests still entered synchronized ArmO3 ROI and exited zero.

An ARM64 `LD_PRELOAD` library on the **node1 initiator only** wraps
`urma_post_jetty_send_wr`, `urma_post_jfs_wr`, and `urma_poll_jfc`.
It reads `cntvct_el0` before and after each call, aggregates counts and ticks,
and prints one line at process exit. Its second revision also totals the
time between consecutive API calls as post→post, post→poll, poll→post, and
poll→poll gaps. `cntfrq_el0` is 25,165,824 Hz in this guest; converting these
ticks gives guest virtual time. The profiler adds no per-call logging.

The ordinary checkpoint contains no `m5 readfile` CLI. An opt-in
`UBSIM_GUEST_READFILE` environment variable for `scripts/run/run-dual.sh` exposed the host
profiler binary through gem5's readfile pseudo-op without changing the
checkpoint or initramfs. A 6-KB `tools/ubsim-readfile.c` bootstrap was sent
to the guest in short UART chunks with `tools/transfer_guest_bootstrap.py`;
it copied the 13,448-byte profiler into `/tmp` on the initiator. The binary
is opt-in via `LD_PRELOAD` in the profiling driver. Only the initiator loaded
it; the target used the regular perftest process.

## Probe effect and phase results

The first instrumented J=1 case ran at 10.638 Mops/s. The uninstrumented
fixed-aggregate-depth J=1 pair immediately before this session ran at
10.828/10.974 Mops/s; the later instrumented J=1 pair ran at
10.736/10.799 Mops/s. The probe's rate effect is roughly 2–3%, so use the
instrumented cases to compare phases, not as uncorrected physical-fidelity
numbers. All cases produced 32768 CQEs, 128 Jetty post calls, exactly 2048
positive JFC polls of 16 CQEs, **zero empty polls**, and zero JFS post calls.

| Jettys | Rate, Mops/s | First post to last CQE, µs | Inside post, µs | Inside poll, µs | Between calls, µs |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 10.736 | 3050.9 | 1457.3 | 1496.8 | 96.8 |
| 2 | 8.910 | 3675.7 | 1459.0 | 1544.8 | 672.0 |
| 4 | 8.811 | 3716.7 | 1516.9 | 1539.9 | 659.9 |
| 1 repeat | 10.799 | 3033.1 | 1400.8 | 1536.8 | 95.5 |

The J=1→J=2 measured interval rises by **625 µs**. About **575 µs (92%)**
of that increase lies between the wrapped provider API calls. J=1→J=4 has
a 666-µs interval increase, with 563 µs (85%) between calls. Time inside
the post and poll calls is comparatively stable. A separate J=1/J=2 run
with additional maximum-gap counters reproduced the split (3.031/3.637 ms
total, 101/682 µs between calls).

| Jettys | Post→post, µs | Post→poll, µs | Poll→post, µs | Poll→poll, µs |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 3.5 | 10.1 | 9.0 | 74.1 |
| 2 | 21.1 | 10.3 | 324.1 | 316.4 |
| 4 | 21.5 | 11.2 | 305.3 | 321.9 |
| 1 repeat | 2.5 | 5.5 | 13.5 | 74.1 |

All rows had the same call-transition counts: 31 post→post, 97 post→poll,
96 poll→post, and 1951 poll→poll. The J=2/J=4 growth therefore occurs in
the work *between* otherwise identically counted API calls. The extra time
is distributed across multiple transitions, not one rare interruption: in
the J=2 long-tail check, poll→post totaled 8110 ticks with a 353-tick
single-gap maximum, while poll→poll totaled 8021 ticks with a 104-tick
maximum. Neither maximum can account for its category's total.

## Localization and limits

`run_once_bw` in the pinned UMDK source
(`sources/umdk/src/urma/tools/urma_perftest/perftest_run_test.c`, around
lines 2119–2285) fills each Jetty's posting window, then polls the shared
JFC in 16-entry batches. Between the measured calls it traverses Jettys,
checks per-Jetty `scnt`/`ccnt` windows, and accounts for each CQE using
`cr[i].user_ctx`. The two growing gap classes contain precisely this loop
control and completion accounting. This is a stronger localization than the
prior aggregate ArmO3 stalls, but the wrapper cannot name a single source
instruction or distinguish CPU execution from cache stalls/interrupts
inside those gaps. The no-probe and probe sweeps both show increasing L1D/L2
misses with Jetty count, so the remaining calibration should sample PCs or
instrument the specific loop statements before changing a timing parameter.

The busy ArmO3 core is not always the same CPU across cases: the first three
cases in the gap sweep used `switch_cpus1`, while the final J=1 used
`switch_cpus3`. Select the busiest core per ROI before comparing counters.
The busy core's committed instructions rose from about 11.72 million at
J=1 to 12.50/12.57 million at J=2/J=4. Its rename serializing-stall cycles
rose from 0.55 million to 1.97/1.88 million. These aggregate statistics
support more expensive source execution but do not assign the gap to the
provider's `dmb` barriers; the wrapper measures provider time separately.

Across all ten instrumented cases, the source UDMA retired 327680 READ WQEs
with `read_window_stalls=0`, `sq_depth_rejects=0`, and `ubase_errors=0`.
The fabric delivered all 655360 request/response frames, with zero drops,
retransmissions, congestion cuts, queue marks, and wire-pacing waits.
There is no evidence that the single 400-Gbit/s physical port limits this
128-B run. The physical single-Jetty value remains 23.098 Mops/s; this
experiment does not include physical multi-Jetty results.

## Evidence and reproduction

- Initial probe check: `experiments/gem5-read-phase-j1-20261013/`.
- First J=2/J=4/repeat profile: `experiments/gem5-read-phase-multijetty-20261013/`.
- Four-gap profile and per-ROI gem5 stats: `experiments/gem5-read-phase-gaps-20261013/`.
- Gap-maximum check and final device/network logs:
  `experiments/gem5-read-phase-tail-20261013/`.
- Source: `tools/urma_phase_profile.c`, `tools/ubsim-readfile.c`,
  `tools/transfer_guest_bootstrap.py`, `tools/profile_gem5_read_jettys.py`.

The same checkpoint was restored once for this session. After `./lab sync`,
a paired no-traffic Atomic→ArmO3→Atomic transition normalized the guests.
Every profiler case used `--client-preload /tmp/liburma_phase_profile.so`
and `--aggregate-depth 8192` in `profile_gem5_read_jettys.py`. The driver
validated matching peer rows, the exact CQE count, and all five ROI fences.
The run was stopped after profiling to flush final component counters.
