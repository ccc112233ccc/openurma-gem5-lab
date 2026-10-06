# OpenEuler UB full-system simulation reference

## Supported architecture

The supported timed topology is an N-node modular system:

```text
gem5 node 0 --UB-HOST-- UDMA 0 --UB-NET--+
                                             ns-3-UB fabric
gem5 node N --UB-HOST-- UDMA N --UB-NET--+
```

Each gem5 process runs an independent ARM64 OLK guest. Each standalone UDMA
process owns device discovery, register/control behavior, DMA, interrupts,
queues, TP state, packetization and RMA semantics. The ns-3 process owns
routing, switch queues, arbitration, serialization, delay, congestion and link
state. A separate Ethernet relay carries only the userspace control channel.

QEMU implements the same UB-HOST boundary for functional experiments. It uses
the same UDMA and ns-3 processes, so hardware behavior is not duplicated in
the host simulator adapter.

## Source ownership

| Layer | Source |
| --- | --- |
| Kernel UB stack and UDMA driver | pinned openEuler OLK 6.6 (`oe66`) |
| UMMU support | pinned openEuler UMMU (`deps/ummu`) |
| liburma, tools, perftest and provider | pinned openEuler UMDK (`sources/umdk`) |
| gem5 | pinned upstream plus reviewed bundle |
| Network model | pinned ns-3-UB plus `integrations/ns3ub/ub-net-adapter.cc` |
| Hardware model | `components/udma-model/` and `components/udma-device-sim/` |

There is no dependency on the retired compatibility checkout, its SystemC
scaffold, compatibility provider, or peer-ring switch. The current model is
the standalone UDMA process plus ns-3-UB fabric described above.

## Setup

Docker host:

```sh
./lab setup --jobs 8
```

Prepared Ubuntu host:

```sh
./lab --runtime native setup --jobs 8
```

The setup command fetches pinned public sources, verifies bundles, builds the
host simulators and adapters, builds the official guest software, and creates
`out/official-udma.cpio.gz`. Exact revisions are in `SOURCE_REVISIONS.md`.

## Start and connect

```sh
./lab start --nodes 2 --profile fast --provider official
./lab status
./lab attach 0
./lab attach 1
```

Detach from a UART with `~.` at the start of a line. Stop all processes for the
run with `./lab stop`.

For the dual-QEMU functional path, SSH is usually more convenient than UART:

```sh
./lab start-qemu-dual
ssh -p 2220 root@127.0.0.1   # node0
ssh -p 2221 root@127.0.0.1   # node1
```

Press Enter at the password prompt. These management forwards listen only on
localhost. QEMU configures both its peer OOB `eth0` and management `eth1`
automatically. In the synchronized gem5 path, `ubsim-net-up` configures the
OOB Ethernet used by `urma_perftest` for its control handshake; it never
carries UB payload traffic.

Use `./lab start-qemu-dual --timing` when the benchmark must observe QEMU TCG
virtual time and the timestamped UDMA/ns-3 fabric. The QEMU data path models a
400-Gb/s port by default. Its host-side DMA engine pipelines page-sized
transfers and caches UMMU translations, as hardware does. The main sensitivity
knobs are:

- `UBSIM_PEER_LINK_RATE_GBPS` (default `400`): ns-3 UB port line rate;
- `UBSIM_QEMU_DMA_MAX_OUTSTANDING` (default `32`): data DMA requests per WQE;
- `UBSIM_QEMU_IOTLB_ENTRIES` (default `4096`): modeled translation-cache size;
- `UBSIM_QEMU_HOST_LATENCY_NS` (default `500`): QEMU-to-UDMA adapter latency.

Resolved values are recorded in `run-qemu-dual/run-manifest.txt`. Metadata and
control-plane DMA remain ordered; only payload movement uses the configurable
parallel window.

The supported backend/provider selections are intentionally singular:

- `--network-backend modular-ns3ub`
- `--ub-transport switch-adapter`
- `--provider official`

Profiles select CPU/cache/memory detail, not a different device model:

- `fast`: AtomicSimpleCPU functional bring-up;
- `udma400`: Atomic CPU with modeled caches;
- `server`: reduced-core ArmO3 server model;
- `kvm`: ARM64 Linux KVM functional path, when the host supports it.

Use `./lab start --help` for every physical/model knob and
`./lab start --print-config` to capture the resolved experiment contract.

## Virtual time

`adapter-local` synchronization uses timestamped DATA/SYNC messages on every
UB-HOST and UB-NET boundary. Positive boundary latency provides conservative
lookahead. Atomic/timing profiles enable it by default; KVM defaults to
unsynchronized execution because very short horizons cause frequent expensive
KVM exits.

```sh
./lab start --profile fast --sync
./lab start --profile fast --no-sync
```

Synchronized mode is required for virtual-time latency claims. Unsynchronized
mode remains useful for boot, driver and functional regression. There is no
global-barrier backend in the supported path.

## Guest checks and experiments

After both shells are ready:

```sh
urma_admin show
```

For two-sided SEND latency, start the server first on node 0:

```sh
urma_perftest send_lat -d udma0 --eid_idx 0 --ctp \
  -s 128 -P 21115 -J 1 -I 128 -n 100
```

Then run the client on node 1:

```sh
urma_perftest send_lat -d udma0 --eid_idx 0 --ctp \
  -s 128 -P 21115 -J 1 -I 128 -n 100 -S 10.0.0.1
```

The repository wrappers can run the same pairing and sweeps:

```sh
./lab latency --samples 100 --size 128
./lab sweep-latency --samples 100 \
  --sizes "2 4 8 16 32 64 128 256 512 1024 2048 4096"
```

SEND/READ/WRITE latency and bandwidth coverage, including fragmentation and
SQ wraparound, is maintained by the modular regression scripts and retained
evidence under `results/` and `official-udma/`.

## Build checks

```sh
PYTHONPATH=tools:. python3 -m unittest tools.test_lab_cli tools.test_ethernet_relay
bash -n scripts/run/*.sh scripts/build/*.sh scripts/*.sh
./lab --runtime native build udma-model
./lab --runtime native build udma-device
./lab --runtime native build ub-switch
./lab --runtime native build ns3ub
```

The `ub-switch` target is a simulator-neutral contract reference. Full-system
runs use the native ns-3 UB-NET adapter.

## Troubleshooting

- `terminal already attached`: use `./lab attach N`; it reclaims a stale
  m5term client without stopping the guest.
- No `udma0`: inspect `run-dual/nodeN/gem5.log` and
  `run-dual/udma-nodeN/udma.log`, then verify the official initramfs exists.
- Fabric not ready: inspect `run-dual/ub-switch/gem5.log` and the UDMA logs;
  every endpoint must complete both UB-HOST and UB-NET handshakes.
- A perftest waits at `Waiting for client`: two-sided SEND requires a server
  and client process. The IP address is only the control connection; EID/TP
  state routes UB payloads.
- KVM is slow with `--sync`: use default unsynchronized KVM for functional
  work, or Atomic/timing CPU for synchronized measurements.

The resolved manifest, process logs, gem5 `stats.txt`, and command/output must
be retained together for any reported experiment.
