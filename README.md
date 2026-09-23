# OpenURMA + gem5 多节点全系统仿真实验室

本仓库从公开源码开始，构建可交互的 ARM64 gem5 全系统环境。两个或更多独立 guest
运行 openEuler OLK 6.6、官方 `ubcore/uburma/ubagg` 驱动、官方 UMDK/UDMA
provider、`urma_admin` 和 `urma_perftest`；缺失的 UDMA/UB 硬件行为由 gem5
设备模型补齐。当前版本已验证设备发现、EID 与链路状态、Jetty/TP、SQ/RQ/CQ、
SEND/SEND_IMM、RMA READ/WRITE、双物理端口、TP 选路、L1 switch 拓扑和官方
bonding 逻辑设备数据面。

仓库只长期保存三类内容：构建与运行脚本、恢复精确源码版本所需的 patch/bundle、
以及每个关键结论的一份精简报告。`gem5/`、`sources/`、`artifacts/`、`system/`
和当前的 `run-dual/` 都是本地可再生工作目录，不提交 Git；重新启动实验时只保留
最新一份 `run-dual/`。需要留档的测试应使用脚本的结果目录或 `--raw-output` 参数。

## 从零开始

正式支持两种入口：ARM64 Ubuntu 22.04 原生运行，以及 Apple Silicon Mac 上的
Docker Desktop。两者使用同一套源码获取、构建和校验脚本；Docker 只提供固定的
Ubuntu 环境，不包含另一套实验实现。至少预留约 8 GiB 内存、80 GiB 磁盘空间。
首次构建需要下载 gem5、OpenURMA、
OpenClickNP、openEuler UMDK/UMMU/OLK 和 gem5 ARM 固件，并编译内核与 gem5，
因此会花较长时间。后续执行是增量的。

### ARM64 Ubuntu 22.04 原生模式

```bash
git clone https://github.com/ccc112233ccc/openurma-gem5-lab.git
cd openurma-gem5-lab
./setup-native.sh --jobs 8
./run-dual-native.sh --profile fast --provider official
./status-dual-native.sh
./sync-dual-native.sh
```

连接两个串口：

```bash
./attach-node0-native.sh
./attach-node1-native.sh
```

原生入口会安装 `docker/ubuntu-22.04-packages.txt` 中的依赖。已经配置好依赖时可用
`./setup-native.sh --skip-deps`。停止实验使用 `./stop-dual-native.sh`；批量测试有
对应的 `run-latency-native.sh`、`run-paired-latency-native.sh` 和
`sweep-latency-native.sh`。

### Docker 模式

```bash
git clone https://github.com/ccc112233ccc/openurma-gem5-lab.git
cd openurma-gem5-lab
./setup-docker.sh --jobs 2
```

原有 `./setup.sh` 保留为 `setup-docker.sh` 的兼容入口。Docker 包装层完成以下工作：

1. 构建 Ubuntu 22.04 ARM64 工具容器；
2. 为 macOS 上的 OLK 源码提供大小写敏感卷；
3. 在容器里调用同一个 `setup-native.sh --skip-deps` 核心流程。

核心流程随后拉取固定源码版本、恢复 `patches/source/` 中的实验提交，编译 gem5、
OLK、官方 UMDK/UDMA provider 与驱动，并生成和校验 initramfs。

只验证源码拉取和补丁恢复，不进行长时间编译：

```bash
./setup-docker.sh --sources-only
```

完整构建成功后，以快速 CPU 模式启动官方完整驱动栈双节点：

```bash
./run-dual.sh --profile fast --provider official
./status-dual.sh
```

等 `status-dual.sh` 显示两个 `guest shell ready` 后，完成双端控制网络初始化：

```bash
./sync-dual.sh
```

再开两个终端连接串口：

```bash
./attach-node0.sh
./attach-node1.sh
```

两个 guest 都出现 shell 后，可以先运行：

```text
urma_admin show
```

随后在 node0、node1 分别运行：

```text
# node0
ou-lat-server --profile ctp-rm-send-imm-i128 20 128 21115

# node1
ou-lat-client --profile ctp-rm-send-imm-i128 20 128 21115
```

看到 node0 的 `Waiting for client to connect...` 表示服务端正在正常等待 node1，
不是卡死。停止实验使用 `./stop-dual.sh`。更完整的 SEND/READ/WRITE 和包长扫描
命令见后文。

OLK-6.6 是有意选择的：当前官方 UMDK 使用 TLV ioctl，旧 OLK-5.10 的
`uburma` ABI 与之不匹配。构建脚本会拒绝混用版本。

### Mooncake 原生 URMA benchmark

将官方 Mooncake 仓库放在本实验仓库旁边，然后构建其原生
`transfer_engine_bench --protocol=ub`。脚本不会修改 Mooncake 或 UMDK 源码；
它使用与 guest 完全相同的 `urma_api.h`、`liburma.so` 和 ARM64 ABI，并记录两边
的精确 Git revision。首次运行会构建固定依赖的 ARM64 builder 镜像：

```bash
git clone https://github.com/kvcache-ai/Mooncake.git ../Mooncake
git -C ../Mooncake checkout 1a0c0a44214ff61a8a4b2e9d90dfb023dd4703ed
./scripts/build-mooncake-urma.sh
./scripts/package-mooncake-urma-initramfs.sh
```

上面是 Docker 默认入口；ARM64 Ubuntu 原生环境使用同一构建核心：

