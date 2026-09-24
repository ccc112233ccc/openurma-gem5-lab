# Mooncake native URMA bring-up

Date: 2026-09-22

This checkpoint builds the upstream Mooncake transfer-engine benchmark without
modifying Mooncake or UMDK source code, packages it into the official-UDMA
guest, and exercises its native `--protocol=ub` initialization path inside an
ARM64 gem5 full-system guest.

## Pinned inputs

- Mooncake: `1a0c0a44214ff61a8a4b2e9d90dfb023dd4703ed`
- official UMDK: `f84b90b8ddd8173b851334f55d332783d248bfc7`
- build mode: `Release`, `USE_UB=ON`
- Mooncake binary SHA-256:
  `10e57b04b6fb6151f0a0e3b253fe0f8428888ac0a23a07b7582507208d827b4e`
- packaged initramfs SHA-256:
  `86e49c50ebbbc6eb838e3bd746d8e482cab2d2c77b0a20c1f0c132570d11c5f5`

The resulting AArch64 executable has direct dynamic dependencies on
`liburma.so.0` and Mooncake's `libasio.so`. The builder also verifies that the
executable is AArch64 and that its command-line parser starts successfully
against the same UMDK libraries used in the guest.

## Reproduction

```bash
git clone https://github.com/kvcache-ai/Mooncake.git ../Mooncake
git -C ../Mooncake checkout 1a0c0a44214ff61a8a4b2e9d90dfb023dd4703ed
./scripts/build-mooncake-urma.sh
./scripts/package-mooncake-urma-initramfs.sh
./lab start --nodes 2 --profile fast --provider official \
  --network-backend builtin --ub-transport switch-adapter --mem-size 1GB
./lab sync
```

The first builder run creates an Ubuntu 22.04 ARM64 build image. Subsequent
builds are incremental. The benchmark is stripped only after ABI and startup
validation; this reduced it from 41 MiB to 1.3 MiB and the compressed initramfs
from 30 MiB to 15 MiB.

## Guest smoke test

Both guests contained the native executable and matching runtime libraries:

```text
/usr/bin/transfer_engine_bench  1.3M
/lib/libasio.so                 285.7K
/lib/liburma.so.0               687.4K
```

Both official devices were active. Node 0 exposed EID suffix `0100` and node 1
exposed `0101`:

```text
num  ubep_dev  tp_type  eid                                      link
0    udma0     UB       ...:0100                                 ACTIVE
```

The resource-creation probe on node 0 was:

```bash
transfer_engine_bench \
  --mode=target --protocol=ub --device_name=udma0 \
  --metadata_server=P2PHANDSHAKE \
  --local_server_name=10.0.0.1:12345 \
  --buffer_size=1048576 --batch_size=1 --block_size=4096 --threads=1
```

The gem5 device log then recorded resources created through the official stack:

```text
[NIC udma official] JFC id=0 ... depth=512 tid=3
[NIC udma official] JFC id=1 ... depth=1024 tid=3
[NIC udma official] JFR id=0 ... jfc=1 depth=1024 tid=3 payload_tid=3
[NIC udma official] Jetty id=1 ... send_jfc=0 jfr=0 recv_jfc=1 depth=1024 ...
[NIC udma official] JFC id=2 ... depth=512 tid=3
[NIC udma official] JFC id=3 ... depth=1024 tid=3
[NIC udma official] JFR id=1 ... jfc=3 depth=1024 tid=3 payload_tid=3
[NIC udma official] Jetty id=2 ... send_jfc=2 jfr=1 recv_jfc=3 depth=1024 ...
```

This proves the path reached Mooncake's upstream UB transport, `liburma`, the
official UDMA provider and kernel drivers, and finally the emulated UDMA command
and queue interface. It is more than a loader-only test.

The help text in this Mooncake revision omits `ub` from its protocol list, but
the same source file contains and executes the `FLAGS_protocol == "ub"` branch.
The successful resource creation above is the runtime check; no help text or
Mooncake source was patched locally.

## Remaining work

This checkpoint deliberately stops at local resource construction. A useful
READ/WRITE result still requires two changes:

1. make Mooncake's P2P metadata/control connection robust in the minimal guest;
2. move distributed virtual-time entry out of the `urma_perftest` wrappers so
   the whole Mooncake target/initiator process pair participates from startup.

After those are complete, run target on node 0 and initiator on node 1 for both
READ and WRITE, then collect virtual-time throughput and the UDMA/switch trace.
