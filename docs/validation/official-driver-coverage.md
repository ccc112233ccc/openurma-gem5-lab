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
| UDMA JFC | `udma_jfc.c`, command header | Create 0x24, modify 0x25, destroy 0x27; CQE/moderation tests | Partial modify masks; new 16-cycle destroy/recreate test exercises CSQ wrap |
| UDMA JFR | `udma_jfr.c`, command header | Create 0x54, destroy 0x57; SEND receive DMA tests | Full modify/query and in-flight teardown not verified |
| AEQ / CEQ | command header and UBASE event setup | Create 0x34/0x44; mailbox/completion event DMA | **Single modeled AEQ/CEQ**, not per-ID multi-queue hardware. CEQ destroy 0x47 added this stage |
| CtrlQ QoS/SEID | `udma_ctrlq*`, UBASE control envelope | Services 4:1/2, 2:1/0x15 in `BuildCtrlqResponse` | Template responses; SEID query is not proof of complete security-entity behavior |
| CtrlQ TP | `udma_ctrlq_tp.h/.c` | GET_LIST 0x21, ACTIVE 0x22, DEACTIVE 0x23; routing state/native activation test | REMOVE/CHECK/SET_ATTR/GET_ATTR not implemented; deactivate does not free ID |
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

## Next priority gaps

1. Enumerate every UBASE/mailbox/CtrlQ command into explicit implemented,
   accepted-no-op or rejected categories; remove false-success paths safely.
2. Return driver-visible errors and advance CSQ on malformed commands.
   `FailUbase()` currently increments a counter and releases busy state, but
   does not complete the descriptor: a driver can still time out.
3. Validate short CtrlQ input/output buffers before success. Some opcode
   branches currently fall through with a success-shaped empty response.
4. Model per-ID event queues and safe in-flight teardown/reuse. The singleton
   CEQ fix does not solve deferred-DMA generation checks or multiple CEQs.
5. Add TP removal/reuse/exhaustion and attribute operations from the official
   definitions; then verify multi-Jetty/multi-Entity state isolation.
6. Run bounded full-system driver lifecycle tests from a compatible ready
   checkpoint after native contracts pass. Do not infer those results here.

```sh
cmake --build artifacts/udma-model-build -j8
ctest --test-dir artifacts/udma-model-build --output-on-failure
```
