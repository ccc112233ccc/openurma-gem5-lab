# OpenURMA Tier-G interactive initramfs

This directory builds an interactive ARM64 initramfs without changing the
OpenURMA checkout.  Unlike OpenURMA's demonstration `init.c`, this guest does
not run one command and power off: it loads the official kernel stack and then
keeps a BusyBox shell attached to the gem5 serial console.

The current saved checkpoint runs the unmodified official UBFI, UBUS, UMMU,
UBASE and UDMA modules through probe. UBASE creates `ubase.udma.0` and
`ubase.unic.0`; official `udma.ko` binds to the UDMA auxiliary device,
registers `udma0` with ubcore and creates `/dev/uburma/udma0`. The next
saved stage gives the official device a configurable management-plane EID and
reports its modeled 400-Gb/s port as `link ACTIVE`.  Official `urma_perftest`
now creates its context, queues, Jettys and process-scoped TPs through the
official stack and reaches the send/receive wait. An untagged development
snapshot now decodes the official queue contexts and connects their SQ, RQ,
doorbell and CQ memory to the modeled data path. End-to-end SQ-to-CQE runtime
validation is still in progress, so the last validated stage remains the
management-plane resource checkpoint.

## Required prebuilt inputs

- A built **OLK-6.6 ARM64** tree containing `vmlinux`, `ipv6.ko`, the official
  `ubcore.ko`, and the official `uburma.ko`.
- The OpenURMA `openurma_ubcore.ko` built against that exact kernel tree.
- The vendored UMDK cross-built for ARM64 (`liburma`, `liburma_common`,
  `urma_admin`, and `urma_perftest`).
- A statically linked ARM64 BusyBox. On an ARM64 Ubuntu container the
  `busybox-static` package normally supplies `/bin/busybox`.
- An ARM64 cross compiler and `cpio`.

OLK-6.6 is deliberate: the vendored UMDK v25.12.0 emits TLV ioctls, while the
older OLK-5.10 `uburma` module in the original demo script expects plain C
structures. The builder rejects that mismatched pairing and also verifies every
module's `vermagic` against the selected kernel.

## Build the image

The OLK checkout must live on the container's case-sensitive ext4 filesystem.
In the current lab it is `/opt/openurma-gem5-lab/oe66`; do **not** build the
host bind-mounted `openurma-gem5-lab/oe66` tree because macOS's default
case-insensitive filesystem aliases several distinct kernel paths.

Run the packager inside the ARM64 build container:

```bash
KSRC=/opt/openurma-gem5-lab/oe66 \
ARM_BUILD=/workspace/openurma-gem5-lab/artifacts/umdk-build \
BUSYBOX_ARM64=/bin/busybox \
OPENURMA_ROOT=/workspace/OpenURMA \
OUT=/workspace/openurma-gem5-lab/out/openurma-interactive.cpio.gz \
  /workspace/openurma-gem5-lab/build-interactive-initramfs.sh
```

Output:

```text
out/openurma-interactive.cpio.gz
out/openurma-interactive.manifest.txt
```

The script performs no downloads. It packages the already-built modules and
UMDK tools, builds the Tier-G `liburma_openurma.so` provider, the compact
`k_smoke` check, and the 14-case `k_dataplane` exercise; it recursively copies
their ARM64 shared-library dependencies and checks architecture plus kernel ABI
before creating the archive.

The generated manifest records the archive digest plus the exact kernel,
module, provider, helper and benchmark input paths and SHA-256 digests. The
default dual-node launcher verifies that contract before boot and refuses a
stale or truncated image. A deliberately supplied custom initramfs remains the
caller's own artifact contract.

If a module was built outside its usual tree, pass its full path with
`UBCORE_KO=...`, `UBURMA_KO=...`, `IPV6_KO=...`, or `OPENURMA_KO=...`.

## Boot and attach

From a host terminal, start the simulator in the foreground. The launcher sets
the required `M5_PATH` for the ARM boot loader and uses the persistent kernel
copy exported by `build_olk66.sh`:

