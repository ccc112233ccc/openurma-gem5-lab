# Official driver / hardware-model coverage audit

Date: 2026-10-08. This is a first source-backed inventory, not a statement that
all UB drivers or all entry points are implemented. Native model tests use
driver-shaped descriptors; they do **not** execute the Linux driver itself.
Full-system evidence is separately recorded in the existing RMA documents.

Sources inspected:

- OLK: `oe66`, revision `5078a3a23a1e1825ec136485173ec98668cdd640`.
- UMDK: `sources/umdk`, revision `8f272493e4138cd52cfb3ce11064a07c8d1be49f`.
- Device: `components/udma-model/src/udma_model.cc`.
- Native regression: `components/udma-model/tests/model_test.cc`.

## Current item 2 scope: performance paths (2026-10-08)

The user's revised scope is authoritative: implement and validate the official
driver paths needed for performance experiments, not every UB feature. The
historical inventory below remains evidence, not an all-command completion gate.

Acceptance priorities:

1. Official provider/driver SEND, READ and WRITE latency/bandwidth paths in the
   full system, including repeated runs, supported size sweeps, 4 KiB/64 KiB/
   1 MiB RMA fragmentation and SEND's supported size limit.
2. Performance-critical SQ/JFR/JFC, SGE/DMA and completion correctness: ring
   wrap, batching, CQ moderation, backpressure, shared resources and outstanding
   up to 512. Resource reuse must permit repeated experiments without hangs or
   stale data/completions.
3. Virtual-time and measurement correctness, then parameter sensitivity to
   link bandwidth/latency, DMA and outstanding. Functional no-sync results do
   not establish timing accuracy or bandwidth saturation. Do not fit constants
   to unrelated physical-system measurements.
4. Multi-Jetty/multiport/CTP routing and relevant congestion behavior for the
   selected performance workloads; implement where the hardware responsibility
   lies, leaving official driver actions to the driver.
5. Reproducible bounded regressions and profiling, recording configuration,
   build/test/simulation wall time and remaining limitations. Independent review
   must distinguish native contracts from actual full-system driver evidence.

Deferred unless they block these experiments: exhaustive management/diagnostic
commands, unrelated UB modules, hotplug, general hot-reset/disconnect recovery
and exhaustive driver unload/reload. Completion-only command counts are not a
completion target. Existing safety guards remain; data corruption, stalls or
misleading performance results on an in-scope path still require fixes.

## Stage3 started: synchronized bandwidth (2026-10-08)

Latest error-path stage: [CTP Remote Abort](ctp-remote-abort-20261009.md)
converts validated target READ/WRITE DMA errors into protocol errors instead
of crashing on an unsupported completion. Final native contracts10/10 and
official-driver no-sync cases12/12 pass; error replay does not reexecute DMA,
and wrong-peer responses cannot terminate an unrelated source WQE. This is
not closure of the synchronized1 MiB throughput/retry-lifetime gate.
The synchronized repeat now returns official error completions instead of
crashing the adapter:128 native WQEs,12 successes/116 failures and212659 retries;
no valid bandwidth result. Command85.937 s, total133.978 s. Target drain was
still incomplete at the runner's stop snapshot. Next repair is starting retry
timers at actual port transmission rather than local enqueue, with queued
attempt invalidation; see the linked acceptance record for concrete tests.

Latest follow-up: [full CTP transaction headers](read-full-ta-20261008.md)
repair exact small/non-power-of-two lengths and per-segment remote addresses.
All12 no-sync official-driver regression cases pass, including1 MiB RMA.
The first synchronized attempt stalled at32 B; QEMU profiling identified a
separate pending-IRQ/MMIO retry loop. An independent CPU device-wait state now
repairs this reproduced failure. All15 consecutive sizes (2–4096 B plus
65/129/4097 B) pass in223.839 s, with245760/245760 WQEs,120 ring wraps,
zero rejects/network failures/retransmissions and both devices quiescent.
The4096 B result is963.98 MiB/s (1.010806 GB/s). Actual CPU-gate counterexample
tests also pass. These results do not claim every synchronization failure is
eliminated; the separate synchronized1 MiB WRITE lifetime failure and throughput
bottleneck remain open.

Latest fix: QEMU's synchronized empty-input poll now services due output SYNC
nonblockingly, so a same-tick sync timer cannot be starved by the polling
callback. The first fixed full-system run completed all seven64–4096 B READ
cases consecutively, with114688/114688 SQ completions, zero model/network
errors or retries, and both devices quiescent. The formerly blocked1024 B
case took6.874 s instead of exceeding180 s. Total wall time173.682 s.
Both QEMU logs confirm the new branch executed. See the READ record below
for independent review and repeat-run evidence; this does not close2 B
compact-length handling or the separate1 MiB CTP lifetime issue.
A second full run passed all seven cases in186.962 s, again114688/114688
completions, zero errors/retries, and both devices quiescent. Independent review
accepted closure of this specific continuous-READ synchronization blocker.

