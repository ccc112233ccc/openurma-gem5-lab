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

## Future checkpoints

The next intended tags are created only after their observable gates pass:

- `stage/...-ubase-aux-device`: UBASE command queue completes and
  `ubase_core.udma` appears.
- `stage/...-official-udma-probe`: unchanged official `udma.ko` completes
  probe and registers with ubcore.
- `stage/...-official-urma-resources`: context, segment and queue resources
  can be created.
- `stage/...-official-dual-node-perftest`: two-node `send_lat` completes on
  the official stack.
