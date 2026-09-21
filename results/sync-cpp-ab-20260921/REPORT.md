# SimBricks-style C++ adapter synchronization

Date: 2026-09-21

## What changed

The original Adapter-local implementation returned from gem5 to Python for
every conservative horizon. Python called `adapterSyncStep()`, slept when no
promise was available, and invoked a bounded `m5.simulate()` for each safe
slice. A 128-byte, 100-sample run crossed that boundary about 41,000 times.

The new implementation follows the event-driven structure used by the
SimBricks gem5 adapter:

- a C++ `EventFunctionWrapper` is inserted into gem5's event queue;
- the callback drains DATA/SYNC records and busy-polls the shared-memory ring
  until it obtains a future conservative horizon;
- the callback schedules itself at that exact tick, so ordinary gem5 events
  cannot cross the horizon;
- Python handles only the infrequent collective ON/OFF pseudo instructions;
- the independent switch busy-polls only while both endpoints are in an active
  synchronized phase and sleeps while the guests boot or wait at a shell.

Unlike a SimBricks experiment synchronized from tick zero, these full-system
guests boot independently and enable synchronization only around an ROI. Their
absolute `curTick()` values can therefore differ substantially. Each active
generation now uses its own phase-relative wire time. Link propagation,
serialization and switch delay remain durations in that common domain, while
each endpoint converts received timestamps back to its local absolute tick.

Reference implementation and design description:

- <https://github.com/simbricks/gem5/tree/simbricks/src/simbricks>
- <https://docs.simbricks.io/learn/simulator-integration/implementation/>

## Method

- macOS/Apple M2 host and Docker Desktop Linux container.
- Two independent gem5 full-system openEuler guests.
- `fast` profile: one `AtomicSimpleCPU` at 3 GHz per node.
- Official UDMA kernel driver, official provider and official
  `urma_perftest send_lat` path.
- Independent UB switch, 400 Gbit/s, 100 ns propagation, one serialization
  stage and no additional switch service delay.
- Guest boot is excluded. Every data point covers a complete two-sided
  `run-latency.sh` command with 100 measured samples and ROI stats enabled.
- Final C++ results are arithmetic means of three runs. The earlier Python and
  global-barrier means come from the companion `../sync-ab-20260921` report.

## End-to-end result

| Implementation | Size | Wall time (s) | Total process CPU (s) | Voluntary context switches | node0 ROI host time (s) | node0 hostTickRate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Python Adapter-local | 128 B | 26.566 | 31.997 | 398,473 | 15.150 | 15,709,522 |
| Global barrier | 128 B | 11.913 | 23.557 | 30,419 | 1.287 | 170,157,192 |
| C++ event Adapter | 128 B | **11.457** | 24.650 | **20,982** | **1.033** | **203,458,845** |
| Python Adapter-local | 4096 B | 13.512 | 23.767 | 43,172 | 2.397 | 98,988,127 |
| Global barrier | 4096 B | **10.922** | **21.737** | 30,181 | **1.243** | **192,446,043** |
| C++ event Adapter | 4096 B | 11.649 | 25.337 | **20,186** | 1.400 | 167,113,143 |

For 128 B, the final C++ Adapter is 2.32 times faster than the Python Adapter
and 3.8% faster than the global barrier in wall time. It reduces voluntary
context switches by 94.7% versus the Python version. For 4096 B it is 13.8%
faster than the Python Adapter, but remains 6.7% slower than the global barrier.
This is a two-node result, not evidence that one scheme will scale better at
larger node counts.

## Optimization evidence

The intermediate 128-byte runs isolate the sources of overhead:

| Step | Wall time (s) | Observation |
| --- | ---: | --- |
| Python stepping | 26.566 mean | About 41k Python/pybind/`m5.simulate()` slices |
| C++ event, absolute ticks | 25.316 | Python crossings disappear, but independently booted nodes retain a large tick skew |
| C++ event, phase-relative ticks, sleeping switch | 15.169 | Removes catch-up on the pre-ROI boot-time skew |
| C++ event, phase-relative ticks, active busy-poll | **11.457 mean** | Removes Docker/macOS short-sleep rounding from the synchronized hot path |

The first C++ run reduced gem5 voluntary context switches from roughly 400,000
to roughly 4,800 per node, proving that the Python hot loop had been removed.
It did not initially improve wall time because one endpoint entered the ROI
about 267 ms of virtual time ahead of the other and waited in C++. Using a
phase-relative timestamp domain removed that artifact. The sleeping-switch run
then showed the switch using only 0.13 CPU seconds while making about 15,700
voluntary context switches; active-phase busy-polling raised switch CPU use to
about 1.3--1.7 seconds and reduced end-to-end time by a further 3.5 seconds.

## Modeled results and correctness

| Size | Median | Average | p99 | Three-run wall-time range |
| ---: | ---: | ---: | ---: | ---: |
| 128 B | 1.03--1.05 us | 1.04--1.06 us | 1.07 us | 11.31--11.62 s |
| 4096 B | 1.17 us | 1.17 us | 1.17 us | 11.63--11.66 s |

For every final run, node0 and node1 advanced the same number of ROI virtual
ticks and reported matching latency distributions. No panic, fatal error,
Python exception or causality violation appeared in the final logs. The
standalone switch protocol/forwarding smoke test also passed.

## Interpretation

The user's concern was correct: invoking the synchronization loop through a
Python method for every lookahead interval is the wrong integration level for
gem5. The proper boundary is a C++ adapter event scheduled on gem5's event
queue. The remaining 4096-byte gap is no longer Python overhead; it is the
cost of fine-grained null-message synchronization and an additional active
switch process. Future optimization should focus on safe-horizon coalescing or
ring notification, followed by 4/8-node scaling measurements, rather than
moving logic back into Python.

