# Current modular model on gem5 FS (2026-10-09)

Scope: functional validation, not bandwidth/latency calibration. Two ARM64
gem5 guests use `atomic_fast` (AtomicSimpleCPU, 3 GHz, no caches or memory
stalls), official provider/driver, two configured UB ports per endpoint,
standalone UDMA and ns-3-UB, synchronization disabled. The launch summary's
cache-size configuration is not evidence that atomic_fast enables caches.

## Boot compatibility repair and reusable checkpoint

`start-ready` rejected the previous snapshots: they lacked current SQ-prefetch,
completion-event-credit and UDMA state-format-v3 parameters. No old snapshot
was relabelled or forced into use.

The first cold boot exposed a firmware incompatibility, not a slow URMA test.
Two samples had identical `simInsts=3145147`, while committed instructions
increased from243889586 to330965940, primarily fetching from boot ROM. gem5's
remote debugger reported PC0x200, CPSR0x3cd (EL3), and LR0xffff800081b597c4.
The LR maps to `__primary_switched`, immediately after `finalise_el2`.
The current CPU advertises SME, but the downloaded ARM64 bootloader set
CPTR_EL3 to0x100 without ESM(bit12). The pinned upstream boot.S explicitly
sets ESM. Rebuilding upstream firmware removed the early-boot loop and both
guests reached Linux/init and loaded their official UB modules unchanged.

`build_gem5.sh` now compiles/installs ARM64 firmware from the same pinned gem5
source. `fetch-sources.sh` preserves existing firmware when filling missing
legacy bundle resources. The failed run's stats/config/log are retained in
`experiments/gem5-functional-current-20261009/boot-failure/`.

The coordinated checkpoint is `shell-ready-functional-v3-2port`. Both gem5
architectural snapshots and external UDMA state files were saved. It was then
actually restored before any reported perftest regression.

```sh
./lab start-ready --profile fast --provider official \
  --network-backend modular-ns3ub --no-sync --ub-port-count 2
./lab sync
UBSIM_RMA_TIMEOUT=120 ./lab rma-regression /workspace/ubsim-gem5-lab/experiments/new-gem5-full
# Additional matrices in the same restored guest lifetime:
UBSIM_RMA_SUITE=outstanding512 UBSIM_RMA_TIMEOUT=120 ./lab rma-regression /workspace/ubsim-gem5-lab/experiments/new-gem5-depth512
UBSIM_RMA_SUITE=multi UBSIM_RMA_JETTYS=4 UBSIM_RMA_TIMEOUT=120 ./lab rma-regression /workspace/ubsim-gem5-lab/experiments/new-gem5-multi
```

Docker output paths above are container paths. `./lab sync` configures/checks
OOB and the architected timer; with this no-sync launch it does not enable
conservative virtual-time synchronization. The regression wrapper now forwards
suite/Jetty settings and fails fast instead of continuing on potentially
wedged guests.

## Wall-clock costs

| Stage | Wall time | Outcome |
| --- | ---: | --- |
| Linux UDMA rebuild + tests |14.203 s|12/12 CTest passed |
| gem5 parallel link |146.334 s|OOM; retried with one build job |
| gem5 single-job retry |101.280 s|built and executable |
| upstream ARM64 bootloader build |0.051 s|built |
| repaired cold start + OOB/timer readiness |207.205 s|both guests ready |
| coordinated checkpoint save |16.215 s|saved |
| checkpoint restore + OOB readiness |12.927 s|both restored guests ready |

Linux GCC also exposed references bound to packed wire-header fields in the
network test peer. Value temporaries/assignments fix that portability issue
without changing the wire protocol. The switch's two CTests passed in0.29 s;
current native CTP order and READ-response-loss process contracts passed in
0.411/0.389 s. The old raw-frame ns-3 process smoke test still fails because
the current native adapter requires a UDMA WQE envelope; it is not counted as
a passing network test. The two ns-3 rebuild attempts took155.03/85.54 s before
the helper's packed-field compilation issues were corrected.

## Full-system results

Evidence root: `experiments/gem5-functional-current-20261009/`.

| Suite | Accepted | Wall seconds | Evidence |
| --- | ---: | ---: | --- |
| Official SEND/READ/WRITE baseline, large RMA, full WRITE scan |12/12|141.786|`full-fresh/`|
| SEND/READ/WRITE, depth512, batch/CQ1/16/512 |9/9|192.163|`depth512/`|
| Four Jettys, SEND/READ/WRITE at128/4096B |6/6|111.524|`multi4/`|

Total accepted-suite time445.473 s (27 cases). This excludes the initial
134.174 s transcript-contaminated attempt. All39 command pairs (initial12,
fresh12, depth9, multi6) ran in one restored lifetime. No further cold boot.

All cases execute on the same pair restored from the new checkpoint. The WRITE
scan checks exactly20 rows per endpoint (2B through1MiB), each with64 iterations.
Independent review confirmed exactly one perftest configuration and one unique
zero exit marker per endpoint in each fresh transcript. The official teardown
still logs `ubcore_find_remove_ex_tp_info: Failed to find ex_tp_info`; zero
command status must not be described as an entirely error-message-free log.

Final stopped-run counters are retained in `final-device/`:

| Counter | node0 | node1 |
| --- | ---: | ---: |
| SQ WQEs / completions |33676 /33676|34012 /34012|
| SQ consumer wraps |136|136|
| Actual pending-RMA peak |512|375|
| Port0 / port1 TX frames |16594 /127834|16674 /128074|
| Port0 / port1 RX frames |93712 /104328|93728 /103992|

Counters aggregate all39 command pairs, not just the final27. Different
endpoint totals arise from the asymmetric latency cases. Both endpoints end
quiescent with zero pending host DMA, RMA, READ DMA queues, remote services,
completion notifications, busy receive/event engines and deferred destroys.
SQ decode/depth rejects, UBASE errors, CEQ consumer rejects and notification
failures are zero. `unknown_queue_writes=690` at last offset0x200000 on each
endpoint remains a diagnostic limitation; do not label all counters error-free.
Network counters show67688 native WQEs submitted/completed, zero failed WQEs,
target DMA errors, pending target DMA, trace-size mismatches and runtime drops.
Port frame counts include protocol envelopes and READ request/response
resegmentation; they are not physical packet counts or a byte-integrity oracle.

The run was stopped normally to collect final counters; its reusable ready
checkpoint remains available using the command above.

## Acceptance boundaries

The first full suite took134.174 s. All12 command pairs returned zero, but its
scan validator correctly rejected a contaminated node1 transcript: gem5
Terminal replayed historical output *after* command injection, beyond the
old pre-send byte cutoff. The original evidence is retained under `full/`;
that run is not reported as12/12 accepted. The UART collector was fixed
to delimit the actual command with a unique, whole-line BEGIN marker, in
addition to the existing unique exit marker and following shell prompt.
Its tests cover delayed historical output, echoed fake markers, split network
reads and nonzero exits. All125 Python tool tests passed in4.503 s.

Official perftest success does not validate every payload byte, guard region
or completion identity. The earlier QEMU integrity results are not substituted
for gem5-specific data-integrity evidence. Depth512 means configured capacity
unless device high-water counters independently demonstrate actual occupancy.
Four Jettys use independent JFRs, not simultaneous shared-JFR interleavings.
Two configured ports alone do not establish actual use or isolation. Full-JFC
backpressure/recovery remains a separate unaccepted gap. No-sync bandwidth
columns are not used as performance conclusions.
