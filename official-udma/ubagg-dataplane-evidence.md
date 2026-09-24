# Official UB aggregation dataplane stage

This stage runs the stock OLK 6.6 UB/UMMU/UBASE/UDMA modules, stock UMDK
`liburma`, stock UDMA and UB aggregation providers, and `urma_perftest`.
The provider and kernel-driver sources are unchanged.  The benchmark has an
optional `OPENURMA_DIST_SYNC=1` coordination path: it brackets the measured
loop with gem5's conservative synchronization, keeps the one-sided server in
the collective, and executes the required single worker inline so Linux
thread creation stays outside the 100-ns interval.  Its ordinary behavior is
unchanged when the environment variable is absent.  The simulator supplies
the missing device-side queue execution, completion events, packet transport
and TP-to-port behavior.

## Reproduction

Start the fast functional pair with two independent 400-Gbit/s ports and an
explicit L1 switch:

```bash
OPENURMA_TRACE_PACKETS=1 \
  ./lab start \
    --profile fast --provider official --ub-port-count 2 \
    --peer-topology l1-switch --peer-port-map 0,1 \
    --peer-port-selection tp-context --peer-switch-delay 0ns
./lab sync
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

## Official bonding WRITE path

The same topology also completes the stock provider's one-sided WRITE path.
Run node 0 without `-S` and node 1 with `-S 10.0.0.1`:

```bash
OPENURMA_DIST_SYNC=1 urma_perftest write_lat \
  -d bonding_dev_0 --eid_idx 0 --ctp --use_bonding \
  --aggr_mode balance -s 128 -P 21116 -J 1 -I 0 \
  -l 1 -n 8 -p 0
```

Both commands returned zero.  After excluding five synchronized warm-up
deltas, the two retained samples reported:

```text
node0: 128 B, min 1.51 us, max 1.77 us, avg 1.64 us
node1: 128 B, min 1.41 us, max 1.51 us, avg 1.46 us
```

Packet traces show that the unchanged aggregation provider alternated every
operation between physical Jetty 1025 / TPN 4 / port 1 and physical Jetty
1024 / TPN 3 / port 0.  On each path the model decoded the official UDMA WQE,
sent a WRITE request (`op=0x82`), performed the remote DMA write, returned an
ACK (`op=0x83`) on the same port and generated the matching send CQE (JFC 9
or JFC 8 respectively).

`OPENURMA_DIST_SYNC=1` is optional simulator test instrumentation in
`urma_perftest`; it brackets the measured loop with gem5's conservative
virtual-time synchronization and uses the existing control socket for a
rendezvous.  It does not replace the official driver/provider queue, memory
registration, WQE, packet or completion paths, and normal perftest behavior
is unchanged when the variable is absent.

## Official bonding READ path

One-sided READ uses the same official topology and resources.  Node 0 runs
the command without `-S`; node 1 appends `-S 10.0.0.1`:

```bash
OPENURMA_DIST_SYNC=1 urma_perftest read_lat \
  -d bonding_dev_0 --eid_idx 0 --ctp --use_bonding \
  --aggr_mode balance -s 128 -P 21117 -J 1 -I 0 \
  -l 1 -n 8 -p 0
```

The server normally has no READ worker because READ is one-sided.  With the
optional distributed synchronization enabled it now remains as a collective
gem5 synchronization participant until the client reports that its final
response arrived.  Only the client posts official READ WQEs and reports
latency; the server still performs no benchmark-side data operation.

Both processes returned zero.  The client reported:

```text
node1: 128 B, min 2.78 us, max 2.98 us, avg 2.88 us
```

The trace contains eight complete hardware-model transactions.  Successive
official WQEs alternate between these paths:

```text
physical Jetty 1025 / TPN 4 / port 1: READ_REQ 0x84 -> READ_RESP 0x85 -> JFC 9 CQE
physical Jetty 1024 / TPN 3 / port 0: READ_REQ 0x84 -> READ_RESP 0x85 -> JFC 8 CQE
```

For each request, the responder resolves the imported remote segment, reads
128 bytes through its UMMU-backed DMA path and returns the payload on the
request's ingress port.  The requester writes that response into the local
registered segment and raises the send completion only after the full response
has arrived.  Thus the measured loop exercises the official provider's READ
WQE layout plus modeled request, remote DMA, response, local DMA and CQE
behavior rather than converting READ into a benchmark-side memory copy.

The same READ path was also exercised with a 64-KiB payload.  The official
provider still emitted one fixed-format WQE containing address and length;
the hardware model fragmented the response into nine transport packets
(eight 8088-byte fragments plus the final 832 bytes) and raised the CQE only
after the complete response had been written to the registered destination.

### Repeated execution in one boot

Two complete 128-byte READ tests were run consecutively on the same guest
pair, using ports 21117 and 21118, without rebooting or reinstalling the
topology.  Both commands returned zero and both reported the same short-run
statistics:

```text
128 B, 8 iterations, 5 warm-up deltas excluded, 2 measured samples
min 2.78 us, max 2.98 us, median 2.86 us, avg 2.88 us
```

The second process pair created new physical Jettys 1026 and 1027, new TPs 5
and 6, and new JFC/JFR contexts.  Its eight READ requests alternated between
TP 6 / port 1 and TP 5 / port 0; all eight responses returned on the matching
port.  There were no CRQ-write, SQ-WQE, SGE or payload-DMA errors.

This repeatability required two device-lifetime behaviors in the model.  It
retains queue-context TID roots after userspace teardown for already-programmed
hardware queues, and it retains the first valid translation of the kernel
UDMA mailbox DMA-pool pages just as it already does for CSQ, CRQ, AEQ and CEQ.
Consequently a failed userspace UMMU invalidation cannot make a later official
mailbox command read stale or unmapped memory.

At the synchronization boundary, all non-halted CPU contexts now receive a
common `quiesceUntil` deadline.  Interrupt and scheduler wakeups before that
deadline are deferred, while the SystemC peer worker sleeps directly to the
initial common tick rather than polling an empty ring every 10 ns.  Periodic
100-ns synchronization remains active during the actual WQE/completion loop.

## Administrative completion behavior

The model now captures the official CEQ context, writes CEQEs for JFC
completions and raises the programmed CEQ MSI vector.  It also preserves the
long-lived CSQ, AEQ, CEQ and MSI translations while user TIDs are being torn
down.  AEQ sequence numbers beyond 1088 were delivered successfully in this
run, and the earlier event-mode mailbox timeout (`ret=-16`) is gone.

## Remaining limitation

The official UMMU driver still logs `invalidate cfg_table failed, ret=-19`
while releasing user contexts and TIDs.  Probe succeeds and the modeled
hardware now preserves the mappings that genuinely have device lifetime, so
resource destruction and subsequent recreation both complete.  The warning
remains a UMMU control-plane bookkeeping gap to fix; it is not suppressed or
converted into benchmark-side success.
