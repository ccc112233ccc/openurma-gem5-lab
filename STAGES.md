# Stage checkpoints

The laboratory is split across several Git repositories because gem5, the
OpenEuler kernel and UMDK retain their upstream histories.  A stage is saved
by applying the same annotated tag to every repository changed in that stage.
`SOURCE_REVISIONS.md` records the matching commits.

## Naming convention

Use `stage/YYYY-MM-DD-short-name`, for example:

```text
stage/2026-09-17-ubus-resource-discovery
```

Each stage commit must state:

1. the observable milestone reached;
2. the first known failing boundary;
3. the validation directory or command;
4. whether official driver sources were modified.

Generated kernels, initramfs images, gem5 binaries and run logs are not stored
in Git.  They remain reproducible through the tracked build scripts and the
pinned revisions in `SOURCE_REVISIONS.md`.

## Saved stages

### `stage/2026-09-17-ubus-resource-discovery`

- Existing two-node Atomic CPU functional data path retained.
- Official UBFI discovers the modeled UBIOS and UBC.
- Official UBUS stack enumerates a root controller and one endpoint.
- Token query and three 1 MiB endpoint resource windows succeed.
- Official UMMU, UBASE and UDMA modules load without source changes.
- First open boundary: UBASE firmware command queue returns `-52`, so
  `ubase_core.udma` is not created yet.
- Validation evidence: `run-official-resource-v3-20260916/` (kept locally,
  intentionally ignored by Git).

### `stage/2026-09-17-ubase-cmdq-usi`

- Unmodified UBASE completes its command queue and reports firmware version
  `1.0.0.0`.
- The generated DT advertises gem5's existing GICv2m frame and associates the
  UBC through `msi-parent`.
- A simulation-only bridge publishes the required `DOMAIN_BUS_UB_MSI` domain
  and attaches it to the firmware-created UBC; official UBUS/UBASE/UDMA source
  files remain unchanged.
- Type-1 USI capability discovery and MSI descriptor setup reach
  `ub_msi_domain_set_desc` successfully.
- First open boundary: UBASE mailbox status/query and EQC creation return
  `-16`, so `ubase_core.udma` is not created yet.
- Validation evidence: `run-official-usi-v3-20260917/` (kept locally,
  intentionally ignored by Git).

### `stage/2026-09-17-ubase-ctrlq-msi`

- The simulation-only GICv2m bridge now mirrors the official ITS UBUS
  irqchip and updates both the UBUS device mask and the parent GIC SPI mask.
- The NIC routes MSI writes through a dedicated system-bus request port and
  consumes the Type-1 address/data tuple programmed by the official driver.
- Unmodified UBASE completes mailbox status, EQC setup, QoS SL discovery and
  the control-plane initialization notification through real CSQ/CRQ DMA and
  Type-1 MSI delivery.
- CtrlQ response sizes and direction bits are derived from the official
  request rather than fixed to one command shape.
- First open boundary: UBASE requests a 256 KiB context IOVA, but only
  `ummu-core.ko` is present and no full UMMU device has registered an IOMMU
  domain, so `dma_alloc_iova()` returns `-ENODEV`.
- Validation evidence: `run-official-ctrlq-v6-20260917/` (kept locally,
  intentionally ignored by Git).
- Official UBFI, UBUS, UBASE, UMMU and UDMA driver sources remain unchanged.

### `stage/2026-09-17-ummu-device-probe`

- The build now includes the full unmodified official `ummu.ko`, in addition
  to `ummu-core.ko`.
- UBIOS publishes one architecture-generic UMMU node and associates UBC 0
  with that node through `ummu_map`.
- The device model exposes the minimal truthful capability set, CR0/GBPA
  acknowledgement semantics, writable queue registers and immediate MCMDQ
  command consumption.
- Unmodified `ummu.ko` completes probe and reports
  `ummu register to ummu core successful!`.
- First open boundary: the subsequent official UBUS attach path lacks a valid
  default BI/decoder description and dereferences a null `uent->bi` while
  assigning its default DMA domain. Decoder/BI discovery is therefore the
  next modeled hardware contract, not another UMMU bypass.