Historical failure evidence (before the poll-SYNC fix):

Latest range request: [2–4096 B READ results](read-2b-4096b-20261008.md).
Seven sizes64–4096 B have valid single-direction readings (0.015792–1.010911
GB/s) in two sessions.2 B failed and cleanup stalled;4–32 B were not run.
The first64–4096 session ended without a1024 B result despite ns-3 completing all81920
requests; UDMA retired81509 and could not checkpoint. A fresh session passed
1024/2048/4096. Completion-tail progress across repeated workloads is therefore
an additional open timing/reliability gate; do not label the grouped results
as a single successful continuous scan or infer a CTP sequence-wrap cause.
Independent review found this attempt ended at148.321 s before its180 s
deadline, with SIGTERM/console EOF; the old UART helper mislabeled EOF as a
timeout. The411 unretired requests are a shutdown snapshot, not proof of a
permanent deadlock. UART EOF classification is now distinct, and shutdown
diagnostics expose SQ readiness/publication, active JFC queues and pending host
DMA. These observations do not change model scheduling or waive timing gates.
The diagnostic rerun genuinely timed out at1024 B after180.211 s (330.535 s
total). It completed64–512 B, then ended with866 ready SQ entries awaiting CQ
publication, one host DMA read, no pending RMA and an almost empty CQ.
See the linked READ record for exact counters. Narrow the investigation to
host-side completion publication and synchronized progress; neither a lost
DMA nor permanent deadlock is established yet. SIGUSR1 now supports live
read-only progress samples on subsequent launches.

Latest requested READ evidence:
[single-direction scan and aligned control](requested-read-scan-20261008.md).
Actual capabilities limit R/T/C to1024/1024/4096. With n16384, batch256 and
CQ moderation1, `-a23` fails immediately at2 B: compact wire length decodes64 B
while the initiator expects2 B, causing repeated response rejection. The4 KiB
control passes at964.05 MiB/s (1.01088 GB/s),16384 completions, zero retries.
Resolve logical-vs-wire READ length with protocol-backed handling and small/
non-aligned counterexamples; do not hide it by increasing RTO. This is an
additional timing acceptance blocker, separate from the1 MiB lifetime issue.

See [timing results](qemu-timing-bandwidth-20261008.md). With the current
pipeline and required QEMU/UDMA/ns-3 synchronization, WRITE bandwidth at64 KiB
was 53120.86 MiB/s bidirectional at400G/port and 32905.46 at200G/port. The4 KiB
case was nearly unchanged. These are first short measurements, not saturation
or repeatability acceptance. The topology has two ports per endpoint; do not
infer single-port utilization from the aggregate output.

**Next timing-path blocker:** [CTP terminal/DMA record](ctp-terminal-drain-20261008.md).
The synchronized1 MiB failure is now confirmed as retry exhaustion followed
16.39628 us later by target execution referencing the deleted source task.
A related accepted-target-DMA lifetime branch is repaired: pending work owns
its callback/range/identity independently and strictly validates replies.
Native delayed READ/WRITE pass with one failure, no successful completion,
one drained target callback and no pending target DMA; malformed completion
is rejected. All12 official no-sync regressions pass again in28.310 s.
Follow-up [independent request/late response acceptance](ctp-request-context-20261009.md)
removes target dependence on the live source map and gates idle fast-forward
on the real scheduler queue. Native late-first, late READ/WRITE and malformed
reply tests pass, including actual late response receipt. It also fixes a null
context in late ACK parsing without creating/advancing a replacement context.
All12 official no-sync regressions pass in29.452 s. The synchronized1 MiB
repeat advances beyond the former failure but encounters target translation
errors and unsupported `RmaError` completion handling. That remains open,
together with retry timing and response-cache anti-replay lifetime. Do not
mask these by increasing RTO or ignoring unknown tasks; no-sync passing is
not timing acceptance.

## Multi-WQE pipeline acceptance (2026-10-08)

Implemented bounded per-Jetty SQ issue separate from ordered retirement. Local
descriptor/payload fetching remains sequential, but READ response/WRITE ACK
waiting no longer blocks issue of independent later WQEs. Hardware WQEBB depth
bounds the unretired window. CQEs retire in issue order, including errors and
non-signaled operations; strong placement/fence conservatively drain/block.
Runtime FIFO/cursors are not checkpointed and are discarded on destroy/restore;
the serialized QueueContext layout and checkpoint version are unchanged.

Independent descriptor-level tests cover reverse WRITE ACKs, reversed READ DMA
completion with payload checks, multi-block WQEs and three ring generations,
strong/fence gates, error/success ordering and destroy/checkpoint ownership.
True CQ DMA failure remains fail-closed: no unsafe resource release, but no
general recovery claim. A CQE that was successfully published must retire even
if CEQ notification subsequently fails; notification failure is separately
counted. This fixes the previous possibility of replaying an already-sent WQE.

