# QEMU modular dataplane functional regression — 2026-09-30

## Result

The current QEMU path passed all 11 functional cases on macOS/Apple Silicon:

- two ARM64 QEMU 11.1.1 TCG guests;
- the stock `liburma-udma.so`, official `udma.ko`, UBASE, UMMU, UB core and UB
  bus drivers in each guest;
- one standalone UDMA device process per guest;
- one native ns-3 UB fabric process shared by both endpoints;
- two modeled UB ports per endpoint at 400 Gbit/s, 100 ns endpoint-link delay,
  and 50 ns switch delay.

Both `udma0` devices reported four ACTIVE EIDs.  The socket OOB link was also
bidirectionally reachable between `10.0.0.1` and `10.0.0.2`.

## Functional matrix

| Case | Payload | Iterations | Wall time | Result |
| --- | ---: | ---: | ---: | --- |
| `send_lat` | 128 B | 20 | 2.706 s | pass |
| `send_bw`, SQ wrap | 128 B | 128 | 2.861 s | pass |
| `send_bw` | 4 KiB | 32 | 2.536 s | pass |
| `write_lat` | 128 B | 8 | 2.435 s | pass |
| `read_lat` | 128 B | 8 | 2.419 s | pass |
| `write_bw`, 16 outstanding | 4 KiB | 64 | 2.572 s | pass |
| `read_bw`, 16 outstanding | 4 KiB | 64 | 2.563 s | pass |
| `write_bw`, fragmented | 64 KiB | 16 | 2.789 s | pass |
| `read_bw`, fragmented | 64 KiB | 16 | 2.799 s | pass |
| `write_bw`, fragmented | 1 MiB | 5 | 4.094 s | pass |
| `read_bw`, fragmented | 1 MiB | 5 | 4.561 s | pass |

Total suite wall time was 32.343 seconds.  Raw UART transcripts, commands,
return codes, and per-case wall times are in
`experiments/qemu-rma-regression-cli-fixed-20260930/` in the validation workspace.

## Cross-process evidence

At shutdown, the two UDMA processes reported 564 completed SQ WQEs in total
and queued/sent 2,464 network fragments.  The ns-3 fabric reported exactly
2,464 frames forwarded and delivered, carrying 26,517,888 payload bytes.
There were no SQ decode rejects, SQ-depth rejects, UBASE errors, network-send
backpressure events, or ns-3 output-backpressure events.

This validates the functional path through the official provider and kernel
drivers, the QEMU UB-HOST boundary, standalone UDMA device models, and the
native ns-3 fabric.  Conservative virtual-time synchronization is disabled on
this QEMU path; consequently, the guest `urma_perftest` latency and bandwidth
numbers are not timing-model results.

## Reproduction

```bash
./lab --runtime native build udma-device
./lab --runtime native build ns3ub
./lab --runtime native build qemu
./lab start-qemu-dual
./lab status-qemu
./lab qemu-rma-regression
./lab stop-qemu
```
