# Official UB aggregation dataplane stage

This stage runs the stock OLK 6.6 UB/UMMU/UBASE/UDMA modules, stock UMDK
`liburma`, stock UDMA and UB aggregation providers, and stock
`urma_perftest`.  No benchmark, provider or kernel-driver source is changed.
The simulator supplies the missing device-side queue execution, completion
events, packet transport and TP-to-port behavior.

## Reproduction

Start the fast functional pair with two independent 400-Gbit/s ports and an
explicit L1 switch:

```bash
OPENURMA_TRACE_PACKETS=1 \
  bash /Users/caobo/workspace/openurma-gem5-lab/run-dual.sh \
    --profile fast --provider official --ub-port-count 2 \
    --peer-topology l1-switch --peer-port-map 0,1 \
    --peer-port-selection tp-context --peer-switch-delay 0ns
bash /Users/caobo/workspace/openurma-gem5-lab/sync-dual.sh
```

Install the topology through the official `uvs_set_topo_info()` entry point:

```text
node0: ou-ubagg-topology 0 0x100 0x101 0x200 0x201
node1: ou-ubagg-topology 1 0x100 0x101 0x200 0x201
```

Run one collective latency smoke test.  Node 0 omits `-S`; node 1 appends
`-S 10.0.0.1`:

```bash
OPENURMA_DIST_SYNC=1 urma_perftest send_lat \
  -d bonding_dev_0 --eid_idx 0 --ctp --use_bonding \
  --aggr_mode balance -s 128 -P 21115 -J 1 -I 0 \
  -l 1 -n 8 --enable_imm -p 0
```

`-I 0` is intentional.  The aggregation provider prepends its eight-byte
header, so a nominal 128-byte inline request would exceed the physical
provider's 128-byte inline limit.  `--use_bonding` also makes perftest reserve
the provider's receive-header space.

## Observed result

Both commands returned status zero and both shells reappeared.  The short run
reported two measured samples after five synchronized warm-up deltas:

```text
node0: 128 B, min 2.60 us, max 3.06 us, avg 2.83 us
node1: 128 B, min 2.70 us, max 2.96 us, avg 2.83 us
```

This sample count is deliberately a structural smoke test, not a statistical
performance result.

The stock control path created physical TP 3 and TP 4.  The model resolved
those TPs to different hardware ports.  Packet traces show successive
SEND_IMM messages alternating as follows:

```text
TP 4: src_port=1, dst_port=1, physical Jetty 1025
TP 3: src_port=0, dst_port=0, physical Jetty 1024
```

Receive CQEs carry the source physical Jetty, source EID and TPN required by
the unchanged aggregation provider's physical-to-virtual lookup.  For node 0,
the two paths were observed as:

```text
local Jetty 1025, remote Jetty 1025, remote EID 0x10101, TPN 4
local Jetty 1024, remote Jetty 1024, remote EID 0x00101, TPN 3
```

The identities are derived from the official Jetty context's `seid_idx` and
the endpoint EID; they are not selected by the benchmark or hard-coded per
test case.

## Administrative completion behavior

The model now captures the official CEQ context, writes CEQEs for JFC
completions and raises the programmed CEQ MSI vector.  It also preserves the
long-lived CSQ, AEQ, CEQ and MSI translations while user TIDs are being torn
down.  AEQ sequence numbers beyond 1088 were delivered successfully in this
run, and the earlier event-mode mailbox timeout (`ret=-16`) is gone.

## Remaining limitation

The official UMMU driver currently logs `invalidate cfg_table failed,
ret=-19` while releasing user contexts and TIDs.  Probe itself succeeds, but
the model environment does not yet give `ummu_core_invalidate_cfg_table()` a
matching registered device/domain for every UDMA TID.  This is a control-plane
bookkeeping gap rather than a dropped UBASE mailbox completion: resource
destruction completes and both applications return zero.  It remains the
next UMMU integration item and is not hidden or converted into benchmark-side
success.