First native pass found two failures (8/10), not ignored: the existing CEQ test
now expects both already-published CQEs to retire; four white-box pending-RMA
fixtures now reserve the matching SQ entry. All original DMA/error assertions
remain. Final native build **4.68 s**, **10/10 tests in 4.54 s**; initial build
**5.41 s**. Eight Python profile/parser tests passed in **0.03 s**.

Full-system official provider/driver results on dual QEMU, no-sync:

| Gate | Result | Suite / wrapper wall seconds | Evidence directory |
| --- | --- | --- | --- |
| depth512, SEND/READ/WRITE × batch/CQ 1,16,512 | 9/9 | 25.943 / 28.24 | `experiments/perf-pipeline512-20261008/` |
| latency/bandwidth, 4 KiB/64 KiB/1 MiB RMA, 2 B–1 MiB scan | 12/12 | 28.532 / 30.81 | `experiments/perf-pipeline-full-20261008/` |

Launch commands took **0.64 / 0.62 s** (not shell-ready time). Both environments
were stopped after collection. Saved UDMA profiles for the isolated depth512
gate show **68 / 85** local RMA peak (formerly 1 / 1), **18432** decoded and
completed WQEs per endpoint, **18432** retired blocks, **18** hardware wraps,
and zero SQ depth/decode rejects or UBASE errors. `check_udma_profile.py`
automatically checks these gates, including peak >1; the old serialized logs
correctly fail that requirement. These are whole-run aggregate peaks, not
per-case concurrency measurements; actual peak512 is not claimed. Full-suite scan
checks all 20 expected rows on both endpoints, not just exit status. QEMU
regression now stops on the first failed case and records incomplete case count
instead of repeatedly waiting on possibly wedged guests.

Remaining priorities: full-system payload-integrity evidence; controlled
concurrency/window and multi-Jetty experiments; synchronized measurement and
configuration sensitivity. Functional no-sync wall-time changes are not modeled
hardware speedup. Local payload-DMA error recovery remains limited; do not
represent this change as implementing every failure/recovery path.

## Performance observation increment (2026-10-08, before pipeline)

Added process-lifetime counters `rma_pending_peak`, `sq_wqebbs_retired` and
`sq_consumer_wraps` to the UDMA profile. They observe existing behavior without
changing scheduling or checkpoint format. RMA peak is device-wide local
READ/WRITE requests, excluding SEND, remote service and CQE publication.
Wrap counts use actual hardware queue depth and retired WQEBBs, not WQE count.

The nine official depth512 cases passed with the new binary in **30.224 s**
(wrapper **32.51 s**, launcher **0.64 s**, initial build **5.25 s**). Both nodes:

| Counter | Value |
| --- | ---: |
| decoded WQEs / completions / retired WQEBBs | 18432 / 18432 / 18432 |
| hardware SQ consumer wraps | 18 |
| local RMA pending peak | 1 |
| SQ depth/decode rejects and UBASE errors | 0 |

Evidence: `experiments/perf-counters-20261008/`, including saved per-node UDMA
profiles and UART reports. Official `udma_u_jfs.c` allocates two WQEBBs per
configured depth entry for this max-SGE/inline configuration, so `-T512`
allocates 1024 hardware blocks; these one-block WQEs cause 18, not 36, wraps.

Independent review confirmed that `ProcessSq` keeps each Jetty busy until
`CompleteSq` finishes publication. READ waits for response DMA, WRITE for ACK;
the peak of one is therefore a model serialization bottleneck, not hardware
evidence that queue depth512 provides concurrency. Next implementation priority
is decoupled bounded SQ issue and ordered retirement, preserving CQE indices,
owner handling, error propagation and destroy/checkpoint quiescence. Re-run
counter-backed depth512 and fragmentation tests after that change.

Independent tests add a four-block ring with multi-block WQEs crossing the
boundary, an exact-boundary completion, and two Jettys holding WRITE ACKs to
prove peak2 is observed and retained after drain. Final build **2.11 s**;
native **9/9 PASS in 0.63 s**. Python **6/6 PASS in 0.05 s** includes new scan
missing/duplicate/wrong-iteration rejection. Both endpoints of the saved
2 B–1 MiB scan were re-parsed successfully; that was transcript validation,
not a fresh scan execution. Payload-integrity coverage remains open.

## Coverage matrix

### Performance-scope increment: explicit depth512 (2026-10-08)

Nine dual-QEMU no-sync full-system cases passed: official perftest SEND/WRITE/
READ bidirectional bandwidth at 4096 B, `-T512 -R512 -n2048`, each with post-list/
CQ-moderation pairs 1/1, 16/16 and 512/512. Each guest printed JFS depth512 and
one complete matching result row; all guest exit statuses were zero. Suite wall
time was **29.753 s**, wrapper **32.03 s**, launch command **0.64 s** (launch
return, not shell-ready duration). No model rebuild was required for this
regression-only increment. Evidence:
`experiments/perf-depth512-20261008-validated/report.json` and per-case UART logs.

