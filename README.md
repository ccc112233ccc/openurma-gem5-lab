# UBSim gem5 full-system lab

This repository boots independent ARM64 gem5 full-system nodes, loads the
official openEuler OLK and UMDK software stack in each guest, and connects their
modeled UB devices through a standalone UB network process.  The stable host
interface is the Python command `./lab`; files below `scripts/` are internal
build and runtime backends.

## Quick start

On macOS or another Docker host:

```bash
./lab setup --jobs 2
./lab start --nodes 2 --profile fast --provider official
./lab status
./lab sync
./lab attach 0
```

For a first build, keep at least 20 GiB of host disk space free and use the
documented `--jobs 2` baseline on a default-memory Docker Desktop VM.  gem5's
generated ARM decoder can exceed that VM's memory with `--jobs 4`; higher
parallelism is safe only after increasing Docker's memory limit.  Source trees,
the ARM gem5 build, Docker image layers, kernel tree and checkpoints are all
large, so a clean-clone build beside an existing lab needs additional space.

In a prepared ARM64 or x86_64 Ubuntu 22.04 environment, select native execution
once on each command (or export `UBSIM_EXECUTION_MODE=native`):

```bash
./lab --runtime native setup --jobs 8
./lab --runtime native start --nodes 2 --profile fast --provider official
./lab --runtime native status
./lab --runtime native sync
./lab --runtime native attach 0
```

`auto` is the default runtime: supported Linux hosts use native mode and macOS
uses Docker. Stop the current experiment with `./lab stop`.

## Command model

```text
./lab [--runtime auto|docker|native] COMMAND [ARGS...]

Lifecycle:   setup, start, status, sync, attach NODE, stop
Experiments: latency, latency-pair, latency-pairs, sweep-latency
Build:       build all|gem5|qemu|kernel|umdk|initramfs|ns3ub|mooncake|udma-model|udma-device|ub-switch
Validation:  validate-server, start-qemu, start-qemu-dual
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

# Four nodes behind one EID-routing switch.
./lab start --nodes 4 --profile fast --provider official

# Apple Silicon: build QEMU 11.1.1 and enter an official-driver probe guest.
./lab build qemu
./lab start-qemu
# In the guest:
urma_admin show

# Apple Silicon functional two-node QEMU + UDMA + ns-3 bring-up.
./lab --runtime native build udma-device
./lab --runtime native build ns3ub
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
  runtime.sh         shared native-versus-Docker execution adapter
tools/               switch simulator, guest helpers, profiling, and tests
official-udma/       official-driver build and contract evidence
integrations/ns3ub/  complete ns-3-UB adapter source and upstream overlay
integrations/gem5/   thin UB-HOST adapter overlay for gem5 builds
integrations/qemu/   thin QEMU SysBus/MMIO/DMA/IRQ UB-HOST adapter
overlay/             files installed into the guest initramfs
patches/             reviewed changes applied to pinned upstream sources
docker/              reproducible ARM64 Ubuntu build image
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
functional environment using the standalone UDMA devices and native ns-3 UB
fabric. The dual path deliberately runs without conservative virtual-time
synchronization, so it is suitable for driver/data-path bring-up rather than
latency claims. Each guest has a separate QEMU user-network management NIC;
localhost ports 2220 and 2221 forward to guest SSH port 22. The empty root
password is confined to this local functional lab and the forwarding sockets
bind only to `127.0.0.1`. An ITS-backed HVF interrupt bridge remains follow-up work; see
[the QEMU adapter note](integrations/qemu/README.md).

## Runtime architecture

Each node is an independent gem5 process running OLK 6.6, the official UBUS,
UMMU, UBASE/UBCORE and UDMA kernel modules, the official UMDK `liburma` stack,
and `urma_perftest`.  An out-of-process UB switch routes traffic by destination
EID.  A separate Ethernet switch carries the benchmark control connection.
The adapter-local conservative synchronization and unsynchronized KVM policies
are selected by `./lab start` options; they are not encoded in wrapper scripts.

For the complete model knobs, evidence, source revisions, and troubleshooting,
see [the reference guide](docs/reference-guide.md),
[modular simulator architecture](docs/modular-simulator-architecture.md),
[source dependency boundary](docs/ubsim-source-boundary.md),
[KVM functional mode](docs/kvm-functional-mode.md), and
[Mooncake bring-up](docs/mooncake-urma-bringup.md).

## Development checks

The CLI and host helpers use only the Python standard library:

```bash
PYTHONPATH=tools:. python3 -m unittest \
  tools.test_lab_cli tools.test_ethernet_relay \
  tools.test_modular_rma_regression
bash -n scripts/run/*.sh scripts/build/*.sh scripts/*.sh
./lab --runtime native build udma-model
./lab --runtime native build udma-device
./lab --runtime native build ub-switch
./lab --runtime native build udma-device
./lab --runtime native start --profile kvm --print-config
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
./lab --runtime native setup --target-arch arm64 --jobs 8
./lab --runtime native start --nodes 2 --profile fast --provider official
```

This does not execute ARM code during the build and does not require an ARM64
Docker container. The separate native x86_64 UMDK ABI target remains available:

```bash
./lab --runtime native setup --target-arch x86_64

# Or, in an already provisioned source tree:
BUILD_STOCK_UDMA=enable ./lab --runtime native build umdk --target-arch x86_64
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
# Simulator-neutral five-process path (2 gem5 + 2 UDMA + 1 ns-3 fabric):
./lab --runtime docker build udma-device
./lab --runtime docker start --profile fast --provider official \
  --network-backend modular-ns3ub --sync
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
[`docs/modular-rma-regression.md`](docs/modular-rma-regression.md).

The same functional matrix can be run on the dual-QEMU path with
`./lab qemu-rma-regression`. QEMU conservative synchronization is disabled,
so this command records wall time and validates data movement but intentionally
does not report gem5 virtual ticks. The current Apple Silicon result is in
[`results/qemu-functional-regression-20260930.md`](results/qemu-functional-regression-20260930.md).

Create a new coordinated shell snapshot only when both guests and the external
device are idle:

```bash
./lab checkpoint shell-ready-rma
./lab start --restore-checkpoint shell-ready-rma
```

The checkpoint contains both gem5 architectural states and both standalone
UDMA states.  Process shutdown prints `[UB_HOST_PROFILE]`, `[UDMA_PROFILE]` and
`[NS3_UB_NET_STATS]` counters for boundary-level profiling.

`BUILD_STOCK_UDMA=enable ./lab build umdk` now builds and links the complete
official `deps/ummu` library by default.  The old bootstrap shim remains an
explicit diagnostic fallback via `UBSIM_UMMU_MODE=shim`; it is not the
default official-provider path.