```bash
OPENURMA_EXECUTION_MODE=native ./scripts/build-mooncake-urma.sh
OPENURMA_EXECUTION_MODE=native ./scripts/package-mooncake-urma-initramfs.sh
```

默认锁定的 Mooncake revision 也记录在 `SOURCE_REVISIONS.md`；如需有意验证其他
版本，可在构建时显式设置 `MOONCAKE_REVISION=<commit>`。

生成物位于 `artifacts/mooncake-urma/`，并被加入
`out/official-udma.cpio.gz`。重新启动 guest 后可以先用
`transfer_engine_bench --help` 验证命令和依赖；双节点 READ/WRITE 命令会在
完成持续虚拟时间接入后固化在这里。当前原生 UB 资源创建的实测记录见
[`docs/mooncake-urma-bringup.md`](docs/mooncake-urma-bringup.md)。

## Boot and attach

From a host terminal, start the simulator in the foreground. The launcher sets
the required `M5_PATH` for the ARM boot loader and uses the persistent kernel
copy exported by `build_olk66.sh`:

```bash
./run.sh
```

gem5's ARM UART opens a TCP terminal inside the container, normally port 3456.
Keep the first terminal running and attach from a second host terminal:

```bash
./attach.sh
```

If gem5 reports a different listener port, pass it explicitly, for example
`OPENURMA_M5TERM_PORT=3457 bash .../attach.sh`. Both processes run in the same
container, so no Docker host-port publication is required.

The lab-local full-system wrapper disables `FEAT_HCX` and `FEAT_SME` in gem5's
advertised ARM release. Without that compatibility setting, OLK 6.6 performs
early EL2 register accesses which gem5 v24 traps before usable exception vectors
are installed, leaving the simulated CPU looping at physical address `0x200`.

At the prompt:

```text
(openurma-gem5) /root # ou-status
(openurma-gem5) /root # urma_admin show
(openurma-gem5) /root # ou-smoke
(openurma-gem5) /root # ou-dataplane
(openurma-gem5) /root # dmesg | tail -n 80
```

Use `~.` at the start of a line to detach `m5term` without stopping the guest;
these are two terminal escape keystrokes, not a guest shell command. Closing a
host terminal window can leave Docker's old `m5term` process alive. The next
`attach*.sh` invocation automatically reclaims only the old client on that same
UART. Set `OPENURMA_ATTACH_TAKEOVER=0` if an intentional attachment in another
host terminal must be protected. Use `poweroff -f` to stop the guest. Exiting
the shell intentionally starts a fresh shell so an accidental `exit` does not
kill PID 1.

## Independent interactive hosts

`run-dual.sh` starts two separate gem5 processes by default. Each process owns an OLK-6.6
kernel, memory image, OpenURMA NIC and serial console. By default a third
`ub-switch-sim` process connects them through two point-to-point adapters. Each
physical link has independent full-duplex per-port queues; the switch, rather
than either endpoint, owns forwarding, port mapping, egress serialization and
switch delay. A host-relayed e1000 link carries
only the stock `urma_perftest` TCP handshake and resource exchange; it is kept
outside the measured interval.

### ns-3-UB network-process bring-up

The first ns-3-UB integration checkpoint can replace the built-in L1 switch
process while preserving the current version-3 physical-port Adapter ABI.  The
official guest software, UDMA queues, DMA and completion semantics remain in
gem5; the independent ns-3 process uses ns-3 packet/time primitives for the
compatibility fabric and keeps EID
routing outside either endpoint.

Clone `ns-3-UB` next to this repository, initialize its submodules, then build
the Linux/aarch64 adapter in the existing OpenURMA container:

```bash
git clone https://gitcode.com/open-usim/ns-3-ub.git ../ns-3-ub
git -C ../ns-3-ub submodule update --init --recursive
OPENURMA_CONTAINER=openurma-repro-20260909 \
  ./scripts/build-ns3ub-adapter.sh
```

Start two guests with the compatibility bridge:

```bash
OPENURMA_CONTAINER=openurma-repro-20260909 \
  ./run-dual.sh --network-backend ns3ub-compat
```

`ns3ub-compat` is deliberately named as a transition mode.  It validates the
process boundary, shared-memory ABI, EID routing and ns-3 integration, but
its forwarding core is still arithmetic. The native-switch milestone is
available with:

```bash
OPENURMA_CONTAINER=openurma-repro-20260909 \
  ./run-dual.sh --network-backend ns3ub-native
```

`ns3ub-native` keeps the official software, UDMA device behaviour, source NIC
port and its serialization in gem5. At the switch-ingress timestamp it wraps
the opaque endpoint carrier in native UB headers and runs it through the
existing `UbSwitch` VOQ, allocator, egress `UbPort`, and `UbLink`. Flow control
is intentionally disabled for this first lossless-path checkpoint; routing,
queuing, switch-port serialization and egress propagation are no longer
adapter arithmetic. The ownership rules and delivery gates are documented in
[`docs/ns3ub-network-boundary.md`](docs/ns3ub-network-boundary.md).

The same launcher supports 2 through 8 guests. Every guest attaches to one
shared UB switch and one shared learning Ethernet control network. The UB
switch routes each DATA/SYNC record by destination EID, so communication is
not restricted to adjacent node numbers:

