# Adapter-local versus global-barrier synchronization

Date: 2026-09-21

## Method

- Host: macOS on Apple M2, Docker Desktop Linux container.
- Two independent gem5 full-system guests, `fast` profile, one
  `AtomicSimpleCPU` at 3 GHz per guest.
- Official UDMA kernel driver, official provider and official
  `urma_perftest send_lat` path.
- Independent UB switch, 400 Gbit/s, 100 ns propagation and one serialization
  stage.
- Guest boot and `sync-dual.sh` were excluded. Each mode was booted once, then
  three 100-sample runs were collected for 128 B and 4096 B.
- Every run used `--roi-stats`. Wall time and `/proc` CPU/context-switch deltas
  cover the complete `run-latency.sh` command; gem5 `hostSeconds` and
  `hostTickRate` cover the measured ROI.
- `adapter-128-r1` is a separate 2000-sample endurance run and is not included
  in the three-run aggregate.

## Aggregate result

Values are arithmetic means of three runs.

| Mode | Size | Wall time (s) | Total process CPU (s) | Voluntary context switches | gem5 ROI host time, node0 (s) | gem5 hostTickRate, node0 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Adapter-local | 128 B | 26.566 | 31.997 | 398,473 | 15.150 | 15,709,522 |
| Global barrier | 128 B | 11.913 | 23.557 | 30,419 | 1.287 | 170,157,192 |
| Adapter-local | 4096 B | 13.512 | 23.767 | 43,172 | 2.397 | 98,988,127 |
| Global barrier | 4096 B | 10.922 | 21.737 | 30,181 | 1.243 | 192,446,043 |

For 128 B, Adapter-local is 2.23 times slower in end-to-end wall time, uses
36% more aggregate CPU, performs 13.1 times as many voluntary context switches
and advances gem5 ROI ticks at only 9.2% of the global-barrier rate. For 4096 B,
it is 24% slower, uses 9% more aggregate CPU and performs 1.43 times as many
voluntary context switches.

The additional fourth process in global-barrier mode is not the bottleneck in
this two-node experiment. It consumed about 0.15 CPU seconds per run. The two
gem5 processes dominate CPU consumption in both modes.

## Guest-visible result

The synchronization implementation changes simulation throughput, not the
modeled result. Representative results were the same in both modes:

| Size | Median | Average | p99 |
| ---: | ---: | ---: | ---: |
| 128 B | 1.05 us | 1.06 us | 1.07 us |
| 4096 B | 1.17 us | 1.17 us | 1.17 us |

Small run-to-run maximum values differed by a few hundredths of a microsecond,
but there was no systematic latency shift between synchronization modes.

## Adapter-loop counters

One additional diagnostic run counted the Adapter simulation loop directly.
The following values are for the measured ON/OFF synchronization generation;
the preceding short rendezvous generation is excluded.

| Size | Node | Host time in Adapter generation (s) | Steps | Wait polls | `m5.simulate()` slices |
| ---: | --- | ---: | ---: | ---: | ---: |
| 128 B | node0 | 19.225 | 41,386 | 343 | 41,043 |
| 128 B | node1 | 19.225 | 46,125 | 5,042 | 41,083 |
| 4096 B | node0 | 2.626 | 3,420 | 1,478 | 1,942 |
| 4096 B | node1 | 2.625 | 3,566 | 1,623 | 1,943 |

The 128-byte inline path therefore creates roughly 41,000 bounded
`m5.simulate()` calls for 100 reported samples. This is the primary slowdown.
The endpoint currently returns to Python for every safe horizon and polls an
empty Adapter with a 20-us host sleep. The UB switch also polls its rings with a
20-us host sleep. These host scheduling operations do not change virtual
latency, but they severely reduce simulation throughput and CPU utilization.

Average aggregate CPU occupancy reinforces this conclusion:

| Mode | Size | Aggregate CPU seconds / wall second |
| --- | ---: | ---: |
| Adapter-local | 128 B | 1.20 cores |
| Global barrier | 128 B | 1.98 cores |
| Adapter-local | 4096 B | 1.76 cores |
| Global barrier | 4096 B | 1.99 cores |

Adapter-local spends substantially more time sleeping or waiting instead of
using the two available gem5 execution threads.

## Conclusion

The current Adapter protocol is a useful architectural boundary and produces
stable virtual-time results, but it is not yet a speed optimization. For this
two-node workload, the mature C++ dist-gem5 barrier is faster despite its extra
process. Adapter-local should remain an experimental/scalability path until its
host-side scheduler is optimized.

The highest-value next changes are:

1. Move the conservative stepping loop out of Python and into C++/gem5's event
   queue, so a SYNC grant schedules an event without returning through pybind.
2. Replace 20-us polling sleeps with eventfd/futex notification on ring head and
   phase changes.
3. Coalesce null-message promises and publish a farther safe horizon when both
   the endpoint and switch queues are empty.
4. Repeat the comparison at 4, 8 and more nodes. A local Adapter design can
   still scale better even though its present two-node constant overhead is
   worse.

The raw JSON counters and dual-UART transcripts were removed from the current
tree after the values above were consolidated. They remain recoverable from
commit `dc1c3ff` and earlier history. The 2000-sample Adapter endurance run took
559.495 seconds, which confirms that the short-run slowdown is sustained rather
than a one-time startup artifact.
