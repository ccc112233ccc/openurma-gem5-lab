# Modular RMA regression

This is the reproducible acceptance matrix for the independent five-process
topology: two gem5 full-system guests, two standalone UDMA devices, and one
native ns-3-UB fabric process.  The guests use the official kernel drivers,
official UMDK UDMA provider, and the complete official `libummu.so.1`.

## Fast functional gate

The full matrix restores a coordinated shell-ready checkpoint and runs with
`--no-sync`.
This removes Linux boot from every test and avoids paying a 100 ns conservative
barrier while checking protocol and device correctness.  ns-3 still executes
serialization, queueing, routing, and delivery events at modeled timestamps;
the reported URMA latency values are not claimed as synchronized timing data.

```bash
./lab start-ready --profile fast --provider official \
  --network-backend modular-ns3ub --no-sync
./lab sync
./lab rma-regression experiments/rma-regression
```

Measured on 2026-09-28 on the M2 development host:

| Case | Bytes | Iterations / post list | Wall time | Result |
|---|---:|---:|---:|---:|
| `send_bw_128_wrap` | 128 | 128 / 16 | 18.947 s | PASS |
| `send_bw_4096` | 4 KiB | 32 / 16 | 11.611 s | PASS |
| `write_lat_128` | 128 | 8 / 1 | 10.029 s | PASS |
| `read_lat_128` | 128 | 8 / 1 | 9.398 s | PASS |
| `write_bw_4096_out16` | 4 KiB | 64 / 16 | 11.527 s | PASS |
| `read_bw_4096_out16` | 4 KiB | 64 / 16 | 11.506 s | PASS |
| `write_bw_65536_frag` | 64 KiB | 16 / 1 | 15.202 s | PASS |
| `read_bw_65536_frag` | 64 KiB | 16 / 1 | 15.559 s | PASS |
| `write_bw_1m_frag` | 1 MiB | 5 / 1 | 37.437 s | PASS |
| `read_bw_1m_frag` | 1 MiB | 5 / 1 | 37.911 s | PASS |

All ten cases passed in **179.138 seconds**. Historical names `wrap` and
`out16` overstate what these commands establish: they did not specify JFS
depth, and `-l 16` is batch size, not an outstanding limit. These results do
not independently prove multiple ring wraps or 16 concurrent WQEs.
The 64 KiB and 1 MiB cases
exercise asynchronous multi-packet READ/WRITE fragmentation in both
directions.

A second full run after removing the legacy UBSim integration path passed
all ten cases on 2026-09-29 in **233.592 seconds**.  Its per-case host times
were 27.759, 14.799, 11.954, 11.894, 14.962, 14.482, 19.846, 19.593,
49.352 and 48.939 seconds in the table order above.  Boundary counters showed
2,424 ns-3 deliveries carrying 26,510,368 payload bytes, 2,424 UDMA fragments,
524 decoded WQEs and 524 completions, with zero SQ depth/decode rejects and
zero network backpressure.  The compact, tracked evidence is in
[`results/modular-rma-regression-20260929.md`](../results/modular-rma-regression-20260929.md);
the complete UART transcripts remain under the git-ignored
`experiments/rma-regression-20260929-cleanup/` working-tree directory.

Large transfers originally exposed two hardware-model bugs.  UDMA tried to
push every fragment synchronously and could deadlock on a full transport ring;
it now retains a pending transfer and drains fragments asynchronously.  The
ns-3 ingress adapter also injected every fragment at one timestamp and could
overflow the finite switch queue; ingress is now paced by the configured
400 Gbit/s serialization time.

## Checkpoint and profiling

### Explicit depth-512 functional stress

On a shell-ready dual-QEMU functional environment:

```bash
UBSIM_RMA_SUITE=outstanding512 ./lab qemu-rma-regression experiments/depth512
```

This suite runs bidirectional SEND/WRITE/READ at 4096 B with `-T 512 -R 512`,
2048 iterations and post-list/CQ-moderation pairs 1/1, 16/16 and 512/512.
It checks both UART transcripts for actual `JFS depth : 512` and one complete
4096 B / 2048-iteration bandwidth row, in addition to guest exit status.
The operation count exceeds the configured queue capacity four times, but
hardware WQEBB producer/consumer wrap counters and actual in-flight high-water
marks are separate evidence still to collect. This is a queue-capacity stress
test, not a claim of 512 simultaneously active hardware requests or verified
payload contents. No-sync bandwidth figures are not timing acceptance results.

The older `full` suite still checks guest exit status only; per-size transcript
validation and independent payload verification remain follow-up work.

`./lab checkpoint NAME` stops both guests at the same pseudo operation, waits
for both architectural checkpoints, drains both UDMA processes, and stores the
two device states beside the gem5 states.  Creating `post-rma-verified` took
19 seconds.  A restored run validates the live `udma0 ACTIVE` state instead of
depending on boot messages absent from a fresh UART log.

Routine validation must use `./lab start-ready`. It resolves the requested
machine and network manifest first, then restores the newest checkpoint whose
complete manifest and per-node gem5/UDMA state match. It fails closed when no
compatible checkpoint exists. Plain `./lab start` is only for intentionally
testing boot, initialization, or creating a new checkpoint.

Normal shutdown emits three boundary profiles:

- `[UB_HOST_PROFILE]`: host messages, polling, synchronization and backpressure.
- `[UDMA_PROFILE]`: wall/virtual time, loops, sleeps, SQ doorbells, DMA reads,
  decoded WQEs, fragments, completions and rejects.
- `[NS3_UB_NET_STATS]`: forwarded/delivered packets and bytes, virtual time,
  synchronization steps and backpressure.

New regression reports also record each node's gem5 tick delta at the UART
command boundaries.  A gem5 tick is one picosecond in this configuration.
For unsynchronized functional runs these are per-node progress measurements;
they must not be interpreted as a common virtual-time latency.  A synchronized
run may compare them directly once the conservative fence is active.

## Conservative synchronization status

The lifecycle protocol supports a collective PREPARE/COMMIT fence with an
explicit enable or disable target.  The enable fence was observed at both
gem5/UDMA boundaries and ns-3 in a five-process run.  With AtomicCPU and the
default 100 ns lookahead, however, synchronized execution advances only about
one simulated microsecond per host second: the CPU event queue must repeatedly
stop at the safe horizon even when no packet is present.  Replacing adapter
sleeps with spinning increased host loops dramatically without increasing
virtual-time progress, confirming that the bottleneck is the fine-grained CPU
horizon, not polling sleep.

For that reason the full correctness matrix uses checkpoint plus `--no-sync`.
Use the synchronized smoke suite only for small timing-valid samples:

```bash
./lab rma-regression --sync-smoke experiments/rma-sync-smoke
```

The disable-fence implementation builds and passes process-level protocol
tests, but its second guest marker could not be reached within the practical
AtomicCPU timeout after enabling 100 ns synchronization.  It should not yet be
treated as an end-to-end acceptance result.
