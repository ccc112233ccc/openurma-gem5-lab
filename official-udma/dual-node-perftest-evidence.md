# Official UDMA dual-node perftest evidence

Validation date: 2026-09-17

## Configuration

- two independent gem5 full-system processes and two independent OLK 6.6
  guests;
- `atomic_fast`, one CPU per guest;
- unmodified official `ubfi.ko`, `ubus.ko`, `ummu-core.ko`, `ummu.ko`,
  `hisi_ubus.ko`, `ubase.ko`, `udma.ko`, `ubcore.ko` and `uburma.ko`;
- stock UMDK `liburma-udma.so`; `urma_perftest` carries only the
  simulator-specific distributed-time synchronization boundary;
- node 0 `udma0` EID `...:0100`, node 1 `udma0` EID `...:0101`;
- 400-Gbit/s peer serialization, 100 ns one-way propagation and 100 ns
  conservative synchronization quantum.

The official image and nodes are created with:

```sh
docker exec openurma-gem5-lab \
  bash /workspace/openurma-gem5-lab/official-udma/build_initramfs.sh

./lab start \
  --profile fast --provider official
./lab sync
```

The recorded benchmark command is equivalent to a server on node 0 and a
client on node 1:

```sh
./lab latency \
  --samples 5 --size 128 --port 21116
```

Both endpoints returned zero. Parsed output:

```text
profile                       node   bytes iterations t_min_us t_max_us t_median_us t_avg_us t_stdev_us p99_us
ctp-rm-send-imm-i128          node0  128   5          0.42     0.44     0.44        0.43     0.01       0.44
ctp-rm-send-imm-i128          node1  128   5          0.42     0.44     0.44        0.43     0.01       0.44
```

The output explicitly reports five excluded synchronized warm-up deltas and
five measured samples. Both official processes deactivate their TP, query
Jetty flush state, destroy resources and return to the guest shell.

## Cross-node proof

The two gem5 logs contain complementary peer-ring events. For example, a
packet transmitted by node 1 at virtual tick `1918609994808` is published
with arrival tick `1918610097368` and consumed by node 0 after that timestamp.
The reply is then transmitted by node 0 and consumed by node 1. Each benchmark
invocation contributes 11 numbered 128-byte packets in each direction for the
warm-up and measured request/reply sequence. Two consecutive validation runs
therefore leave 22 TX and 22 RX peer-ring events in each node log, together
with matching UDMA receive completions.

Complete local evidence is retained in:

- `run-official-dual-v2-20260917/official-send-lat.txt`;
- `run-official-dual-v2-20260917/node0/gem5.log`;
- `run-official-dual-v2-20260917/node1/gem5.log`; and
- both nodes' `system.terminal` files.

Generated run artifacts are intentionally excluded from Git. No official OLK
driver or UMDK provider source file was modified for this result.

## RMA READ/WRITE gate

The same official stack now completes bidirectional `write_bw` and `read_bw`.
Uppercase `-B` is required: both independent guests issue work during the
conservative-time epoch. TCP setup and result exchange stay outside that epoch,
so the 100-ns synchronization quantum covers only the measured data path.

Representative 8-KiB commands are:

```sh
# node 0, start first
OPENURMA_DIST_SYNC=1 urma_perftest write_bw -d udma0 --eid_idx 0 \
  --ctp -B -s 8192 -P 21252 -J 1 -I 64 -n 5 -l 1 -Q 1 -p 0
# node 1
OPENURMA_DIST_SYNC=1 urma_perftest write_bw -d udma0 -S 10.0.0.1 \
  --eid_idx 0 --ctp -B -s 8192 -P 21252 -J 1 -I 64 \
  -n 5 -l 1 -Q 1 -p 0

# Replace write_bw with read_bw on both nodes and use a fresh port for READ.
```

Both endpoints returned zero for every row below and printed identical
averages:

| operation | bytes | iterations | average MiB/s | message rate Mpps |
| --- | ---: | ---: | ---: | ---: |
| WRITE | 128 | 5 | 349.10 | 2.859803 |
| WRITE | 8192 | 5 | 22,217.84 | 2.843883 |
| WRITE | 65536 | 5 | 44,074.69 | 0.705195 |
| READ | 128 | 5 | 731.43 | 5.991863 |
| READ | 8192 | 5 | 22,923.99 | 2.934271 |
| READ | 65536 | 5 | 24,466.37 | 0.391462 |

WRITE reads the local SGE through the initiator UMMU context, emits 8088-byte
maximum peer-link fragments, translates the registered remote VA through the
target's active UMMU context, and acknowledges only after target DMA completes.
READ emits a payload-free request; the target DMA-reads its registered memory
and streams response fragments that are DMA-written into the initiator SGE.
Each multi-fragment operation produces one official CQE (opcode 3 for WRITE,
opcode 6 for READ). An 8192-byte operation is visibly split into 8088+104 bytes
and a 65536-byte operation into nine fragments.

This extends the useful transfer range beyond CTP SEND's official 4096-byte
limit. The formal gate ends at the 64-KiB `max_read_size`/`max_write_size`
currently returned by the modeled firmware. Negative token/permission checks
remain separate gates. Complete local logs are retained under
`run-rma-final-20260917/` and are excluded from Git.

## Fixed-WQE large-transfer diagnostic

Validation on 2026-09-18 removed an artificial simulator restriction that
required every WRITE fragment to fit in the 64-slot peer ring simultaneously.
The simulator now accepts the WQE once and streams fragments as ring capacity
becomes available. This matches the official provider layout: the ordinary
RMA control section is 48 bytes and one SGE is 16 bytes, so a non-inline,
one-SGE READ or WRITE occupies one 64-byte WQEBB regardless of referenced
payload length.

The following deliberately out-of-contract diagnostics completed on both
endpoints with five iterations, one Jetty and one outstanding operation:

| operation | bytes | average MiB/s | fragments per WQE |
| --- | ---: | ---: | ---: |
| WRITE | 131072 | 44,563.75 | 17 |
| WRITE | 1048576 | 47,983.57 | 130 |
| READ | 1048576 | 49,567.88 | 130 response fragments |

Each endpoint returned zero. The 1-MiB commands used `-s 1048576 -n 5 -l 1
-Q 1 -B`; WRITE used port 21320 and READ used 21321. These results validate
WQE decoding, UMMU traversal and multi-window link streaming, but do not claim
that the current modeled device advertises a 1-MiB operation limit. That limit
remains 64 KiB until a hardware-backed firmware capability profile supplies a
larger value. Logs are retained in `run-rma-large-final-20260918/` and excluded
from Git.
