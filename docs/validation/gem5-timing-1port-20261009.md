# gem5 single-port timing gate (2026-10-09)

Requested scope: check actual synchronized perftest results, without fitting
model constants to physical-board measurements. Configuration: two ARM64 FS
guests, AtomicSimpleCPU/atomic_fast at3GHz, official provider/driver, standalone
UDMA and ns-3-UB, one400Gbit/s port per endpoint,100ns link lookahead, ROI
conservative synchronization. Atomic-fast does not model cache/memory stalls;
these tests cannot establish server CPU timing fidelity.

Evidence root: `experiments/gem5-timing-1port-20261009/`.

## Checkpoint and discovered synchronization defects

No compatible one-port ROI checkpoint existed. Created
`shell-ready-timing-v3-1port` once: launch6.272s + shell/OOB155.870s,
checkpoint12.604s. Subsequent attempts restore this checkpoint, not cold boot.

1. `smoke/`: perftest returned zero but all five fence logs were absent.
   The pinned UMDK/gem5 bundles still consume internal `OPENURMA_*` instrumentation
   names and emit `openurma lifecycle sync fence`, whereas the launcher used
   renamed public UBSIM names. Explicit boundary mappings now support the actual
   pinned ABI. This is not a second hardware model or a driver modification.
   A new five-participant per-case fence validator rejects this false success.
2. `smoke-fixed/`: real entry blocked for120.193s. Both devices had zero WQEs and
   no DMA; fabric had no tasks or outputs. The global ns-3 event queue never
   empties because `UbSwitchAllocator` perpetually reschedules its10ms deadlock
   watchdog. The fence now requires owned business work to drain (including
   partial WQEs and target DMA), not all background events. Native absolute time
   and events are retained; `AdvanceTo()` is not relaxed.
3. `smoke-final/`: all five entered generation1 but UDMA stayed at0ps, fabric at
   100000ps, WQEs remained0; bounded attempt rejected after120.193s. COMMIT drain
   could consume the first new-epoch SYNC before resetting incoming timestamps,
   losing its horizon. gem5 and UDMA now stop their receive batch at COMMIT until
   epoch reset; ns-3 refreshes its local `now` before publishing the new SYNC.

These failed runs provide no accepted bandwidth numbers. Logs are retained in
`abi-failure/`, `fence-stall/`, `epoch-stall/` and the named case directories.

## Remaining full-system blocker: exit drain

After the epoch fixes and a59.099s gem5 incremental rebuild, restore+OOB took
6.973+5.955s. `smoke-epoch/` entered all five participants successfully and
actually executed SEND: each UDMA reported8 WQEs/8 SQ completions and ns-3
reported16 submitted/16 completed/0 failed native WQEs. This is progress, not
an accepted timing result: the complete exit fence is still missing.
The bounded case failed after120.206s (suite120.219s); stalled instances were
stopped and `exit-drain/` retains both host/device logs and the network log.

Progress snapshot: node0 at52350565ps, quiescent; node1 at52544340ps with one
pending host DMA write and `receive_busy=1`; network at52450565ps with no native
WQE left. The early finisher's `toggleLifecycleSync()` deschedules its adapter
poll event and blocks inside `serviceDevice(UINT64_MAX)`. It neither advances
gem5's event queue nor publishes a newer safe horizon. Its UDMA then constrains
the network minimum horizon, preventing the other endpoint's tail DMA from
finishing. Independent source review confirms this circular wait mechanism.

Required next repair: event-driven exit drain. Pause guest CPU execution while
continuing the actual gem5 device event queue and finite conservative SYNC
horizons in the old epoch. Commit/rebase/resume only after all ownership drains.
Do not synthesize advancing timestamps, grant infinite horizons or skip pending
DMA. Acceptance needs an asymmetric-finish counterexample with a real delayed
DMA callback, plus a complete repeated full-system entry/exit sequence.

The planned1024-iteration bandwidth suite is deliberately fail-fast after the
smoke gate; it has not run and has no reportable bandwidth rows.

## Independent lower-level evidence

Native idle lifecycle test: two endpoints, enter/leave twice, each active epoch
advances12ms across the watchdog period. Old gate: generation1 timeout,
20.192s, exit6. New gate: PASS0.344s; four matching commits; native event queue
remains nonempty and21 background events actually execute. After epoch-time
fix: PASS0.325s. This is adapter evidence, not official-driver execution.

CTP NO/RO/SO native contract passed0.271s; lost READ-response duplicate
suppression passed0.228s. Network rebuild4.257s, peer rebuild0.648s; subsequent
epoch fix rebuild8.903s. UDMA rebuild +12/12 CTests3.599s, including a real-ring
COMMIT-followed-by-SYNC preservation counterexample. Python regression130/130
passed3.926s (4.049s process wall).

## Measurement acceptance

For each full-system case, save only the newly appended portions of both
gem5, both UDMA and network logs. All five must contain identical, strictly
increasing complete enter/leave generation sequences. A command returning
zero or setting an environment variable is insufficient.

`timing-send` uses SEND128/1024/4096B, N1024, J1, depth512, post-list64,
CQ moderation64, single direction (no `-B`). Read client output, not an average
of client and server windows. Official MiB/s converts to decimal GB/s by
multiplying1048576/1e9. One400Gbit/s port has a50GB/s raw one-way ceiling;
short-window results do not establish steady-state saturation.

The pinned guest benchmark only supports single-direction bandwidth ROI for
SEND; READ/WRITE bandwidth ROI requires its bidirectional path. Do not label
the proposed SEND suite a single-direction READ/WRITE measurement.