- Validation evidence: `run-official-ummu-v2-20260917/` plus the attached
  PL011 trace (run output is intentionally ignored by Git).
- No official UMMU, UBFI, UBUS, UBASE or UDMA source file was modified.

### `stage/2026-09-17-ubase-aux-device`

- Official UBUS builds its default BI and decoder, enumerates endpoint `00002`
  and attaches both root and endpoint devices to the official UMMU domains.
- The model translates UBASE CSQ/CRQ and GICv2m MSI IOVAs through the TECT,
  TCT and ARM64 page tables programmed by the unmodified official UMMU stack.
- Official CtrlQ request/response traffic completes through DMA and Type-1
  MSI; mailbox completions are delivered through the programmed AEQ and its
  own MSI vector.
- Unmodified `ubase.ko` completes probe and creates auxiliary devices
  `ubase.udma.0` and `ubase.unic.0`.
- First open boundary: unmodified `udma.ko` matches `ubase.udma.0`, but the
  modeled firmware resource response reports zero UDMA jetty ranges, so its
  resource-table initialization returns `-EINVAL`.
- Validation evidence: `run-official-ummu-v8-20260917/` and
  `run-official-ummu-v8-model.log` (kept locally, intentionally ignored by
  Git).
- No official UMMU, UBFI, UBUS, UBASE or UDMA source file was modified.

### `stage/2026-09-17-official-udma-probe`

- The kernel configuration enables the official UMMU SVA/KSVA path; the
  unmodified UMMU driver allocates MAPT blocks and publishes its translation
  context through TCT and MCMDQ.
- Modeled UBASE and UDMA resource queries return finite, ABI-aligned queue,
  table and jetty capabilities.
- The control-plane CTRLQ returns the SEID belonging to the endpoint already
  enumerated by UBIOS, through the real CSQ/CRQ DMA and Type-1 MSI path.
- Unmodified official `udma.ko` completes `probe()`, reports
  `init udma successfully`, registers `udma0` with ubcore and creates
  `/dev/uburma/udma0`.
- First open boundary: the official device-status query sees modeled port
  speed zero, so `urma_admin show` reports `udma0` with `link NOP`. Port
  capability/status is the next hardware contract.
- Validation evidence: `run-official-ummu-v14-20260917/` and
  `run-official-ummu-v14-model.log` (kept locally, intentionally ignored by
  Git).
- No official UMMU, UBFI, UBUS, UBASE or UDMA source file was modified.

### `stage/2026-09-17-official-udma-active`

- The management-plane UDMA EID is an explicit per-node model parameter; it
  is distinct from the UBUS entity EID, as required by the official UMMU
  duplicate-entry checks.
- The model implements the official `UDMA_CMD_QUERY_PORT_INFO` response and
  reports one 400-Gb/s UB lane.
- Unmodified official `udma.ko` completes probe with EID
  `0000:0000:0000:0000:0000:0000:0000:0100`; `urma_admin show` reports
  `udma0` as `ACTIVE` and `/dev/uburma/udma0` persists.
- A same-guest server/client smoke test reaches the stock UDMA provider and
  creates Jettys 1024 and 1025 through the official userspace and kernel
  resource path.
- First open boundary: both peers request TP allocation through CtrlQ service
  `TP_ACL`, opcode `GET_TP_LIST (0x21)`; the model does not yet provide that
  management-plane response, so the official driver times out.
- Validation evidence: `run-official-ummu-v16b-20260917/` and
  `run-official-ummu-v16b-model.log` (kept locally, intentionally ignored by
  Git).
- No official UMMU, UBFI, UBUS, UBASE or UDMA source file was modified.

## Future checkpoints

The next intended tags are created only after their observable gates pass:

- `stage/...-official-urma-resources`: context, segment and queue resources
  can be created.
- `stage/...-official-dual-node-perftest`: two-node `send_lat` completes on
  the official stack.