Independent reviewer accepted the narrower depth512 functional claim and ran
the five parser/command tests: **5/5**, **0.04 s** (main run **0.06 s**). Negative
tests reject missing UARTs, clamped depth and incomplete result rows. Reports
now expose validation errors separately from guest exit status, including CSV.

This does not establish actual 512-request hardware concurrency, measured ring
wrap counts, payload correctness or synchronized bandwidth. Historical `out16`
labels described batch size, not queue depth; the regression guide corrects the
earlier concurrency/wrap claims. Next performance evidence should add in-flight
and WQEBB producer/consumer counters, full-scan output completeness and payload
verification before interpreting bandwidth scaling.

Here **partial** means a real implementation with explicit limits;
**completion-only** means command success is not evidence of device behavior.
Paths below are repository-relative.

| Area | Official source / entry | Device implementation and evidence | Current boundary |
|---|---|---|---|
| Firmware resources and discovery | `oe66/drivers/ub/ubfi`, `ubus/enum.c`, `ubus/ubus_entity.c` | `UbiosConfigRead`, `BuildUbiosResponse`; native resource/message tests | Partial: configured topology and response templates, not arbitrary discovery/hotplug |
| Vendor bus messaging | `oe66/drivers/ub/ubus/vendor/hisilicon` | UBIOS SQ/RQ/CQ DMA; token response test | Partial: selected enumeration/private messages only |
| UBASE CSQ/CRQ | `oe66/drivers/ub/ubase/ubase_cmd.c` | `HandleUbaseDescriptor`, `FinishUbaseDescriptor`, `EmitCtrlqResponse`; MMIO/DMA/IRQ tests | Queue ownership and wrapping covered; not all command meanings |
| Capability/link queries | `oe66/include/ub/ubase/ubase_comm_cmd.h` | `BuildUbaseResponse`: 0x0030, 0x0002, 0x6200; inline 0x0001/0x7001 | Partial: fixed capability data; 0x6200 currently reports fixed 400000 |
| Other UBASE commands | same command header | Default branch completes with empty response | **Completion-only**; must enumerate and classify before claiming support |
| UMMU translation | `oe66/drivers/iommu/hisilicon`, `deps/ummu` | token lookup, page walk, IOTLB; native translated queues/payload and invalid-token tests | Partial: current ARM64 table formats; not complete UMMU fault reporting/permissions coverage |
| UBCORE / UBURMA | `oe66/drivers/ub/urma/ubcore`, `uburma` | Official guest software routes requests to provider/driver | Software execution is not independent proof for each ioctl |
| UDMA JFS/Jetty context | `oe66/drivers/ub/urma/hw/udma/udma_cmd.h`, `udma_jetty.c` | Mailbox create 0x04, query 0x06, destroy 0x07 | Create/DMA path covered; query is synthesized, full modify/flush/in-flight teardown not verified |
| UDMA JFC | `udma_jfc.c`, command header | Create 0x24, modify 0x25, query 0x26, destroy 0x27; serialized CQE publication and drain | Partial modify masks; query preserves post-destroy state for official destroy-done polling |
| UDMA JFR | `udma_jfr.c`, command header | Create 0x54, destroy 0x57; SEND receive DMA tests | Full modify/query and in-flight teardown not verified |
| AEQ / CEQ | command header and UBASE event setup | One official AEQ (ID 0), per-ID CEQs; JFC CEQN routing and context irq_num | Serialized event publication, independent CEQ destruction; queue-full/consumer-doorbell behavior still incomplete |
| CtrlQ QoS/SEID | `udma_ctrlq*`, UBASE control envelope | Services 4:1/2, 2:1/0x15 in `BuildCtrlqResponse` | Template responses; SEID query is not proof of complete security-entity behavior |
| CtrlQ TP | `udma_ctrlq_tp.h/.c` | GET_LIST 0x21, ACTIVE 0x22, DEACTIVE 0x23, REMOVE 0x13; exhaustion/reclaim/reuse tests | REMOVE supports inactive routes with no pending RMA; CHECK/SET_ATTR/GET_ATTR not implemented |
| Official provider SEND / SEND_IMM / WRITE / READ | `sources/umdk/src/urma/hw/udma/udma_u_jfs.c` | `SqWqe`, `ProcessSq`, receive paths; native and historical full-system RMA tests | Implemented subset; SQ opcodes accepted are 0, 1, 3, 6 |
| Atomic / other provider opcodes | same provider | Rejected by current SQ opcode validation | Not implemented in UDMA, even if ns-3 supports corresponding transactions |
| Multiport selection | `udma_ctrlq_tp.c` and provider TP handles | `tp_routes_`, `SetLinkState`; native fallback test | Functional route/port fallback, not exhaustive multi-Jetty/multiport matrix |
| Aggregation / other official UB modules | `ubagg`, `cdma`, `obmm`, `sentry` | `ubagg` packaged by official initramfs; others require individual audit | Packaging/source presence is not functional verification |

