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

## Coverage matrix

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

### Next autonomous acceptance priorities

1. Track remote READ/WRITE service DMA in checkpoint quiescence and resource
   lifetime; review found these can currently be omitted from `IsQuiescent()`.
2. Validate READ response interval coverage, not just TA uniqueness and total
   bytes: distinct TAs with overlapping offsets must not falsely complete.
3. Define cancellation/reset with transport quiescence for missing callbacks
   or ACKs. Never time out successfully while a DMA can still land.
4. Expand complete driver unload/reload, multi-Entity/multiport and error-path
   full-system evidence after those native contracts pass.
5. Continue removing completion-only operations by official caller semantics;
   explicit unsupported errors are not equivalent to implemented features.

The thread has an hourly continuation automation (`ubsim`) to advance this
list with separate implementation/acceptance agents, save local milestones,
and report meaningful results or decisions. This is a continuation mechanism,
not a declaration that item 2 is complete.

## Next priority gaps

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
