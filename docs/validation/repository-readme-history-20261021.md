# UBSim gem5 full-system lab

This repository boots independent ARM64 gem5 full-system nodes, loads the
official openEuler OLK and UMDK software stack in each guest, and connects their
modeled UB devices through a standalone UB network process.  The stable host
interface is the Python command `./lab`. JSON topology tasks use the native
Python launcher; legacy imperative commands still use their shell backends.

## Quick start

This prepared workspace already has the gem5 binaries, guest image, and
matching coordinated checkpoints. Restore a two-node modern configuration:

```bash
./lab topology validate configs/topology-gem5-asymmetric-links.json
./lab topology start configs/topology-gem5-asymmetric-links.json
./lab sync
./lab status
./lab rma-regression --sync-smoke
./lab stop
```

For new modern/4GB tasks, one node-neutral gem5 checkpoint can restore two or
three hosts (up to eight supported hosts) with independent guest identities and
UDMA devices. See [node-template checkpoint design](../../docs/node-template-checkpoint.md)
for creation, compatibility rules, and validation.

For a fresh clone on Linux, first build the ARM64 gem5 stack with
`./lab setup --jobs 2`.
New guest images or machine/network configurations need a compatible coordinated
checkpoint before `topology start` can restore them. The current workspace also
has a native Apple Silicon QEMU functional path. See
[the current build and run guide](../../docs/build-and-run.md) for both paths,
incremental build commands, artifacts, and checkpoint handling.

For a first build, keep at least 20 GiB of host disk space free. Guest kernel,
UMDK, and initramfs builds require Linux; on Apple Silicon macOS, the existing
guest image and checkpoints can run with native gem5 and QEMU. The ARM gem5
build, kernel tree, and checkpoints all need substantial space.

In a prepared ARM64 Ubuntu 22.04 environment:

```bash
./lab setup --jobs 8
# The next step requires a matching coordinated checkpoint.
./lab topology start configs/topology-gem5-two-heads.json
./lab status
./lab sync
./lab attach 0
```

All commands run on the host. Stop the current experiment with `./lab stop`.

## Command model

An experimental declarative topology entry point validates a JSON graph of
hosts, UDMA devices, switch ports, and links before launching the currently
supported backends:

```bash
./lab topology validate configs/topology-qemu-two-heads.json
./lab topology plan configs/topology-qemu-two-heads.json
./lab topology start configs/topology-qemu-two-heads.json
```

See [the topology compiler contract](../../docs/topology-orchestration.md) for its
current backend limits and checkpoint behavior. The second example,
`configs/topology-qemu-heterogeneous.json`, starts QEMU guests with different
CPU models, core counts, and memory capacities. The third example,
`configs/topology-qemu-asymmetric-links.json`, assigns different UB line rates
and propagation delays to the two hosts.

The local visual workbench provides task creation, a clickable topology canvas,
node-specific terminal access and launch logs:

```sh
./lab ui
```

Open `http://127.0.0.1:8765/`; see [workbench usage](../../docs/ui-workbench.md).

```text
./lab COMMAND [ARGS...]

Lifecycle:   setup, topology, start-ready, start, status, sync, checkpoint, checkpoint-template, attach, stop
Experiments: latency, latency-pair, latency-pairs, sweep-latency, rma-regression
Build:       build all|gem5|qemu|kernel|umdk|initramfs|template-initramfs|ns3ub|udma-model|udma-device|ub-switch|mooncake
QEMU:        start-qemu, start-qemu-dual, status-qemu, attach-qemu, qemu-rma-regression, stop-qemu
```

Examples:

```bash
# Show every simulator/model option without starting a guest.
./lab start --help
./lab start --print-config

# Routine development: restore the newest fully compatible shell checkpoint.
./lab start-ready --nodes 2 --profile fast --provider official --no-sync

# Attach to any node. UART ports are derived from node ID centrally.
./lab attach 0
./lab attach 1

# Official two-sided URMA latency tests.
./lab latency --samples 100 --size 128
./lab latency-pair 0 3 100 128 21115
./lab latency-pairs --samples 100 --size 128
./lab sweep-latency --samples 100 --sizes "2 4 8 16 32 64 128 256 512 1024 2048 4096"

# Apple Silicon: build QEMU 11.1.1 and enter an official-driver probe guest.
./lab build qemu
./lab start-qemu
# In the guest:
urma_admin show

# Apple Silicon functional two-node QEMU + UDMA + ns-3 bring-up.
./lab build udma-device
./lab build ns3ub
./lab start-qemu-dual
./lab status-qemu
./lab qemu-rma-regression
./lab attach-qemu 0
./lab attach-qemu 1

# Or connect through the dedicated localhost-only management NIC.
ssh -p 2220 root@127.0.0.1   # node0; press Enter for the empty password
ssh -p 2221 root@127.0.0.1   # node1; press Enter for the empty password
```

The console detach sequence is `~.` at the start of a line.  `./lab attach N`
reclaims only a stale `m5term` client for that node; it does not stop gem5.

## Repository layout

```text
lab                  Python CLI; the only supported host entry point
ubsim_lab/        CLI routing, runtime selection, and node addressing
configs/             lab-owned ARM64 full-system machine configuration
protocol/            simulator-neutral UB-HOST and UB-NET wire protocols
components/
  udma-model/         simulator-neutral UDMA device behavior and unit tests
  udma-device-sim/    standalone SimBricks host/network device process
  ub-switch-sim/      simulator-neutral UB-NET contract reference and tests
scripts/
  build/             heavyweight gem5/kernel/UMDK/initramfs builders
  run/               internal launch, lifecycle, synchronization, benchmarks
  validation/        read-only model/profile validation
  runtime.sh         shared host process helpers
tools/               switch simulator, guest helpers, profiling, and tests
official-udma/       official-driver build and contract evidence
integrations/ns3ub/  complete ns-3-UB adapter source and upstream overlay
integrations/gem5/   thin UB-HOST adapter overlay for gem5 builds
integrations/qemu/   thin QEMU SysBus/MMIO/DMA/IRQ UB-HOST adapter
overlay/             files installed into the guest initramfs
patches/             reviewed changes applied to pinned upstream sources
docs/                design notes and the detailed reference guide
results/             retained experiment reports, not executable control code
artifacts/, out/     generated build products
run-dual/            generated process state, manifests, logs, and UART output
sources/, gem5/      fetched/patched upstream source trees (generated)
```

The separation is intentional:

- `ubsim_lab/` is the control plane and stable interface.
- `scripts/run/` contains simulator orchestration that still benefits from
  Bash process control, but users do not call it directly.
- `protocol/` is the stable process boundary. `UB-HOST` carries MMIO, DMA and
  interrupts; `UB-NET` carries only wire-visible frames and link events.
- `components/udma-model/` owns device behavior without gem5, QEMU, or ns-3
  types. The current gem5 model is being migrated behind that boundary.
- `integrations/qemu/` contains no UDMA semantics: it forwards guest MMIO,
  DMA, and interrupts to the same standalone device process used by gem5.
- `configs/arm64_fs.py` is the lab-owned full-system machine; it instantiates
  only the thin UB-HOST adapter and contains no in-process UDMA model.
- official Linux/UMDK code remains under the pinned upstream source trees;
  reproducible patches live under `patches/`.

The QEMU path provides both a single-node contract probe and a two-node TCG
environment using the standalone UDMA devices and native ns-3 UB fabric.
Functional mode is the default and is suitable for driver/data-path bring-up.
An optional synchronized TCG timing mode exists, but the current QEMU result
is not a physical-machine latency calibration. Each guest has a separate
QEMU user-network management NIC;
localhost ports 2220 and 2221 forward to guest SSH port 22. The empty root
password is confined to this local functional lab and the forwarding sockets
bind only to `127.0.0.1`. An ITS-backed HVF interrupt bridge remains follow-up work; see
[the QEMU adapter note](../../integrations/qemu/README.md).

## Runtime architecture

Each node is an independent gem5 process running OLK 6.6, the official UBUS,
UMMU, UBASE/UBCORE and UDMA kernel modules, the official UMDK `liburma` stack,
and `urma_perftest`.  An out-of-process UB switch routes traffic by destination
EID.  A separate Ethernet switch carries the benchmark control connection.
The adapter-local conservative synchronization and unsynchronized KVM policies
are selected by `./lab start` options; they are not encoded in wrapper scripts.

