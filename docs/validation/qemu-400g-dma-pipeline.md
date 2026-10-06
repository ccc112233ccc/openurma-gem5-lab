# QEMU 400-Gb/s data-path validation

This check isolates the modeled UDMA data path from host wall time. Both QEMU
guests ran with TCG `icount`, required adapter synchronization from virtual
time zero, a native ns-3 UB fabric, and the default 400-Gb/s port rate.

Configuration:

```text
peer_link_rate_gbps=400
host_link_latency_ns=500
peer_latency_ns=100
switch_delay_ns=50
dma_max_outstanding=32
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
1048576  16          38907.20         38902.21            0.038902
```

The earlier serialized implementation reached about 553 MB/s because every
4-KiB payload page paid a complete token-translation and adapter round trip
before the next page could start. Pipelined payload DMA raised the result to
about 11.78 GB/s. Adding a bounded modeled IOTLB raised the warm steady state
to 38.90 GB/s, about 77.8% of the 50-GB/s raw one-port limit.

This is not fitted to a measured board value. The remaining gap reflects the
current modeled protocol fragmentation, host DMA granularity, WQE/ACK work,
and endpoint/switch serialization. Control and metadata DMA remain ordered;
only payload movement uses the configurable outstanding window.
