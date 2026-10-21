# Public evidence for URMA RMA ordering and WRITE payload fetch

Date: 2026-10-10. Scope: assess whether the present one-Jetty, one-in-flight
non-inline WRITE source-payload DMA is required by URMA/UB, and identify what
must remain a model assumption. This is an evidence review, not a measurement
of the Kunpeng 950 UDMA silicon.

## Evidence table

| Claim supported by a primary source | Source | Consequence for this model |
| --- | --- | --- |
| A Jetty has an SQ for WQEs and a JFC for completions; JFS depth is a configurable device capability. The official perftest exposes `-T` and documents a 1–15000 JFS-depth parameter range. | [openEuler URMA User Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20User%20Guide.ch.html), §§5.2.2, 6 (`jfs_depth`), perftest parameter table; [openEuler URMA API Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html), §§2.1.2, 3.3.1.4.1 | Queue depth supports many posted WQEs; it does **not** specify how many source payload DMA requests a particular NIC issues concurrently. Do not equate `-T 8192` with 8192 DMA slots. |
| WRITE fetches the local source data, sends it to the target, the target stores it, then sends TAACK. READ sends a request, target fetches data and returns it, initiator places it locally. | [openEuler URMA User Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20User%20Guide.ch.html), §5.2.3 | READ and WRITE have different device-memory paths. The guide describes **what** each operation does, but gives no one-per-Jetty DMA serialization rule or DMA latency. |
| The WQE independently exposes `place_order` (NO/RO/SO), `comp_order`, `fence`, `complete_enable`, and `inline_flag`. NO imposes no execution order; RO requests have no order guarantee among themselves; SO waits for preceding tagged RO/SO requests. `fence` concerns preceding READ and atomic requests. | [openEuler URMA API Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html), §§2.4.1.1.7–8, 3.6.1.3 | WQE issue, payload fetch, transaction placement, and CQE publication are distinct stages. Ordering semantics can be enforced after parallel fetch using sequence tracking/reorder buffering; they do not by themselves require one outstanding payload fetch. The detailed mechanism is an engineering inference, not a specified architecture. |
| `urma_write()` forbids the caller from modifying its source buffer until the request completes; successful remote placement is followed by a polled completion. `urma_read()` forbids changing its local destination buffer until completion. | [openEuler URMA API Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html), §§2.4.3.1–2 | Concurrently fetching several already-posted WRITE source buffers is compatible with the API's buffer-lifetime rule. An implementation must still associate each result with its WQE and delay completion until required remote success. |
| Normal data operations are nonblocking; a successful post only means the command entered a queue, and a hardware completion is later written to a JFC. Flush/error handling explicitly considers outstanding WRs. | [openEuler URMA User Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20User%20Guide.ch.html), §§5.2.4.1, 5.3.3 | A design must retain pending-WQE state through target ACK, CQE, error, and teardown. A single per-Jetty `fetching` flag is insufficient for a refactor that overlaps several payload DMA operations. |
| In conventional verbs RDMA, the SQ advertises `max_send_wr` outstanding WRs; an RC QP also has READ/atomic-specific `max_rd_atomic` and responder resource limits. The inline setter applies to SEND and RDMA WRITE, not READ. | [rdma-core `ibv_create_qp`](https://github.com/linux-rdma/rdma-core/blob/master/libibverbs/man/ibv_create_qp.3), [rdma-core `ibv_modify_qp`](https://github.com/linux-rdma/rdma-core/blob/master/libibverbs/man/ibv_modify_qp.3), [rdma-core `ibv_wr_post`](https://github.com/linux-rdma/rdma-core/blob/master/libibverbs/man/ibv_wr_post.3.md) | RDMA supports multiple outstanding work requests and operation-specific limits; it cannot supply the Kunpeng UDMA's unknown per-Jetty source-DMA depth. A fixed READ/WRITE performance ordering is not implied. |

## Local implementation and actual benchmark flags

- `sources/umdk/src/urma/tools/urma_perftest/perftest_run_test.c:1324`
  zero-initializes the WQE array with `calloc`; `init_jfs_wr_base()` at
  line 1004 then sets completion, solicited, and inline flags, but not
  `place_order`, `comp_order`, or `fence`. Thus the current `-I 0` sweep
  sends WQEs with all three of those flags zero (subject to any later
  modification in the posting path). A JFS/Jetty `order_type` is a separate
  resource configuration set in `perftest_resources.c:478`.
- `docs/ns3ub-network-boundary.md` assigns source SQ/WQE decoding and host
  DMA to `udma-model`, while ns-3-UB owns CTP ordering, segmentation,
  retransmission and switched-fabric behavior. In
  `integrations/ns3ub/ub-net-adapter.cc:886–979`, ns-3-UB receives a
  **complete** WRITE WQE payload after source DMA, and propagates the WQE's
  order tag to its native Jetty. The network implementation therefore
  cannot establish a hardware source-DMA depth of one.
- The native transaction implementation at
  `sources/ns-3-ub/src/unified-bus/model/protocol/ub-transaction.cc:2138–2162`
  admits `ORDER_NO` and `ORDER_RELAX` WQEs without waiting for the previous
  ordered WQE; `ORDER_STRONG` consults the per-Jetty ordered-WQE list. Thus
  even native ROI mode is not a blanket one-WQE-at-a-time rule.
- The same adapter currently uses one reusable native Jetty ID (`1`) for
  each endpoint path, independent of the guest Jetty ID, and retains the
  guest ID only as metadata (`ub-net-adapter.cc:943–959`). This is a
  separate multi-Jetty fidelity question: it can couple ordering or
  outstanding state across guest Jettys and should be tested before using
  ns-3-UB multi-Jetty scaling as evidence for real independent Jettys.
- `docs/validation/gem5-read-write-asymmetry-20261010.md` measures one
  source payload DMA in flight per Jetty for non-inline 128-B WRITE,
  204.69 ns mean fetch time and 4.853 Mpps WRITE; the reciprocal 4.885 Mpps
  is a close match. READ reaches 9.862 Mpps. This establishes the **model's**
  causal bottleneck, not a physical UDMA rule.

## What the public material does not establish

1. The Kunpeng 950 UDMA's physical source payload DMA slots per Jetty,
   shared DMA-engine width, SRAM buffer size, cache policy, or scheduling.
2. Whether the target system's `order_type` or internal controller policy
   imposes an undocumented fetch-order constraint even for NO-order WQEs.
3. A hardware READ-versus-WRITE rate curve or the ratio for the user's
   128-B workload; no physical WRITE measurements have been supplied.
4. A numeric default such as 2, 4, 8, or 16 for payload DMA depth. The
   official JFS queue depth and RDMA QP caps are not a substitute.

## Judgment and model criterion

The one-in-flight non-inline WRITE payload fetch is a legal *implementation
policy* and may be functionally correct, but neither the official URMA API
nor the openEuler guide requires it. Since it almost exactly caps the measured
simulated one-Jetty WRITE throughput, it should not be treated as a
calibrated Kunpeng 950 property. It is currently a **performance-fidelity
assumption with high impact**.

A defensible model should have a bounded, configurable per-Jetty fetch depth
and a separate bound for the shared source DMA engine. It should retain a
per-WQE sequence, payload ownership, and terminal state; issue ready frames
according to the requested NO/RO/SO and completion semantics; and drain or
fail all pending WQEs safely on teardown. Vary depth while holding one port,
one Jetty, workload flags, and fabric settings fixed. The depth may be
selected for sensitivity analysis now, but should not be labeled as the
physical value without vendor evidence or a matched physical WRITE curve.