The name **UBSE** must be tied to concrete protocol operations and source
symbols before assigning support. This checkout's UB driver directory does
not contain a standalone module named `ubse`; SEID query responses alone
cannot establish that every intended UBSE function is supported.

## Lab-owned software boundary

`official-udma/build_initramfs.sh` also packages the lab-owned
`ubsim_ub_v2m.ko` bridge. It must not be counted as an official UB driver.
Virtual-time instrumentation and simulator adapters are likewise lab code.
The objective remains official provider/driver behavior on simulated hardware;
the current system should not be described as having zero guest-side glue.

## First implementation result: resource lifetime

The model already erased JFS/JFC/JFR contexts, but CEQ destruction silently
completed without revoking its guest DMA address. Mailbox opcode 0x47 now
clears the singleton CEQ address, depth and producer.

New tests use the actual CSQ MMIO doorbell and descriptor DMA, not private
helper calls:

- CEQ destroy, recreate, destroy: enabled state tracks each transition.
- JFC ID 7: sixteen destroy/recreate cycles, checking resource counts and
  command completion ownership/status across ring wrap.
- After CEQ destruction, submit two real SEND WQEs with CQ completion and
  moderation count 2. The first completes; the second notification fails
  because no CEQ exists. CQ DMA is observed, and **no DMA touches the old
  CEQ buffer**. This tests revocation, not successful use of a removed queue.

Initial model build/test took 3.17 s / 0.57 s wall time. The later focused
incremental build/test took 1.14 s / 0.39 s. No guest cold boot was performed.
Final standalone-device rebuild took **3.28 s**; its process contract and
model regression both passed in **1.84 s** total wall time. Existing running
guests were not restarted or claimed as revalidated.

## Second implementation result: control errors and TP reuse

Commands whose descriptors can be read and completed now return Linux errno
in CSQ `ret`, return ownership and advance Head on semantic failure. This
matches `ubase_get_cmd_result()` in `ubase_cmd.c`; the driver returns `-ret`.
CtrlQ send failure exits before waiting for CRQ in `ubase_ctrlq.c`, so rejected
requests deliberately produce no success-shaped CRQ response.

Native MMIO/DMA tests cover:

| Case | Linux errno / expected behavior |
|---|---|
| Short CtrlQ input/output; null or malformed context | EINVAL (22) |
| Unsupported CtrlQ opcode | EOPNOTSUPP (95) |
| Context DMA read failure | EIO (5) |
| Duplicate JFS/JFC/JFR creation (JFC exercised) | EEXIST (17) |
| Unknown TP activation/deactivation/removal | ENOENT (2) |
| All ports down / activating a down-port TP | ENETDOWN (100) |
| TP table full | ENOSPC (28); existing entries preserved |
| Removing an active TP | EBUSY (16) |
| Oversized CRQ output or short UBASE response capacity | EMSGSIZE (90) |
| Descriptor chain exceeds submitted entries | EINVAL (22); consume only bad header |

Tests fill all **1,023 model TP slots**, reject the next allocation, deactivate
and remove a route, reuse its freed ID, remove all routes, then allocate again.
This is the current model capacity, not a claim about hardware limits.
Removal conservatively rejects whenever *any* RMA is pending; this is not
complete per-TP live flush/teardown behavior. A later asynchronous-DMA test
checks that command callbacks survive deferred completion.

Official `GET_BD_NUM` queries now return descriptor requirements for modeled
response buffers, preventing silent truncation. Three command-path recursive
callback chains use weak self-references to eliminate their ownership cycles.
This does not assert that all asynchronous paths have been audited.

Unrecoverable queue faults remain distinct: an unreadable first descriptor,
invalid queue base, or unwritable completion can still reach `FailUbase()`.
No safe driver-visible completion is claimed when the completion buffer itself
cannot be accessed. Generic UBASE and mailbox defaults still include successful
empty responses; the error improvements do **not** remove all false-success
paths.

Reproducible source inventory:

```sh
python3 tools/audit_command_coverage.py
python3 tools/audit_command_coverage.py --json
```

The tool reads selected pinned UBASE/UDMA enums, including both mailbox headers,
and assigns conservative, manually reviewed categories. It counts aliases as
symbols, not unique operations. It is neither exhaustive across all UB modules
nor dynamic verification. Results at that earlier stage: UBASE 86 symbols (78 completion-only,
6 partial, 2 dispatch); mailbox 74 (52 completion-only, 22 partial); CtrlQ TP 8
(4 partial, 4 rejected); CtrlQ device 6 (1 partial, 5 rejected).

