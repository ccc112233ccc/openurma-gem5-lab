# New-session handoff — 2026-10-09

Read `AGENTS.md` first. The user requested disk cleanup and a stopped environment
to continue in a new conversation. Do not restart experiments automatically.

## Code and runtime

- Repository: `/Users/caobo/workspace/openurma-gem5-lab`.
- Branch: `feature/ns3ub-network-backend`.
- Latest implementation commit: `63b0d0e`; previous functional gate: `3d2ca71`.
- Code is locally committed; these changes were NOT pushed. Do not push without
  authorization. Cleanup did not revert code or reset the branch.
- Docker container: `ubsim-gem5-lab`, stopped for handoff. Start it with
  `docker start ubsim-gem5-lab` when needed.
- No host or container simulator processes remained before cleanup.
- Existing `ubsim` heartbeat automation was already PAUSED; keep it paused.
- Official drivers, sources, current binaries, guest image, reports and PPTs
  are preserved. Container-side project path: `/workspace/ubsim-gem5-lab`.

## Retained ready checkpoints

Only two current state-format-v3 checkpoints remain (~191MiB combined):

- `shell-ready-functional-v3-2port`: fast/official/modular-ns3ub/no-sync/2ports.
- `shell-ready-timing-v3-1port`: fast/official/modular-ns3ub/ROI-sync/1port.

Functional launch from the macOS repository, when requested:

```sh
docker start ubsim-gem5-lab
./lab start-ready --profile fast --provider official \
  --network-backend modular-ns3ub --no-sync --ub-port-count 2
./lab sync
./lab attach 0
# A separate terminal: ./lab attach 1
```

Use `start-ready`, not routine cold boot. `./lab sync` here initializes OOB and
checks the timer; the no-sync launch remains a functional, not timing, test.

## Accepted evidence and unresolved work

- `docs/validation/gem5-functional-current-20261009.md`:27/27 official perftest
  functional cases from a restored checkpoint; not gem5 byte-integrity proof.
- `docs/validation/qemu-functional-matrix-20261009.md`: QEMU payload/queue/multi
  functional evidence. Full-JFC overflow recovery remains incomplete.
- `docs/validation/gem5-timing-1port-20261009.md`: latest bounded timing attempts.
  No synchronized bandwidth result accepted. Do not report old no-sync BW as
  valid performance or tune constants to match physical hardware.

Timing entry ABI, ns-3 background-event gate, and COMMIT/SYNC epoch handoff were
fixed. SEND now executes8 WQEs per endpoint (network16/16 complete), but the
early finisher blocks inside gem5's synchronous exit-fence wait, stops advancing
the conservative horizon, and prevents the other host's tail DMA from finishing.
Next repair is an event-driven exit drain: pause guest CPU, continue real gem5
device events and finite conservative horizons, then commit/rebase/resume only
when ownership drains. Do NOT grant infinite horizons, synthesize time or skip
pending DMA. Add asymmetric-finish/delayed-DMA evidence before claiming success.

## Cleanup performed

- Removed168 untracked experiment `*.shm` ring files,42.165GiB allocated.
  Kept the logs/JSON/manifests/results; experiments now~122MiB.
- Removed12 explicitly identified inactive container `/tmp` shared-memory
  files (~3.3GiB logical allocation). Kept temporary logs.
- Removed five obsolete checkpoints: `post-rma-verified`,
  `shell-ready-full-ummu`, `shell-ready-rma-cap-fixed`, `shell-ready-v2`,
  `shell-ready-v3` (~497MiB).
- Deleted files were disposable runtime state, not moved to Trash; the old
  snapshots cannot be restored without recreating them. Current ready snapshots
  and committed code remain. Docker disk-image space reclamation may lag deletion.
- Do not blanket-prune Docker: other CANN/HCCL images and volumes are unrelated.
- Future native experiment cleanup should unlink ring files after all peers
  exit; retain diagnostic logs, not shared-memory dumps.
