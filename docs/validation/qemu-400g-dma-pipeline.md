# QEMU 400-Gb/s data-path validation

Historical pre-SQ-pipeline experiment. Unit/topology corrections below were
added on 2026-10-08; this is not a result for the current multi-WQE engine.

This check isolates the modeled UDMA data path from host wall time. Both QEMU
guests ran with TCG `icount`, required adapter synchronization from virtual
time zero, a native ns-3 UB fabric, and the default 400-Gb/s port rate.

Configuration:

```text
peer_link_rate_gbps=400
host_link_latency_ns=500
peer_latency_ns=100
switch_delay_ns=50
dma_max_outstanding=256
iotlb_entries=4096
```

Command pair (server omits `-S`; client adds `-S 10.0.0.1`):

```sh
urma_perftest write_bw -d udma0 --eid_idx 0 --ctp -B \
  -s 1048576 -P 21401 -J 1 -I 64 -n 16 -l 1 -Q 1 -p 0
```

Observed on both endpoints:

```text
bytes    iterations  BW peak[MB/sec]  BW average[MB/sec]  MsgRate[Mpps]
1048576  16          63375.79         63364.70            0.063365
```

The `-B` report is the sum of local and remote bandwidth; this is explicit in
`print_bi_bw_report()` in the official `urma_perftest` source. The measured
63364.70 value uses MiB/s despite the printed MB/s label (`PERFTEST_BW_MB`
is 2^20). It is therefore 66.4427 GB/s aggregate, or an average of 265.77
Gbit/s per direction, not the previously stated 253.4 Gbit/s. Dividing by two
does not prove the directions were equal. The default topology has two ports
per endpoint and wildcard CTP member selection; without per-port byte counters
this result does not establish utilization of a single 400-Gbit/s port.

The earlier serialized implementation reached about 553 MB/s because every
4-KiB payload page paid a complete token-translation and adapter round trip
before the next page could start. A 32-request payload-DMA window plus a
bounded modeled IOTLB raised the warm bidirectional aggregate to 38.90 GB/s.
A 256-request window, enough to cover all 4-KiB pages in one 1-MiB WQE, raised
it to 63.36 GB/s aggregate.

A controlled run with `dma_max_outstanding=512` produced 63,432.93 MiB/s
aggregate, only 0.11% above the 256-request
run. This is expected: one 1-MiB WQE contains exactly 256 4-KiB pages, so this
page-DMA window has no additional work to expose above 256. A hardware queue
depth of 512 instead describes WQEs or SQ entries; modeling that requires
multiple WQEs in flight and is a separate device-engine limit.

This is not fitted to a measured board value. The remaining gap reflects the
current modeled protocol fragmentation, host DMA granularity, WQE/ACK work,
and endpoint/switch serialization. Most importantly, the then-current jetty engine
retires one WQE before fetching the next, so the wire pipeline drains at WQE
boundaries. Control and metadata DMA remain ordered; only payload movement
uses the configurable outstanding window.