```bash
./run-dual.sh --nodes 4 --profile fast --provider official \
  --sync-mode adapter-local
./sync-dual.sh
# client node3 -> server node0
./run-node-pair-latency.sh 0 3 100 128 21115
# optional: run adjacent pairs concurrently as a scaling workload
./run-paired-latency.sh --samples 100 --size 128
./attach-nodeN.sh 2
```

`-S 10.0.0.X` selects the remote userspace process for the official TCP
resource exchange. That exchange carries the remote UB EID into the official
kernel `GET_TP_LIST` request. The modeled device records the resulting
`TPN -> destination EID` relation, reads the TPN from each official SQE, and
puts the EIDs in the Adapter header. The independent switch alone decides the
egress endpoint. Thus the IP is control-plane addressing; UB payload delivery
is EID based. Adapter-local synchronization waits only for the EID peer of the
active session, whereas `global-barrier` makes every gem5 instance participate
in one dist-gem5 barrier.

The detailed `server` profile is a reduced-core Arm server slice: four 3 GHz
`ArmO3CPU` cores with an explicit 8-wide front/back end, 192-entry ROB,
64-entry IQ, 32/32-entry load/store queues, four load ports and two store
ports. The integer, floating-point and vector rename files are explicitly
256 entries; the model uses gem5's deterministic `TournamentBP` and
`DefaultFUPool` rather than leaving an effectively unlimited 200-port memory
front end in place. Each core owns a 64 KiB L1I, 64 KiB L1D and 1 MiB L2; all
four share a 32 MiB L3 on a 2 GHz, 64-byte uncore fabric. The L1I/L1D/L2/L3
miss resources are respectively 8/16/32/64 MSHRs; their target and write
queues, LRU policy, disabled prefetchers, clusivity, coherent-bus pipelines
and snoop-filter capacities are all explicit and recorded.

Memory is a capacity-consistent 64 GiB, eight-channel DDR4-2400 model with
128-byte channel interleaving, `RoRaBaCoCh` mapping and no XOR hashing. Every
channel has one rank, a 64-entry read-burst queue, a 128-entry write-burst
queue, open-adaptive pages and FR-FCFS scheduling; the controller pipeline and
write-drain thresholds are explicit as well. Linux is capped at 8 GiB only to
avoid spending boot time initializing unused pages; the DRAM geometry and
timing model remain 64 GiB. The benchmark process is pinned to CPU 2.

Boot and idle execution use `AtomicSimpleCPU` so an interactive pair remains
practical. After both nodes finish the final distributed rendezvous,
`urma_perftest` switches all cores to O3 immediately before its warm-up and
measured loop, then switches them back before synchronization is disabled.
Do not pre-switch a guest manually; `ou-cpu-switch` is intentionally a
diagnostic guard because an unmatched toggle would invalidate the next run.

### Fast functional profile

The default profile is now the shortest path to an interactive, two-node
functional check:

```bash
./run-dual.sh
```

It runs one `AtomicSimpleCPU` per guest with the UDMA provider and the same
400 Gbit/s-per-port switched UB fabric used by the detailed profiles. It keeps the official
kernel modules, UMDK libraries, `urma_admin`, and `urma_perftest`; only detailed
CPU/cache timing is bypassed. Use it for driver bring-up, command validation,
resource setup, and end-to-end traffic checks.

For a later timing/trend experiment, opt in explicitly to the server model:

```bash
./run-dual.sh --profile server
```

That profile still boots with `AtomicSimpleCPU`, switches to the four-core
`ArmO3CPU`/cache/DDR model for the measured ROI, and switches back afterward.

The older `udma400` profile remains as a fast `atomic_cache` diagnostic. All
three UDMA profiles use 400 Gbit/s per UB port and one serialization stage on
each host-to-switch link. Their UDMA layout inputs are a 48-byte
SEND control area and 64-byte WQEBBs; direct WQE is disabled so the official
provider uses its normal memory-backed SQ. The defaults contain no fitted
device delays: switch, direct-WQE, SQ-fetch, per-WQEBB and synthetic
payload-DMA service terms are all zero.

The default topology is an explicit independent L1 switch rather than a
direct-link timing approximation. Each hop owns a 400-Gbit/s serialization queue and the configured link
propagation delay. The switch has independent output-port queues and an optional
fixed forwarding delay:

```bash
./run-dual.sh \
  --profile server --ub-transport switch-adapter --ub-port-count 2 \
  --peer-topology l1-switch --peer-port-map 0,1 \
  --peer-port-selection tp-context \
  --peer-switch-delay 0ns
```

`--peer-port-map 0,1` means source port 0 exits the switch toward remote port 0
and source port 1 exits toward remote port 1. The mapping is an experiment
input, not hard-coded wiring: `1,0` crosses the paths, while `0,0` makes both
source ports contend for remote port 0. In `l1-switch` mode, the default
100-ns link delay is charged once from host to switch and once from switch to
the remote host. `direct` mode charges it once end to end.

The exact resolved values are written to `run-dual/run-manifest.txt`, so a
result never depends on an unreported preset.

`--ub-transport direct-ring --peer-topology direct` retains the previous
endpoint-to-endpoint transport for A/B regression. In the default
`switch-adapter` mode, node 0 and node 1 no longer share one UB data ring:
they map `/tmp/openurma-dual.node0.adapter` and
`/tmp/openurma-dual.node1.adapter` respectively, while the switch maps both.
The common 64-byte Adapter header supports versioned `DATA` and `SYNC` records
and carries source and destination EIDs.
With the default two nodes, `adapter-local` therefore has exactly three timed
simulator processes: gem5 node 0, the UB switch, and gem5 node 1. An N-node run
has N gem5 processes plus the same switch process; there is no dist-gem5
switch. `--sync-mode global-barrier` retains that extra global synchronization
process as an explicit compatibility/reference mode.