For the complete model knobs, evidence, source revisions, and troubleshooting,
see [the reference guide](../../docs/reference-guide.md),
[modular simulator architecture](../../docs/modular-simulator-architecture.md),
[source dependency boundary](../../docs/ubsim-source-boundary.md),
[KVM functional mode](../../docs/kvm-functional-mode.md), and
[Mooncake bring-up](../../docs/mooncake-urma-bringup.md).

## Development checks

The CLI and host helpers use only the Python standard library:

```bash
PYTHONPATH=tools:. python3 -m unittest \
  tools.test_lab_cli tools.test_ethernet_relay \
  tools.test_modular_rma_regression
bash -n scripts/run/*.sh scripts/build/*.sh scripts/*.sh
./lab build udma-model
./lab build udma-device
./lab build ub-switch
./lab build udma-device
./lab start --profile kvm --print-config
```

Build products and fetched upstream trees are not part of the control-plane
script count.  Run `git ls-files '*.sh' '*.py'` to audit repository-owned
automation.

## Architecture targets

Host architecture and guest architecture are separate. A native x86_64 Ubuntu
22.04 host can directly build the complete ARM64 full-system experiment. The
setup creates an extraction-only ARM64 sysroot, cross-builds guest software,
and builds an x86_64-host gem5 executable with the ARM ISA model:

```bash
./lab setup --target-arch arm64 --jobs 8
./lab start --nodes 2 --profile fast --provider official
```

This does not execute ARM code during the build. The separate native x86_64
UMDK ABI target remains available:

```bash
./lab setup --target-arch x86_64

# Or, in an already provisioned source tree:
BUILD_STOCK_UDMA=enable ./lab build umdk --target-arch x86_64
```

The resulting files use `artifacts/umdk-build-x86_64/`, so they cannot overwrite
the ARM64 guest artifacts. This is intentionally a userspace-only target. The
pinned OLK source declares `CONFIG_UB` as `depends on ARM64`; its UMMU SVA path
uses ARM64 system registers and page-table APIs. The current gem5 platform also
uses ArmSystem, GIC interrupts, and an ARM64 page-table walker. Consequently,
`build kernel|initramfs|all --target-arch x86_64` is rejected instead of
silently producing a nonfunctional full-system image.

## ns-3-UB adapter source

The simulator-neutral UB-NET v1 adapter is checked in under
`integrations/ns3ub/`; it is not hidden in a sibling development checkout.
`./lab setup --sources-only` fetches the pinned public ns-3-UB baseline into
`sources/ns-3-ub`, and the build command installs the reviewed adapter overlay:

```bash
./lab build ns3ub
# Simulator-neutral five-process path (2 gem5 + 2 UDMA + 1 ns-3 fabric).
# Use an existing compatible checkpoint for routine runs:
./lab build udma-device
./lab topology start configs/topology-gem5-two-heads.json
```

The build runs bidirectional UB-NET process contracts with synchronization
disabled and required. `modular-ns3ub` selects UB-HOST v1 from each gem5 to
an independent UDMA process and UB-NET v1 from those devices to native ns-3-UB.
The modular mode requires the official provider. `--sync` makes both UB-HOST
links and both UB-NET links participate from tick zero. The configured
host/device and endpoint/fabric propagation delays are the conservative
lookahead. Use `--no-sync` for fast functional bring-up; its latency output is
not a synchronized virtual-time result. With the default 100 ns lookahead, a
cold full-system Atomic boot is intentionally expensive, so synchronized
experiments should restore a prepared coordinated checkpoint rather than
weakening the runtime causality contract.

The normal development and regression path restores a shell-ready checkpoint
with `start-ready` and runs without conservative synchronization. It compares
the complete requested machine/network manifest and fails rather than using an
incompatible checkpoint. Plain `start` is reserved for work that intentionally
tests kernel boot, initialization, or checkpoint creation. This keeps functional stress
tests fast while ns-3 still executes every serialization, queueing, routing and
delivery event at its modeled virtual timestamp:

```bash
./lab start-ready --profile fast --provider official \
  --network-backend modular-ns3ub --no-sync
./lab sync
./lab rma-regression experiments/rma-regression
```