```bash
bash /Users/caobo/workspace/openurma-gem5-lab/run.sh
```

gem5's ARM UART opens a TCP terminal inside the container, normally port 3456.
Keep the first terminal running and attach from a second host terminal:

```bash
bash /Users/caobo/workspace/openurma-gem5-lab/attach.sh
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

## Two independent interactive hosts

`run-dual.sh` starts two separate gem5 processes.  Each process owns an OLK-6.6
kernel, memory image, OpenURMA NIC and serial console.  The OpenURMA NICs exchange
timestamped UB packets over a shared mmap ring. A host-relayed e1000 link carries
only the stock `urma_perftest` TCP handshake and resource exchange; it is kept
outside the measured interval.

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
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh
```

It runs one `AtomicSimpleCPU` per guest with the UDMA provider and the same
400 Gbit/s direct UB link used by the detailed profiles. It keeps the official
kernel modules, UMDK libraries, `urma_admin`, and `urma_perftest`; only detailed
CPU/cache timing is bypassed. Use it for driver bring-up, command validation,
resource setup, and end-to-end traffic checks.

For a later timing/trend experiment, opt in explicitly to the server model:

```bash
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh --profile server
```

That profile still boots with `AtomicSimpleCPU`, switches to the four-core
`ArmO3CPU`/cache/DDR model for the measured ROI, and switches back afterward.

The older `udma400` profile remains as a fast `atomic_cache` diagnostic. All
three UDMA profiles use a 400 Gbit/s UB port and one serialization stage for the direct
link between the two simulated hosts. Their UDMA layout inputs are a 48-byte
SEND control area and 64-byte WQEBBs; direct WQE is disabled so the official
provider uses its normal memory-backed SQ. The defaults contain no fitted
device delays: switch, direct-WQE, SQ-fetch, per-WQEBB and synthetic
payload-DMA service terms are all zero.

To study the supplied two-node path through one L1 switch, change the topology
explicitly rather than fitting the direct-link defaults to its end-to-end
numbers. For example, two serialization stages model host-to-switch and
switch-to-host transmission; add only a switch delay justified by the switch
model or measurements:

```bash
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh \
  --profile server \
  --peer-serialization-stages 2 \
  --peer-switch-delay 0ns
```

The exact resolved values are written to `run-dual/run-manifest.txt`, so a
result never depends on an unreported preset.

The default one-way UB propagation delay is 100 ns. It is also the positive
lookahead: every ring DATA record carries an `arrival_tick`, timestamps are
monotonic in each direction, and a receiver may not consume the record early.
During the actual latency loop, gem5's distributed conservative barrier uses
the largest causal quantum by default: `quantum = lookahead = 100 ns`. This is
also stock `DistEtherLink`'s default relationship and avoids twice as many
barriers as a 50 ns quantum. Both values remain independently configurable, and
the launcher enforces `0 < quantum <= lookahead`. UMDK setup is left outside
this fine-grained epoch because synchronizing seconds of process startup at
nanosecond resolution is correct but needlessly slow.

```bash
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh
bash /Users/caobo/workspace/openurma-gem5-lab/status-dual.sh
# after both guests report "guest shell ready"
bash /Users/caobo/workspace/openurma-gem5-lab/sync-dual.sh
# after a completed timing run, require config + O3 + DMA evidence
bash /Users/caobo/workspace/openurma-gem5-lab/validate-server-profile.sh \
  --require-runtime
```

Inspect the resolved profile without starting guests, or select the old
no-cache/100G behavior explicitly:

```bash
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh --print-config
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh \
  --profile legacy
```

