# Official UDMA dual-node perftest evidence

Validation date: 2026-09-17

## Configuration

- two independent gem5 full-system processes and two independent OLK 6.6
  guests;
- `atomic_fast`, one CPU per guest;
- unmodified official `ubfi.ko`, `ubus.ko`, `ummu-core.ko`, `ummu.ko`,
  `hisi_ubus.ko`, `ubase.ko`, `udma.ko`, `ubcore.ko` and `uburma.ko`;
- stock UMDK `liburma-udma.so` and `urma_perftest`;
- node 0 `udma0` EID `...:0100`, node 1 `udma0` EID `...:0101`;
- 400-Gbit/s peer serialization, 100 ns one-way propagation and 100 ns
  conservative synchronization quantum.

The official image and nodes are created with:

```sh
docker exec openurma-repro-20260909 \
  bash /workspace/openurma-gem5-lab/official-udma/build_initramfs.sh

bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh \
  --profile fast --provider official
bash /Users/caobo/workspace/openurma-gem5-lab/sync-dual.sh
```

The recorded benchmark command is equivalent to a server on node 0 and a
client on node 1:

```sh
bash /Users/caobo/workspace/openurma-gem5-lab/run-latency.sh \
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