The default one-way UB propagation delay is 100 ns. It is also the positive
lookahead: every Adapter DATA record carries a `receive_tick`, and a receiver
may not consume the record early. Arrival times are monotonic on each physical port;
they need not be globally monotonic when several ports transmit concurrently.
Every point-to-point Adapter gives each direction and physical port its own
64-slot FIFO. Every record carries explicit source and destination port IDs.
The receiver merges ready queue heads by virtual arrival time, so a delayed
packet on one port cannot block an already-arrived packet on another port.
During the actual latency loop, each endpoint publishes a `SYNC` promise on the
same per-port FIFO as `DATA`. The switch advances the promise through the same
propagation and serialization state as traffic, so a receiver runs only to the
earliest promised virtual tick. Monotonic ON/OFF generations collectively
enter and leave this mode even when the two guests reach the ROI at different
virtual times. UMDK setup is left outside this fine-grained epoch because
synchronizing seconds of process startup at nanosecond resolution is correct
but needlessly slow. In compatibility mode, the launcher still enforces
`0 < --sync-quantum-ns <= lookahead` for the dist-gem5 barrier.

Each simulated socket can advertise multiple physical UB ports without turning
them into one wider link. For example, the current dual-port bring-up is:

```bash
./run-dual.sh \
  --profile fast --provider official --ub-port-count 2
```

The official UBUS discovery response contains two active physical-port TLVs.
The official UDMA provider still exposes one logical `udma0` device, which is
normal for this topology. By default, the simulator no longer invents an
egress port from Jetty, token or opcode fields. The unmodified provider writes
the official 24-bit TPN into each SQE; the unmodified kernel/control path first
obtains that TPN with `GET_TP_LIST` and activates it with `ACTIVE_TP`; the
modeled hardware then resolves the active TPN to the port selected for that TP.
All fragments of one WRITE stay on that port, and READ responses and WRITE
ACKs return through the request's ingress port. `--peer-port-selection
legacy-hash` exists only for explicit comparison with the earlier mock path.

Each port owns an independent 400-Gbit/s serialization timeline and
cross-process queue, so TPs assigned to different ports can progress
concurrently while one TP retains packet order. The current direct wiring is
port 0 to port 0 and port 1 to port 1; switch mode uses the explicit port map
described above. This checkpoint implements single-port TP pinning. It does
not yet implement the official bonding-group table and hardware hash required
for one TP to stripe over several ports, two NUMA sockets, or a separate UDMA
instance per socket.

```bash
./run-dual.sh
./status-dual.sh
# after both guests report "guest shell ready"
./sync-dual.sh
# after a completed timing run, require config + O3 + DMA evidence
./validate-server-profile.sh \
  --require-runtime
```

Inspect the resolved profile without starting guests, or select the old
no-cache/100G behavior explicitly:

```bash
./run-dual.sh --print-config
./run-dual.sh \
  --profile legacy
```

Every model input has both a command-line option and an environment equivalent.
Run `run-dual.sh --help` for the complete mapping. The principal controls are
`--cpu-mode`, `--cpu-freq`, `--ub-port-count`, `--peer-topology`,
`--peer-port-map`, `--peer-port-selection`, `--peer-link-rate-gbps`,
`--peer-serialization-stages`, `--peer-switch-delay`,
`--peer-link-overhead-bytes`, `--sq-control-bytes`, `--wqebb-bytes`,
`--sq-sge-bytes`, `--direct-wqe-max-blocks`,
`--direct-wqe-latency`, `--sq-fetch-latency`, `--sq-wqebb-latency`,
`--payload-dma-latency`, `--payload-dma-rate-gbps`,
`--udma-poll-interval`, `--udma-iotlb-entries`, `--dma-max-outstanding`, the
`--o3-*`, per-level cache queue, coherent/memory/I/O bus, I/O-cache and memory
controller controls. Memory address mapping, interleave/XOR and snoop-filter
capacity are explicit as well. For example, this A/B
changes only the CPU interpretation while retaining the structural `udma400`
link and UDMA settings:

```bash
./run-dual.sh \
  --profile udma400 --cpu-mode atomic_hot