Standalone device incremental build took **3.49 s**, and its process-contract
and model tests passed in **1.40 s**. No guest cold boot or full-system driver
revalidation was performed for this stage.
Final no-change rerun took **0.10 s** to check build targets and **0.17 s**
for the same two tests; assertions remain enabled via `-UNDEBUG`.

## Third implementation result: event queue lifetime guards (historical)

AEQ destroy (0x37) now clears its DMA address, depth and producer instead of
silently succeeding. AEQ outstanding operations are counted through DMA and
interrupt completion. AEQ/CEQ create or destroy returns EBUSY (16) while its
event path is in flight; CEQ also checks pending events. This prevents these
commands from replacing model queue state underneath an old callback.
Checkpoint save treats outstanding AEQ events as non-quiescent; restore now
rejects non-quiescent state before consuming input.

Tests hold actual event DMA callbacks while subsequent CSQ commands execute.
Both queues reject destroy/replacement while busy and permit destruction after
completion. AEQ recreation at a different address emits the expected event
without new writes to the old buffer. Busy checkpoint save/restore is rejected.
Native build/test took **1.45 s / 0.52 s** before the final restore guard rerun.
The final build/test including that guard passed in **3.57 s / 1.15 s**.

Important limitation: `ubase_destroy_eq()` in
`oe66/drivers/ub/ubase/ubase_eq.c` frees the coherent buffer even when the
mailbox destroy reports failure. Therefore the EBUSY policy is only a model
state guard, **not complete official-driver teardown safety**. A future drain
path must defer successful destruction until in-flight DMA/interrupts finish,
with bounded failure handling. Per-ID queues, parallel AEQ producer allocation
and queue-full semantics also remain unimplemented. No guest revalidation is
claimed for these native tests.

## Fourth implementation result: asynchronous EQ destruction

The previous EBUSY-on-destroy policy is replaced by a drain. If an AEQ/CEQ
event is outstanding, the model retains the CSQ descriptor without returning
ownership or advancing Head. Completion callbacks resume destruction only
after accepted event DMA and MSI operations finish. It then clears queue state,
completes the command successfully and resumes subsequent CSQ descriptors.
Create/replacement while busy continues to return EBUSY.

CEQ notifications already queued are drained; newly arriving notifications
after destroy acceptance fail instead of extending the queue lifetime. This
does not automatically drain every JFC/SQ/RMA producer: software must stop
producers before dismantling the event queue, and producer teardown remains
separate work.

Native tests independently hold the event DMA and then its Type-1 MSI write.
At both boundaries, destroy and a following NOTIFY_DRV_CAPS remain uncompleted.
After MSI completion both finish successfully in order, the queue is disabled,
and the existing AEQ new-buffer recreation/no-old-buffer-write test passes.
This exercises the official polling-destroy command shape without modifying
the driver, but does not execute the official kernel itself.

First build/test: **3.69 s / 1.55 s**. Final expanded DMA+MSI test build/run:
**1.47 s / 0.63 s**, both process-contract and model tests passed. No guest
restart was performed.

Failure boundary: callbacks must eventually complete. There is no transport
cancellation or lost-callback watchdog yet. Returning a timeout while a write
can still occur would recreate the freed-buffer hazard; that fault requires
transport quiescence/reset, not an invented successful completion. Therefore
full guest unload safety and fault recovery remain unverified.

## Fifth implementation: independent review and integrated lifecycle work

Implementation and independent acceptance now run together. Review exposed
shared-producer races and early DMA-error completion that single-queue tests
did not exercise. The working acceptance gates are:

| Gate | Implemented contract | Evidence / remaining boundary |
|---|---|---|
| Multiple CEQs | JFC DW2 CEQN selects per-ID ring; EQ DW6 irq_num selects MSI | Native SEND routing; destroying A does not wait for delayed B, success/failure branches |
| AEQ concurrency | Event publication is serialized through DMA and MSI | Two event-enabled mailboxes retain distinct sequences and ring slots; only official AEQ ID 0 supported |
| Shared JFC | Per-ID CQE publication queue owns producer until final callback | Independent delayed-DMA tests with distinct local IDs, success/failure and destroy |
| Timer notifications | JFC destruction also drains its pending CEQ events | Independent delayed moderation notification test; prevents stale event on reused JFC ID |
| JFS/JFR lifetime | Closing stops new work; started SQ/RX operations drain before erase | Native delayed SQ read, SEND completion, WRITE ACK, RX payload/CQE and ID reuse; not complete WQE flush semantics |
| Token DMA errors | Stop new fragments, wait for accepted translation/DMA before final error | 16 read/write scenarios, including failure ordering and 16 MiB synchronous callbacks without recursive stack growth |
| JFC query | Preserve official context and return modeled producer/write progress; keep destroyed snapshot | Independent live/in-flight/post-destroy query, checkpoint, ID reuse and error tests passed |
| Command audit | Distinct opcode counts, aliases preserved; declaration fingerprints fail closed on changes | Python unit tests; source review is explicitly not execution evidence |
| Guest dataplane | Dual native QEMU, official provider/driver, no-sync | Final integrated binary: 12/12 RMA cases passed, suite 29.203 s (wrapper 31.48 s) |