`rma-regression` writes `results.csv`, `report.json`, the resolved model
manifest and one complete UART transcript per case.  It covers SEND latency
and bandwidth, READ/WRITE latency and bandwidth, 4 KiB/64 KiB/1 MiB
fragmentation, SQ ring wrap and 16 outstanding operations.  Every row includes
wall-clock time and the per-node gem5 tick delta observed across that case.  In `--no-sync` mode
the two node deltas are deliberately reported separately; their maximum is a
progress indicator, not a globally synchronized latency.  The verified matrix
and its measured host times are summarized in
[`docs/modular-rma-regression.md`](../../docs/modular-rma-regression.md).

The same functional matrix can be run on the dual-QEMU path with
`./lab qemu-rma-regression`. QEMU conservative synchronization is disabled,
so this command records wall time and checks completion/results (not payload
contents) but intentionally
does not report gem5 virtual ticks. The current Apple Silicon result is in
[`results/qemu-functional-regression-20260930.md`](../../results/qemu-functional-regression-20260930.md).

Use `UBSIM_RMA_SUITE=outstanding512` for depth512 with batch/CQ1,16,512;
`UBSIM_RMA_SUITE=boundaries` for non-power-of-two inline/fragment boundaries;
or `UBSIM_RMA_SUITE=multi UBSIM_RMA_JETTYS=4` for four Jettys. A configured
depth is not a measured in-flight high-water mark; perftest alone does not
verify payload bytes or prove resource isolation.

For byte-level checks, build `scripts/build/build-urma-integrity.sh` in the
Linux builder, then use `python3 tools/run_qemu_integrity.py OUTPUT --suite
boundaries` on shell-ready no-sync QEMU guests. Additional suites are `wrap`,
`concurrent`, `shared`, `large` and `send-overlimit`. The last is an expected
length-error test, not a successful oversized SEND. `large` explores RMA
beyond the advertised capability; `shared` rotates Jettys rather than claiming
simultaneous shared-JFR arrival ordering. On any failure, stop the experiment
before reusing guests. See the
[functional acceptance record](../../docs/validation/qemu-functional-matrix-20261009.md)
for evidence and open full-JFC overflow semantics.

Create a new coordinated shell snapshot only when both guests and the external
device are idle:

```bash
./lab checkpoint shell-ready-rma
./lab start --restore-checkpoint shell-ready-rma
```

The checkpoint contains both gem5 architectural states and both standalone
UDMA states.  Process shutdown prints `[UB_HOST_PROFILE]`, `[UDMA_PROFILE]` and
`[NS3_UB_NET_STATS]` counters for boundary-level profiling.

SQ descriptor prefetch is configurable on both dual-host launch paths:
`UBSIM_SQ_PREFETCH_WQEBBS=64` bounds each SQ's cache (including pending reads),
`UBSIM_SQ_FETCH_BURST_WQEBBS=16` bounds each contiguous descriptor read, and
`UBSIM_SQ_FETCH_MAX_OUTSTANDING=4` bounds concurrent descriptor reads per SQ.
One WQEBB is 64 bytes. Set all three to `1` for the serial-fetch control.
These are model architecture parameters, not fitted physical-hardware constants;
they do not change host-link latency or payload-DMA concurrency. Prefetch reads
only doorbell-submitted entries and decodes them in order. The resolved manifest
records these settings; old ready checkpoints without matching settings are not
automatically reused.

Completion notifications have a separate bounded pipeline:
`UBSIM_COMPLETION_EVENT_SLOTS=64` limits the total reserved, queued and active
notifications (valid range 1..4096). CEQ consumer doorbells return ring credits;
a full CEQ is not overwritten. CQE commit can finish before its CEQ/MSI
notification, but checkpoint/destroy still track the notification's lifetime.
Device state format is now 3; format-2 snapshots are rejected, and `start-ready`
checks the format and slot budget in the manifest before selecting a snapshot.

`BUILD_STOCK_UDMA=enable ./lab build umdk` now builds and links the complete
official `deps/ummu` library by default.  The old bootstrap shim remains an
explicit diagnostic fallback via `UBSIM_UMMU_MODE=shim`; it is not the
default official-provider path.