```

Command-line values override environment values, which override profile
defaults. Each launch writes every resolved model input to
`run-dual/run-manifest.txt`; retain that file with results. `legacy` selects the
former `atomic` CPU, 100 Gbit/s single-stage serialization and disables the new
front-end costs, so old experiments remain comparable.

In the `server` ROI, CPU instruction/data traffic, SQ stores, CQ polling and
page-table work use O3 plus the timing cache/DDR hierarchy. Device-initiated
SQ fetches, payload reads/writes and CQE writes enter the same hierarchy through
a native gem5 `RequestPort`: it uses `sendAtomic` only during boot and changes
to `sendTimingReq` with the O3 ROI. Transfers are split at cache-line and 4 KiB
boundaries and permit 16 requests in flight by default. The port follows the
standard device path through `iobus` and a coherent I/O protocol-adapter cache
before the memory bus. The I/O bus shares the 2 GHz, 64-byte fabric and records
its 2/1/2/1-cycle frontend/forward/response/header pipeline. The adapter
defaults to 1 KiB, 8-way, one-cycle
tag/data/response at the 2 GHz fabric clock, with 32 MSHRs, targets and write
buffers. All of those values are overrideable and recorded. This makes DMA
participate in ownership/invalidation traffic, fabric backpressure and
DRAM-controller queueing. The UDMA context
control ABI carries the creating process's page-table root explicitly, so a
benchmark pinned to CPU 2 is not translated through CPU 0's address space.
The NIC has a context-tagged, fully associative 4 KiB IOTLB with deterministic
true-LRU replacement. Its default capacity is 64 entries, `0` disables it,
and misses perform real timing DMA reads for each guest page-table level;
there is no configured hit rate or fitted miss delay. Context and queue/MR
lifecycle events invalidate the exact cached ranges they own. Override the
capacity with `--udma-iotlb-entries` (or
`OPENURMA_UDMA_IOTLB_ENTRIES`) for sensitivity runs.
These mechanisms replace the former functional DMA bridge; the default fixed
device-service and fitted bandwidth terms remain zero.

Open two more host terminals:

```bash
./attach-node0.sh
./attach-node1.sh
```

The fixed assignments are:

| guest | UART | hostname | EID | Ethernet OOB |
| --- | ---: | --- | --- | --- |
| node0 | 3460 | `openurma-node0` | `...:0100` | `10.0.0.1/24` |
| node1 | 3470 | `openurma-node1` | `...:0101` | `10.0.0.2/24` |

For larger runs, UARTs continue at a stride of 10, EIDs continue from
`...:0102`, and OOB addresses continue as `10.0.0.(node+1)` on the shared
learning Ethernet switch. For example node3 uses UART 3490, EID `...:0103`
and OOB `10.0.0.4`.

First prove that payload bytes cross the two independent guest memories.  Run
the server first:

```sh
# node0
ou-peer-server

# node1, in the other console
ou-peer-client
```

For a synchronized `send_lat` through the official UDMA provider/API path, the
host helper starts both sides and automatically enters/leaves the conservative
epoch around the ping-pong loop. The vendored `urma_perftest` itself carries the
gem5 rendezvous, ROI-switch and statistics instrumentation; those additions do
not alter the provider ABI or WQE/CQE implementation.
Its default benchmark profile is an exact, explicit comparison configuration:
CTP, RM (`-p 0`), SEND_IMM, `-I 128`, and `-J 1`.
The two UART commands pass through one host-side start gate and are injected
without a wall-clock stagger by default. The UMDK client already retries its
TCP connect. Fine-grained synchronization is enabled only after the TCP
exchange and TP setup have established the peer EID; zero stagger simply
reduces unnecessary setup wait.

```bash
# quick functional run: defaults to 100 measured samples, 128-byte messages
./run-latency.sh

# comparison run matching the supplied hardware iteration count
./run-latency.sh \
  --profile ctp-rm-send-imm-i128 --samples 16384 --size 128

# ask the instrumented stack to delimit/reset/dump gem5 ROI statistics
./run-latency.sh \
  --roi-stats --samples 16384 --size 128
```

The helper prints a tab-separated summary with one row per node after the raw
UART transcript. Use `--format tsv` for machine-readable output only and
`--raw-output FILE` to retain the complete transcript. The pre-profile behavior
is still available as `--profile legacy`.

To run the same test manually in the two consoles, start node0 and then node1.
The server waits in the official TCP control path until the client connects;
the host helper above remains the reproducible and faster route:

```sh
# node0
ou-lat-server --profile ctp-rm-send-imm-i128 100 128 21115

# node1
ou-lat-client --profile ctp-rm-send-imm-i128 100 128 21115
```

Here `100` means 100 reported samples: the wrappers add the timestamps needed
for five discarded warm-up deltas, enable distributed virtual-time sync, and
run CTP/RM/SEND_IMM with `-J 1 -I 128 -l 1`. To invoke `urma_perftest` directly,
use node0 as the server (no `-S`) and point node1's TCP control connection at
node0. `-S` is the Ethernet control-plane address, not the UB EID:

```sh
# node0
OPENURMA_DIST_SYNC=1 LD_LIBRARY_PATH=/lib:/usr/lib \
taskset 4 urma_perftest send_lat -d openurma0 --eid_idx 0 --ctp -s 128 \
  -P 21115 -J 1 -I 128 -l 1 -n 26 --enable_imm -p 0

# node1
OPENURMA_DIST_SYNC=1 LD_LIBRARY_PATH=/lib:/usr/lib \
taskset 4 urma_perftest send_lat -d openurma0 --eid_idx 0 --ctp -s 128 \
  -P 21115 -J 1 -I 128 -l 1 -n 26 --enable_imm -p 0 -S 10.0.0.1
```

The direct form uses `-n 26` to report 20 post-warm-up latency samples and
BusyBox's hexadecimal CPU mask `taskset 4` to pin the process to CPU 2.
Lowercase `-l` is the JFS post-list size; uppercase `-I` is the inline threshold.

After editing these guest wrappers, rebuild the interactive initramfs before
booting a new pair. Both wrappers also accept `--roi-stats`, or equivalently
`OPENURMA_ROI_STATS=1`.

For a reproducible size curve, use the sweep helper after `sync-dual.sh`:

```bash
./sweep-latency.sh \
  --profile ctp-rm-send-imm-i128 --samples 16384
