# OpenURMA gem5 full-system lab

This repository boots independent ARM64 gem5 full-system nodes, loads the
official openEuler/OpenURMA software stack in each guest, and connects their
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

In a prepared ARM64 or x86_64 Ubuntu 22.04 environment, select native execution
once on each command (or export `OPENURMA_EXECUTION_MODE=native`):

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
Build:       build all|gem5|kernel|umdk|initramfs|ns3ub|mooncake|udma-model
Validation:  validate-server, start-single
```

Examples:

```bash
# Show every simulator/model option without starting a guest.
./lab start --help
./lab start --print-config

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
```

The console detach sequence is `~.` at the start of a line.  `./lab attach N`
reclaims only a stale `m5term` client for that node; it does not stop gem5.

## Repository layout

```text
lab                  Python CLI; the only supported host entry point
openurma_lab/        CLI routing, runtime selection, and node addressing
configs/             gem5 full-system machine and OpenURMA device topology
protocol/            simulator-neutral UB-HOST and UB-NET wire protocols
components/
  udma-model/         simulator-neutral UDMA device behavior and unit tests
scripts/
  build/             heavyweight gem5/kernel/UMDK/initramfs builders
  run/               internal launch, lifecycle, synchronization, benchmarks
  validation/        read-only model/profile validation
  runtime.sh         shared native-versus-Docker execution adapter
tools/               switch simulator, guest helpers, profiling, and tests
official-udma/       official-driver build and contract evidence
integrations/ns3ub/  complete ns-3-UB adapter source and upstream overlay
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

- `openurma_lab/` is the control plane and stable interface.
- `scripts/run/` contains simulator orchestration that still benefits from
  Bash process control, but users do not call it directly.
- `protocol/` is the stable process boundary. `UB-HOST` carries MMIO, DMA and
  interrupts; `UB-NET` carries only wire-visible frames and link events.
- `components/udma-model/` owns device behavior without gem5, QEMU, or ns-3
  types. The current gem5 model is being migrated behind that boundary.
- `configs/` and the C++ code in `tools/` contain the existing integrated model
  and network implementations while that migration is in progress.
- official Linux/UMDK code remains under the pinned upstream source trees;
  reproducible patches live under `patches/`.

## Runtime architecture

Each node is an independent gem5 process running OLK 6.6, the official UBUS,
UMMU, UBASE/UBCORE and UDMA kernel modules, the official UMDK `liburma` stack,
and `urma_perftest`.  An out-of-process UB switch routes traffic by destination
EID.  A separate Ethernet switch carries the benchmark control connection.
The `adapter-local`, global-barrier, and unsynchronized KVM policies are
selected by `./lab start` options; they are not encoded in wrapper scripts.

For the complete model knobs, evidence, source revisions, and troubleshooting,
see [the reference guide](docs/reference-guide.md),
[modular simulator architecture](docs/modular-simulator-architecture.md),
[KVM functional mode](docs/kvm-functional-mode.md), and
[Mooncake bring-up](docs/mooncake-urma-bringup.md).

## Development checks

The CLI and host helpers use only the Python standard library:

```bash
PYTHONPATH=tools:. python3 -m unittest tools.test_lab_cli tools.test_ethernet_relay tools.test_ub_switch_sim
bash -n scripts/run/*.sh scripts/build/*.sh scripts/*.sh
./lab --runtime native build udma-model
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

The complete external-process adapter is checked in under
`integrations/ns3ub/`; it is not hidden in a sibling development checkout.
`./lab setup --sources-only` fetches the pinned public ns-3-UB baseline into
`sources/ns-3-ub`, and the build command installs the reviewed adapter overlay:

```bash
./lab build ns3ub
./lab start --network-backend ns3ub-native
```