Two unsupported diagnostic requests (GET_CQE_AUX_INFO 0x4213 and
GET_AE_AUX_INFO 0x4214) now return EOPNOTSUPP instead of empty success. Official
`udma_ctl.c` callers check command failure before consuming the response.

Checkpoint state is now **version 2**, including per-ID CEQ/JFC mappings and
JFC query snapshots. Version 1 is rejected rather than misinterpreted. Existing
gem5 ready checkpoints must pass complete compatibility checks; a coordinated
replacement must be made once if none is compatible. Native QEMU functional
verification does not restore old model checkpoints.

The first QEMU attempt reached both guest shells, but ending its parent tool
session reaped the background processes. The measured dataplane run kept launch
and regression in one supervised session. Docker was unavailable; no Docker
restart or gem5 cold boot was required.

The following are **not** implied by these tests: arbitrary UB-module support,
full guest driver unload/reload, lost-callback/transport-reset recovery, hardware
performance calibration, or a full multi-Entity/multiport matrix.

Final coordinated clean build: **5.03 s**. All **6 CTests passed in 3.00 s**;
all **7 Python audit tests passed in 0.16 s**. Independent acceptance found no
new P1 in the implemented paths. The READ-error regression includes two
outstanding response DMAs: remote error or local DMA failure cannot return an
error CQE or finish resource destruction before the last write finishes.

Final QEMU evidence is under
`experiments/official-coverage-qemu-20261008-b/` (local generated artifacts):
`results.csv`, twelve UART transcripts and the model manifest. Cases cover
SEND latency/bandwidth, SQ wrap, 4 KiB SEND, READ/WRITE latency, bidirectional
4 KiB outstanding=16, 64 KiB/1 MiB fragmentation and a single-process
2 B–1 MiB inline64 WRITE bandwidth scan. All command return codes were zero;
UDMA logs contained no error/timeout entries. These are functional results.

The current distinct-opcode audit still reports **76 UBASE and 32 mailbox
completion-only commands**. `--check` now rejects changed enum declarations
until their namespace is reviewed; this is not a hardware-implementation check.

### Historical autonomous acceptance priorities (superseded)

The remote-service quiescence and response-interval priorities were closed by
the sixth implementation below. The following broader priorities are deferred
under the performance-path scope above unless an experiment needs them:

1. Define cancellation/reset with transport quiescence for missing callbacks
   or ACKs. Never time out successfully while a DMA can still land.
2. Expand complete driver unload/reload, multi-Entity/multiport and error-path
   full-system evidence after those native contracts pass.
3. Continue removing completion-only operations by official caller semantics;
   explicit unsupported errors are not equivalent to implemented features.

The thread has an hourly continuation automation (`ubsim`) to advance the
current performance-path scope with separate implementation/acceptance agents, save local milestones,
and report meaningful results or decisions. This is a continuation mechanism,
not a declaration that item 2 is complete.

## Sixth implementation: remote-service quiescence and byte coverage

Remote READ/WRITE now hold a service lease from valid request acceptance until
all token DMA and the final response/ACK/error-send callback finish.
`IsQuiescent`, checkpoint save/restore and time rebasing respect these leases.
Failed DMA sends RmaError rather than a success-shaped ACK; a failed network
send also releases exactly one lease. These one-sided operations address a
segment token and remote address, so no unsupported Jetty ownership is invented.
Malformed zero-length or wrapping-address requests are rejected before DMA.

READ responses reserve sorted, non-overlapping byte ranges **before** DMA,
in addition to reserving TA numbers. Distinct TAs cannot write the same range
twice or falsely complete while leaving a hole. Adjacent ranges, out-of-order
arrival and out-of-order DMA remain valid. Bounds checks use subtraction to
avoid overflow; a last valid byte equal to UINT64_MAX is accepted. WRITE ACKs
now enforce the same range uniqueness and overflow-safe accumulation.

Native tests exercise concurrent remote services; delayed DMA and network
completion; synchronous/asynchronous failure; multi-page failure drain;
checkpoint guards at each phase; identical/partial/containing overlaps;
duplicate TAs; malformed lengths; integer boundaries; successful reordered
coverage; and rejection of new valid fragments after a request has failed.
Independent acceptance found no P1 in these changes.

Clean build: **5.36 s**. Initial **7/7 CTests passed in 3.42 s**. Additional
highest-address/failed-request assertions rebuilt in **1.31 s** and passed
**7/7 in 0.81 s**. The production binary then passed the full dual-QEMU
official perftest matrix **12/12**, suite **28.999 s**, wrapper **31.28 s**.
Local evidence: `experiments/official-coverage-qemu-20261008-c/`.