```

The default sizes are
`2 4 8 16 17 32 64 65 80 81 128 129 192 193 256 512 1024 1025 2048 4096`.
They expose the 16/17-byte and 80/81-byte inline WQEBB transitions, the
64/65-byte receive-DMA cache-line transition, the 128/129-byte inline cutoff,
and representative non-inline DMA cache-line transitions instead of inferring
them between powers of two. Each new timestamped directory contains
`results.tsv`, a sweep manifest, the resolved model manifest, and one raw
dual-UART transcript per message size. With `--roi-stats`, it also
extracts exactly the newly appended gem5 statistics block for each node and
size instead of accidentally selecting the final whole-run block. Pass sizes
positionally or with `--sizes`; the helper refuses to overwrite a non-empty
result directory.

Both wrappers set `OPENURMA_DIST_SYNC=1`; the patched perftest first performs a
normal TCP/resource exchange and TP creation while fine-grained synchronization
is off. After setup supplies the peer EID, perftest performs its last TCP
readiness handshake, collectively enables EID-scoped Adapter synchronization,
runs the latency loop, then collectively disables synchronization before
reporting. Starting the server first is safe because it blocks in the official
TCP accept path rather than advancing the measured virtual-time epoch.
The same unmodified guest command selects the legacy dist-gem5 implementation
when the launcher is run with `--sync-mode global-barrier`.

The original Python-stepped comparison is retained in
[`results/sync-ab-20260921/REPORT.md`](results/sync-ab-20260921/REPORT.md), and
the replacement C++ event-queue implementation is profiled in
[`results/sync-cpp-ab-20260921/REPORT.md`](results/sync-cpp-ab-20260921/REPORT.md).
Adapter-local no longer returns through Python for each lookahead interval.
The NIC's C++ event drains DATA/SYNC records, polls until the peer publishes a
future conservative horizon, and re-schedules itself at that tick. Because the
guests boot independently, each synchronized ROI uses phase-relative wire
timestamps; the switch busy-polls only while both endpoints are in that ROI.
In the measured two-node case this makes 128-byte runs slightly faster than the
global barrier, while 4096-byte runs remain slightly slower. A four-node run
with two concurrent 128-byte pairs retained identical virtual latency results
and widened the steady-state wall-time advantage from about 3.8% to about 6%.
The raw transcripts and methodology are in
[`results/sync-nnode-20260921/REPORT.md`](results/sync-nnode-20260921/REPORT.md).
That report is the historical fixed-pair scaling checkpoint. The subsequent
arbitrary-EID implementation and cross-pair SEND/READ/WRITE validation are in
[`results/eid-routing-20260921/REPORT.md`](results/eid-routing-20260921/REPORT.md).
In iteration mode, the optional stats reset is immediately before the first
post-warm-up timestamp and its dump immediately follows the timestamp closing
the final reported delta, so the ROI block covers the same samples as the
latency report.
Do not enable only one side: the pseudo-op is collective by design.
The requested count is the number of reported samples: the wrappers add six
exchanges so perftest can form deltas and exclude five explicit startup deltas
as a steady-state warm-up before computing percentiles. Distributed latency
mode currently requires exactly one Jetty/pair, and the fixed ring slot supports
messages up to 8088 bytes; unsupported settings fail instead of printing a
plausible but invalid report.

Iteration-mode, unidirectional `send_bw` supports the same collective virtual-
time boundary for one pair and one Jetty. Use an inline threshold of 64 bytes
for the official provider: a 128-byte inline WQE occupies three WQEBBs while
the provider allocates this SQ at two WQEBBs per advertised entry, so `-I 128`
can exhaust the physical SQ before perftest's WQE-depth accounting polls a CQE.

```sh
# node0 (server)
OPENURMA_DIST_SYNC=1 urma_perftest send_bw -d udma0 --eid_idx 0 --ctp \
  -a12 -P 21119 -J 1 -I 64 -n 1024 -l 16 -Q 16 -p 0

# node1 (client/sender)
OPENURMA_DIST_SYNC=1 urma_perftest send_bw -d udma0 -S 10.0.0.1 \
  --eid_idx 0 --ctp -a12 -P 21119 -J 1 -I 64 \
  -n 1024 -l 16 -Q 16 -p 0
