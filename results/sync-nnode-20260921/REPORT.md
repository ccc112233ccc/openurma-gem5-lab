# Paired N-node Adapter synchronization

Date: 2026-09-21

## Scope

This checkpoint generalizes the experiment from one fixed pair to an even
number of gem5 guests (2--8). All endpoints attach to one independent UB
switch process. The initial scalable topology pairs adjacent endpoints:
`0<->1`, `2<->3`, and so on. Each pair has its own OOB setup relay and its own
DATA/SYNC relationship; it does not wait for unrelated pairs in Adapter-local
mode.

This is intentionally not an all-to-all EID router yet. Pairing isolates the
synchronization scaling question from future arbitrary-destination routing.

## Implementation

- `ub-switch-sim --multi` maps any even number of endpoint rings and validates
  a symmetric, non-self peer map.
- Each endpoint owns independent active-phase state and egress timing.
- The switch mirrors conservative promises only between paired peers and
  busy-polls while any pair is in a measured phase.
- `run-dual.sh --nodes N` launches N independent full-system gem5 processes,
  one shared UB switch, and N/2 OOB relays.
- `sync-dual.sh`, `status-dual.sh`, `stop-dual.sh`, and the serial command tool
  discover the node count from the run manifest.
- `run-paired-latency.sh` starts one official two-sided `urma_perftest`
  session per pair at the same host-side gate.

The two-node CLI remains backward compatible.

## Validation

Configuration for the measured four-node run:

- Apple M2 host, Docker Desktop Linux container;
- four independent ARM64 gem5 full-system/openEuler guests;
- one 3 GHz `AtomicSimpleCPU` per guest;
- official OLK UDMA kernel stack, official UMDK UDMA provider, and official
  `urma_perftest` WQE/CQE path;
- two simultaneous CTP/RM/SEND_IMM sessions, 128-byte messages, one Jetty and
  100 measured samples per endpoint;
- one shared 400-Gbit/s UB switch, 100 ns propagation and no fitted switch
  service delay.

All four endpoints completed in both modes. Every endpoint reported median
1.05 us, average 1.06 us, p99 1.07 us and standard deviation 0.01 us. The
switch's four-endpoint protocol test also verifies that pair `2<->3` cannot
observe DATA or SYNC state from pair `0<->1`.

## Wall-time result

| Synchronization | Cold first run | Steady runs | Steady mean |
| --- | ---: | --- | ---: |
| C++ Adapter-local | 18.04 s | 13.27, 13.44, 13.51 s | **13.41 s** |
| dist-gem5 global barrier | 18.25 s | 14.37, 14.29 s | **14.33 s** |

For this four-node workload, Adapter-local is 6.4% faster than the global
barrier in steady state. The cold runs are nearly equal and are reported
separately instead of being mixed into the steady-state average.

The earlier two-node 128-byte experiment measured 11.457 s for the same C++
Adapter design and 11.913 s for the global barrier, a 3.8% Adapter advantage.
Those historical runs enabled ROI-stat dumps while this N-node functional
comparison did not, so their absolute scaling ratios are indicative rather
than a strict controlled A/B. The within-four-node comparison above is the
controlled result: same booted guests, workload, message count and model;
only the synchronization implementation changes.

## Interpretation

The result supports the expected direction, but not an unbounded speedup.
Removing unrelated participants from the conservative dependency set widens
the advantage from a few percent at two nodes to about six percent at four
nodes. At the same time, each full-system guest still executes all Linux,
driver and benchmark instructions, so adding gem5 processes remains the
dominant host cost. The Adapter refactor improves synchronization scalability;
it does not make CPU simulation free.

The next meaningful experiment is an 8-node/four-pair run on a host with
enough physical CPU and memory, followed by general EID-based routing. It is
better to add that routing explicitly than to disguise fixed pairing as a
complete multi-host fabric.

## Evidence

- `adapter-4node*.uart.txt` and `global-4node*.uart.txt`: complete four-UART
  transcripts.
- `*.time.txt`: host wall-time captures.
- `tools/test_ub_switch_sim.py`: legacy two-endpoint and isolated four-endpoint
  DATA/SYNC protocol tests.