Every model input has both a command-line option and an environment equivalent.
Run `run-dual.sh --help` for the complete mapping. The principal controls are
`--cpu-mode`, `--cpu-freq`, `--peer-link-rate-gbps`,
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
bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh \
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
bash /Users/caobo/workspace/openurma-gem5-lab/attach-node0.sh
bash /Users/caobo/workspace/openurma-gem5-lab/attach-node1.sh
```

The fixed assignments are:

| guest | UART | hostname | EID | Ethernet OOB |
| --- | ---: | --- | --- | --- |
| node0 | 3460 | `openurma-node0` | `fe80::1` | `10.0.0.1/24` |
| node1 | 3470 | `openurma-node1` | `fe80::2` | `10.0.0.2/24` |

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
TCP connect. Correctness does not depend on simultaneous host submission: the
guest-side collective rendezvous freezes the first role until its peer arrives;
zero stagger simply avoids an unnecessary pre-test catch-up cost.

```bash
# quick functional run: defaults to 100 measured samples, 128-byte messages
bash /Users/caobo/workspace/openurma-gem5-lab/run-latency.sh

# comparison run matching the supplied hardware iteration count
bash /Users/caobo/workspace/openurma-gem5-lab/run-latency.sh \
  --profile ctp-rm-send-imm-i128 --samples 16384 --size 128

# ask the instrumented stack to delimit/reset/dump gem5 ROI statistics
bash /Users/caobo/workspace/openurma-gem5-lab/run-latency.sh \
  --roi-stats --samples 16384 --size 128
```

The helper prints a tab-separated summary with one row per node after the raw
UART transcript. Use `--format tsv` for machine-readable output only and
`--raw-output FILE` to retain the complete transcript. The pre-profile behavior
is still available as `--profile legacy`.

To run the same test manually in the two consoles, start node0 and then node1.
The first role waits at the guest-side virtual-time rendezvous; the host helper
above remains the reproducible and faster route:

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
bash /Users/caobo/workspace/openurma-gem5-lab/sweep-latency.sh \
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
collective ON/OFF rendezvous before opening the TCP control connection. Thus a
manually started server waits for the client in virtual time instead of racing
ahead according to host wall time. After setup, perftest performs its last TCP
readiness handshake, collectively enables dist-gem5 synchronization, runs the
latency loop, then collectively disables synchronization before reporting.
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
A finite 64-slot peer ring still applies backpressure and retries an unconsumed
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
verified range, not a claim that every larger provider-advertised size already
works. Remote token-value and access-permission fault enforcement also remains
a later correctness gate; the present path validates registered-address
translation, payload movement, ordering and completion behavior.

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

An older fitted experiment is retained under
`sweeps/udma400-calibrated-final-n16384/` only as an archived regression
artifact. It used a switched-path hardware curve to tune a different topology
and is not a default, a physical-model validation, or the basis for current
trend analysis.

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
bash /Users/caobo/workspace/openurma-gem5-lab/stop-dual.sh
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
| gem5 | `v24.0.0.1`, commit `b1a44b89c7bae73fae2dc547bc1f871452075b85` |
| OpenURMA | `0ae5dce300154d761f97095864bda0cf2546b265` |
| OpenClickNP | `c1c6acc58032a1894507d88659b3cca668b0e1a5` |
| vendored UMDK | `4eab3e4ad170b06bfe5d5c1014341e81edb9bf58` |
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
docker exec -it openurma-repro-20260909 bash -lc \
  'JOBS=1 /workspace/openurma-gem5-lab/build_gem5.sh'
```

`JOBS` defaults to 1 and uses gold's low-memory link mode to stay below the 8 GiB container limit. The script refuses a wrong checkout or a concurrent gem5
build, verifies/creates the three `/home/ubuntu` compatibility symlinks, links
the OpenURMA device directory into gem5, applies the SystemC deschedule patch
idempotently, installs both checked-in fixed topology sources, builds
`libopenurma_sc_tlm.a` against gem5's embedded SystemC ABI, builds `m5term`, and
finally invokes `scons --linker=gold --limit-ld-memory-usage
build/ARM/gem5.opt USE_SYSTEMC=1` with
`EXTRAS=/workspace/OpenURMA/eval/twonode/gem5_scaffold/src`. `EXTRAS` is
required because Python's source-tree walk does not follow the
`src/dev/openurma` symlink; without it, gem5 can finish successfully while
silently omitting `UBController` and `NICTopologySC`. No top-level `SConstruct`
edit is needed.