```

The validated 400-Gbit/s, 100-ns direct-link run produced these sender-side
average values (the tool labels binary MiB/s as `MB/sec`):

| bytes | average MiB/s | message rate Mpps |
| ---: | ---: | ---: |
| 128 | 1,220.22 | 9.9960 |
| 512 | 4,880.88 | 9.9960 |
| 1024 | 9,757.98 | 9.9922 |
| 2048 | 19,523.53 | 9.9960 |
| 4096 | 39,047.05 | 9.9960 |

The 400-Gbit/s payload ceiling is 47,683.7 MiB/s. With the current roughly
100-ns message issue/synchronization cadence, the largest legal CTP SEND,
4096 bytes, reaches about 81.9%. The official UMDK release specification caps
CTP/RM SEND messages at 4 KiB (single-path TP/RC has a separate 64-KiB limit),
so values above 4096 bytes are outside this experiment's valid domain. Earlier
5120- and 6144-byte simulator probes only exposed a missing device-limit check
in the model; they are not hardware-comparable SEND results and are deliberately
excluded here. Therefore this single-Jetty CTP SEND experiment does not reach
the 400-Gbit/s line-rate plateau within the supported message-size range. Use a
proper WRITE data path or additional legal parallel streams to study saturation.
A finite 64-slot ring on each physical port still applies backpressure and retries an unconsumed
SQ WQE; ring-full is no longer treated as a simulator panic. These numbers are
SEND results and must not be reported as `write_bw` results. The independent
RMA READ/WRITE path described below is used for larger transfers.

### Official UDMA READ/WRITE bandwidth

The model now executes the official provider's READ (WQE opcode 6) and WRITE
(WQE opcode 3) paths. A WRITE DMA-reads the initiator SGE, fragments it onto
the simulated 400-Gbit/s link, translates the registered remote virtual address
through the target guest's active UMMU context, DMA-writes target memory, and
returns one completion only after the final fragment is applied. A READ sends a
request to the target, DMA-reads target memory, returns one or more response
fragments, DMA-writes the initiator SGE, and then produces one completion. The
target NIC progresses this work asynchronously; it does not require a target
receive WQE or a userspace receive loop.

Use uppercase `-B` (bidirectional), not lowercase `-b`, on both nodes. The
simulator-only perftest synchronization patch keeps the existing TCP setup and
report exchange outside the 100-ns conservative synchronization epoch and
enables distributed virtual time only around the measured RMA loop. The
official OLK drivers and `liburma-udma.so` provider remain unmodified.

```sh
# WRITE: node0 first, then node1
OPENURMA_DIST_SYNC=1 urma_perftest write_bw -d udma0 --eid_idx 0 --ctp \
  -B -s 8192 -P 21252 -J 1 -I 64 -n 5 -l 1 -Q 1 -p 0
OPENURMA_DIST_SYNC=1 urma_perftest write_bw -d udma0 -S 10.0.0.1 \
  --eid_idx 0 --ctp -B -s 8192 -P 21252 -J 1 -I 64 \
  -n 5 -l 1 -Q 1 -p 0

# READ: node0 first, then node1
OPENURMA_DIST_SYNC=1 urma_perftest read_bw -d udma0 --eid_idx 0 --ctp \
  -B -s 8192 -P 21254 -J 1 -I 64 -n 5 -l 1 -Q 1 -p 0
OPENURMA_DIST_SYNC=1 urma_perftest read_bw -d udma0 -S 10.0.0.1 \
  --eid_idx 0 --ctp -B -s 8192 -P 21254 -J 1 -I 64 \
  -n 5 -l 1 -Q 1 -p 0