Next recovery boundary: current SimBricks termination does not acknowledge
cancellation of already accepted host DMA. HostPort has pending callbacks but
no cancel/drained protocol; gem5 timing DMA may still complete later. First
implement fail-stop and coordinated host shutdown on loss, then define an
explicit drain/acknowledgement plus epoch protocol before any hot reset or
guest-memory reuse. Returning fake successful teardown is not an alternative.

Read-only transport review also identified concrete next regression targets:
`NetworkPort::PendingEmpty()` omits partial `fragments_`; CommitSync does not
validate its generation; `HostPort::HandleDmaCompletion` removes a pending
callback before validating completion type and does not fully check length;
abrupt process loss needs control-socket HUP detection, and the normal
SimBricks close path can spin if the outbound ring is full. These are findings,
not implemented fixes. Address them before claiming loss/reset robustness.

## Next priority gaps

### Transport validation increment (2026-10-08)

The next increment addresses malformed messages before attempting reconnect:

- Host DMA completions must match a live ID, requested direction, status,
  length and inbound payload capacity before pending ownership is released.
  Failed reads never copy payload. Malformed completions do not call the
  original completion callback or erase it before the device exits.
- Host PREPARE cannot overwrite an in-flight fence. Network COMMIT must match
  the generation and target of a successfully sent PREPARE; unsolicited,
  stale/future or duplicate COMMIT is a protocol error. Run also checks the
  host/network generation before rebasing time.
  A later host PREPARE must advance generation, matching both official adapter
  implementations' increment-on-request behavior.
- Network idle checks include partial incoming frames. Network payload length
  is bounded by the actual ring slot before allocation/copy. Recognized UDMA
  wire headers and non-CTP fragment bounds, final-fragment flags, offsets and
  stable metadata are validated. Untrusted transfer length no longer drives
  a large upfront reserve. Reassembly keys include source EID and sequence.
- These protocol violations terminate the device with exit status 4 and an
  explicit diagnostic, without checkpointing or entering the potentially
  blocking normal SimBricks Close path. This is **device-only fail-stop**:
  the host may still have DMA in flight and must be stopped/drained by the
  orchestrator. It is not hot reset, reconnect, or coordinated shutdown.
- Independent final review caught a separate shutdown-checkpoint path that
  checked only the model. `SaveDeviceState` now checks both adapters, partial
  frames and pending fences before opening/truncating the destination. Tests
  preserve a sentinel checkpoint while a half-frame/fence is pending, then
  permit saving after completion. A model-only quiescent result is insufficient.

The graceful/abrupt peer-loss path, bounded normal close, host stop/drain
coordination and epoch-based recovery remain separate work. Fragment validation
does not yet establish a global reassembly-memory budget or partial-frame
timeout policy.

Validation: clean build **5.20 s**. New host/network validation tests passed,
but the first complete run failed the old process-contract fixture: mock WRITE
completion omitted length. Actual gem5/QEMU both echo it. Correcting that
fixture (not weakening validation) rebuilt in **0.48 s**, then **9/9 passed
in 0.59 s**. Final monotonic-generation guard rebuilt in **1.33 s**, with
**9/9 passed in 1.98 s**. That binary passed **12/12** dual-QEMU official
perftest cases in **28.995 s** (wrapper **31.26 s**); local evidence is
`experiments/transport-validation-qemu-20261008-final/`. No-sync QEMU does not
exercise live lifecycle rebasing; lifecycle corruption checks have native
ring-level evidence only. No gem5 cold boot was used.
The subsequent shutdown-checkpoint guard changes were validated natively;
they do not change the dataplane and were not separately rerun on guests.
Final build/test with that guard: **1.11 s / 1.34 s**, **9/9 passed**.

### Remaining broader driver gaps (not current completion gates)

This inventory is retained for transparency. Only entries needed by the
performance-path acceptance priorities above are in the current work scope.

1. Enumerate every UBASE/mailbox/CtrlQ command into explicit implemented,
   accepted-no-op or rejected categories; remove false-success paths safely.
2. Expand driver-visible error coverage beyond the validated command branches;
   define recovery for unreadable queues and unwritable completions.
3. Audit remaining service-specific input fields, flags and semantics beyond
   the new minimum-length checks.
4. Complete event-queue consumer/fullness semantics and remaining in-flight
   teardown/reuse cases beyond the per-ID CEQ and serialized publication work.
5. Extend TP removal to safe per-TP in-flight teardown and add attribute
   operations; then verify multi-Jetty/multi-Entity state isolation.
6. Run bounded full-system driver lifecycle tests from a compatible ready
   checkpoint after native contracts pass. Do not infer those results here.

```sh
cmake --build artifacts/udma-model-build -j8
ctest --test-dir artifacts/udma-model-build --output-on-failure
```
