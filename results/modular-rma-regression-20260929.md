# Modular RMA regression evidence — 2026-09-29

- Repository baseline: `aa00341063653861ac155eab43a9ad0b7da024ca`
- Runtime: macOS M2, Docker ARM64, gem5 `AtomicSimpleCPU`
- Topology: 2 gem5 + 2 standalone UDMA + 1 ns-3-UB fabric
- Guest stack: official OLK/UB drivers, UMMU, UMDK UDMA provider and
  `urma_perftest`
- Restore point: `post-rma-verified`
- Synchronization: disabled functional mode
- Link model: one 400 Gbit/s port per endpoint, 100 ns propagation

| Case | Bytes | Iterations / post list | Wall seconds | Result |
|---|---:|---:|---:|---:|
| `send_bw_128_wrap` | 128 | 128 / 16 | 27.759 | PASS |
| `send_bw_4096` | 4 KiB | 32 / 16 | 14.799 | PASS |
| `write_lat_128` | 128 | 8 / 1 | 11.954 | PASS |
| `read_lat_128` | 128 | 8 / 1 | 11.894 | PASS |
| `write_bw_4096_out16` | 4 KiB | 64 / 16 | 14.962 | PASS |
| `read_bw_4096_out16` | 4 KiB | 64 / 16 | 14.482 | PASS |
| `write_bw_65536_frag` | 64 KiB | 16 / 1 | 19.846 | PASS |
| `read_bw_65536_frag` | 64 KiB | 16 / 1 | 19.593 | PASS |
| `write_bw_1m_frag` | 1 MiB | 5 / 1 | 49.352 | PASS |
| `read_bw_1m_frag` | 1 MiB | 5 / 1 | 48.939 | PASS |

Suite wall time: **233.592 seconds**.

Shutdown profiles:

```text
[NS3_UB_NET_STATS] forwarded=2424 delivered=2424 payload_bytes=26510368 virtual_ps=615800000 wall_ns=283244988879 loops=289558 idle_sleeps=282536 sync_steps=0 sync_backpressure=0 output_backpressure=0
[UDMA_PROFILE] eid=0x100 wall_ns=280870223587 virtual_ps=34507000000 loops=345070 idle_sleeps=280133 sync_steps=0 sync_backpressure=0 net_fragments_queued=1132 net_fragments_sent=1132 net_send_backpressure=0 submitted=0 completed=0 contexts=2 mmio_writes=996 jetty_mmio_writes=58 sq_doorbells=58 sq_dma_reads=178 sq_wqes=178 sq_completions=178 sq_depth_rejects=0 sq_decode_rejects=0 unknown_queue_writes=358 last_unknown_queue_offset=0x200000 ubase_errors=0
[UDMA_PROFILE] eid=0x101 wall_ns=280937240171 virtual_ps=33991400000 loops=339914 idle_sleeps=280215 sync_steps=0 sync_backpressure=0 net_fragments_queued=1292 net_fragments_sent=1292 net_send_backpressure=0 submitted=0 completed=0 contexts=2 mmio_writes=872 jetty_mmio_writes=76 sq_doorbells=76 sq_dma_reads=346 sq_wqes=346 sq_completions=346 sq_depth_rejects=0 sq_decode_rejects=0 unknown_queue_writes=216 last_unknown_queue_offset=0x200000 ubase_errors=0
```

The two UDMA profiles sum to 2,424 fragments, 524 decoded WQEs and 524
completions.  These match the ns-3 delivery count, with no SQ depth/decode
reject and no network output backpressure.

## Independent tick-accounting rerun

The same ten cases were then rerun end to end with UART-boundary gem5 tick
capture enabled.  All ten passed again, and every case produced non-null tick
samples for both guests.  `simulated_elapsed_ns_max` is the larger of the two
guest tick deltas; it is reported for provenance and is not host wall time.

| Case | Wall seconds | node0 ticks | node1 ticks | Max simulated ns | Result |
|---|---:|---:|---:|---:|---:|
| `send_bw_128_wrap` | 25.697 | 309647463912 | 2010471814701 | 2010471814.701 | PASS |
| `send_bw_4096` | 15.992 | 239500590336 | 705720235005 | 705720235.005 | PASS |
| `write_lat_128` | 12.985 | 229559104107 | 257567832342 | 257567832.342 | PASS |
| `read_lat_128` | 12.933 | 357079938291 | 239404984038 | 357079938.291 | PASS |
| `write_bw_4096_out16` | 16.424 | 236360475261 | 314870370444 | 314870370.444 | PASS |
| `read_bw_4096_out16` | 15.783 | 231594856983 | 291824972577 | 291824972.577 | PASS |
| `write_bw_65536_frag` | 21.569 | 238849759485 | 274819132107 | 274819132.107 | PASS |
| `read_bw_65536_frag` | 21.448 | 249346067004 | 256995437643 | 256995437.643 | PASS |
| `write_bw_1m_frag` | 54.401 | 256470136803 | 312252506262 | 312252506.262 | PASS |
| `read_bw_1m_frag` | 53.996 | 326369308662 | 305822153052 | 326369308.662 | PASS |

Rerun suite wall time: **251.265 seconds**.  Raw UART captures and the JSON/CSV
report were generated under the ignored experiment directory
`experiments/rma-regression-20260929-tick-e2e/`.