```

The following two-node smoke results were reproduced symmetrically on both
endpoints. As elsewhere, `MB/sec` is the benchmark's binary MiB/s label:

| operation | bytes | average MiB/s | message rate Mpps |
| --- | ---: | ---: | ---: |
| WRITE | 128 | 349.10 | 2.859803 |
| WRITE | 8192 | 22,217.84 | 2.843883 |
| WRITE | 65536 | 44,074.69 | 0.705195 |
| READ | 128 | 731.43 | 5.991863 |
| READ | 8192 | 22,923.99 | 2.934271 |
| READ | 65536 | 24,466.37 | 0.391462 |

Unlike CTP SEND's official 4-KiB message limit, RMA transfers are fragmented
internally and have been validated here through 64 KiB. This is the currently
advertised device-contract range: the modeled firmware reports 64 KiB in the
official driver's separate `max_read_size` and `max_write_size` fields.

A non-inline RMA WQE does not contain the payload. Its 48-byte control section
holds the remote segment/address and its 16-byte SGE holds local address,
length and token, so a one-SGE READ/WRITE remains one 64-byte WQEBB as transfer
length grows. The simulator therefore queues the WQE once and streams 8088-byte
maximum link fragments through a finite 64-slot physical-port ring. It never requires
all fragments to fit in the ring at once.

As a mechanism-only test beyond the advertised contract, 128-KiB WRITE and
1-MiB READ/WRITE also completed on both endpoints:

| operation | bytes | average MiB/s | link fragments per WQE |
| --- | ---: | ---: | ---: |
| WRITE | 131072 | 44,563.75 | 17 |
| WRITE | 1048576 | 47,983.57 | 130 |
| READ | 1048576 | 49,567.88 | 130 response fragments |

These rows prove fixed-size WQE decoding and multi-window streaming; they do
not raise the device capability exposed to the official driver. A future
hardware profile may supply larger firmware-derived READ/WRITE limits without
changing WQE layout. Remote token-value and access-permission fault enforcement
also remains a later correctness gate; the present path validates
registered-address translation, payload movement, ordering and completion.

There is a second, upstream `send_lat` sampling detail which matters when
comparing short and long runs. SEND-LAT actually defaults to a JFR depth of 512
(despite the current UMDK help text saying LAT defaults to one). It preposts 512
receive WQEs, reposts one after every receive while more than 512 iterations
remain, then stops reposting for the final 511 deltas. Reposting is inside the
measured ping-pong interval and includes the provider's receive-doorbell MMIO.
Consequently a 1001-sample run contains 490 steady repost deltas and 511 faster
drain deltas, so its median selects a different path from a 16384-sample run.
Use 16384 samples for the supplied hardware comparison (or at least more than
1022 when retaining the default JFR depth); the 100-sample default is intended
only as a quick functional run.

An older fitted experiment used a switched-path hardware curve to tune a
different topology. It is preserved in Git history only; it is not a default,
a physical-model validation, or the basis for current trend analysis.

The 2026-09-14 stock-UDMA validation below is historical and predates the
native timing-DMA, O3 server and IOTLB profile. In that older functional-DMA
run, both nodes completed the 128-byte inline path and the 4096-byte SGE path.
With 100 ns propagation and one 400-Gbit/s serialization stage, packet
timestamps differed by exactly 102.56 ns for 128 bytes and 181.92 ns for 4096
bytes. Keep those numbers only as a link-equation regression; they are not a
validation result for the current `server` profile.

`sync-dual.sh` also rejects a guest that did not register its architected timer.
The OLK 6.6 direct-boot wrapper deliberately omits VExpress's optional
memory-mapped timer node because its non-secure access normally has to be
initialized by firmware; the modeled CP15 timer remains available at 25.16 MHz.
Do not use a run whose boot log contains a 250 Hz `sched_clock` fallback for
latency statistics.

The conservative-clock check was previously tested with the `legacy` model by
sending `SIGSTOP` to node1 for 10 wall-clock seconds after ten bidirectional
exchanges. Both ring directions stopped together, resumed without a causality
error, and the 10 seconds did not appear in simulated latency: both nodes
reported a 19.03 us median and 19.05 us p99 for 100 measured 128-byte samples.
These numbers are a synchronization regression result, not a calibration target
for `udma400`. A separate 257-sample run completed with 263 produced and 263
consumed records in each direction, crossing the old model queue's 256-entry
failure point.

`-s` is the message size.  `-l` is the JFS post-list length, so do not use
`-l 128` to request a 128-byte message.  The current provider does not implement
the optional TP-aware API, so omit `--tp_aware`.

SEND receive completion is consumed when the receiver polls its CQ, which is
what `send_lat` does. READ and WRITE use the NIC worker's asynchronous peer-ring
path instead, so a passive RMA target does not need to post a receive WQE.

Stop only these two guests and their Ethernet relay with:

```bash
./stop-dual.sh
```

## Verified run (2026-09-10)

The rebuilt stack reached the interactive ARM64 guest and produced these
results through the stock UMDK userspace and kernel path:

```text
urma_admin show: openurma0  UB  eid0 fe80::1
ou-smoke:         exit=0, control-plane PASS + data-plane completion
ou-dataplane:     RESULT 14/14 verb checks passed, exit=0
```

The 14 checks cover WRITE, READ, CAS, SWAP, FADD, FSUB, FAND, FOR, FXOR,
SEND, SEND_IMM, WRITE_IMM, a second CAS case, and WRITE out-of-bounds error
handling.

## Pinned versions and reproducible gem5 build

This lab is pinned to repository object IDs so a moving branch cannot silently
change the simulator/kernel ABI:

| Component | Version or exact commit |
| --- | --- |
| gem5 | upstream `b1a44b89c7bae73fae2dc547bc1f871452075b85`, lab `724651433c9bdee2c7f0484ab85b9620b0810993` |
| OpenURMA | upstream `0ae5dce300154d761f97095864bda0cf2546b265`, lab `a49521580a27d8a3f66588342581fd9d73cfb42a` |
| OpenClickNP | `c1c6acc58032a1894507d88659b3cca668b0e1a5` |
| vendored UMDK | upstream `4eab3e4ad170b06bfe5d5c1014341e81edb9bf58`, lab `f84b90b8ddd8173b851334f55d332783d248bfc7` |
| openEuler OLK-6.6 | `5078a3a23a1e1825ec136485173ec98668cdd640` |

The ARM firmware/resource bundle is gem5's official
[`aarch-system-20220707.tar.bz2`](https://dist.gem5.org/dist/v22-0/arm/aarch-system-20220707.tar.bz2):

```text
SHA-256  1e49513d680ccd3c0dd32cbfcf7d3421b4bf8bb35f7c8467de9c863cf7cc1344
```

It is saved as `downloads/aarch-system-20220707.tar.bz2` and extracted into
`system/`. The current VExpress full-system configuration resolves both
`system/binaries/boot.arm64` and `system/binaries/boot.arm`; keep both files.
The kernel and initramfs are supplied separately and no disk image is used.

After any currently running SCons process has finished, reproduce the patched
simulator inside the ARM64 build container:

```bash
docker exec -it openurma-gem5-lab bash -lc \
  'JOBS=1 /workspace/openurma-gem5-lab/build_gem5.sh'
```

`JOBS` defaults to 1 and uses gold's low-memory link mode to stay below the 8 GiB container limit. The script refuses a wrong checkout or a concurrent gem5
build, verifies/creates the three `/home/ubuntu` compatibility symlinks, links
the OpenURMA device directory into gem5, applies the SystemC deschedule patch
idempotently, installs both checked-in fixed topology sources, builds
`libopenurma_sc_tlm.a` against gem5's embedded SystemC ABI, builds `m5term`, and
finally invokes `scons --linker=gold --limit-ld-memory-usage
build/ARM/gem5.opt USE_SYSTEMC=1` with
`EXTRAS=/workspace/openurma-gem5-lab/sources/OpenURMA/eval/twonode/gem5_scaffold/src`. `EXTRAS` is
required because Python's source-tree walk does not follow the
`src/dev/openurma` symlink; without it, gem5 can finish successfully while
silently omitting `UBController` and `NICTopologySC`. No top-level `SConstruct`
edit is needed.
