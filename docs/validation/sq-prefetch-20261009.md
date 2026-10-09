# SQ descriptor prefetch and official READ bandwidth

## Change and boundaries

The former per-Jetty fetch gate read one 64-byte WQEBB before issuing the next
request. With a 500 ns host link each way, a single descriptor fetch occupied
about 1 us. The earlier small-READ result was approximately 0.951 Mpps. This was
not a limit of one complete READ in flight: network responses could overlap.

The new simulator-neutral UDMA model separates bounded descriptor prefetch from
ordered WQE decode/issue. The defaults are a 64-WQEBB cache per SQ, at most
16 contiguous WQEBBs per read and four concurrent reads. Empty placeholders
reserve the cache budget before asynchronous DMA. Reads stop at the published
producer and at the ring boundary. Absolute issue indexes prevent ring aliasing;
out-of-order read callbacks do not reorder WQE issue. Fence/strong placement,
variable-length WQEs, owner validation and source completion ordering remain.
WQEs larger than the cache use the existing continuation path. Faults latch;
in-flight callbacks keep resources busy. Checkpointing rejects in-flight,
failed or unconsumed-cache state.

These are explicit model architecture parameters, not known physical UDMA
defaults and not fitted latency constants. No host/network latency, link rate,
guest/provider code or payload-DMA concurrency was changed. Set
`UBSIM_SQ_PREFETCH_WQEBBS=1 UBSIM_SQ_FETCH_BURST_WQEBBS=1
UBSIM_SQ_FETCH_MAX_OUTSTANDING=1` for the serial-fetch control. Both gem5 and
QEMU launchers record the three parameters. `start-ready` compares them with
the checkpoint manifest; the standalone model state itself does not validate
these configuration values. No gem5 cold boot was performed.

## Validation

- Independent core counterexamples: 7/7 CTest, incremental build plus tests
  6.79 s, tests 5.42 s. Covers producer growth/bounds, bounded cache/in-flight,
  reverse callbacks, synchronous callbacks, invalid owner, DMA failure,
  fence/strong placement, multi-block ring crossing, small-cache fallback,
  destroy/checkpoint drain, repeated ring generations and out-of-order CQ.
- Full device build and tests: 10/10, combined 13.81 s, CTest 4.94 s.
- Launcher/runner/checker Python suite: 65/65, wall 0.31 s.
- Official-driver no-sync dual-QEMU regression: 12/12, suite 29.926 s,
  launch-to-stop wrapper 35.97 s. Evidence:
  `experiments/sq-prefetch-functional-20261009`. Includes SEND/READ/WRITE,
  ring wrap, 4 KiB/64 KiB/1 MiB fragmentation and the inline WRITE sweep.
  1 MiB still exceeds the advertised 64 KiB capability; this does not extend
  that advertised capability or establish performance from no-sync execution.

## Synchronized single-direction READ sweep

Evidence: `experiments/sq-prefetch-read-20261009`. QEMU TCG, icount shift 0,
required conservative synchronization, 500 ns host link, 100 ns network link,
400 Gbit/s per port, two ports per endpoint, lossless-lower-layer CTP assumption.
Same n2048/depth512/post-list16/CQ16 parameters as the previous single-direction
sweep; no `-B`. This is not yet a one-port-topology match to the physical table.

```sh
python3 tools/run_qemu_timing_trial.py experiments/NEW --bw-verb read_bw \
  --unidirectional-read --sizes 2 4 8 16 32 64 128 256 512 1024 2048 4096 65536 \
  --iterations 2048 --depth 512 --post-list 16 --cq-mod 16 --inline 0 \
  --drain-timeout 8
```

All 13 sizes pass the strict counter/output checker. The tool's printed MB/sec
is converted from MiB/s to decimal GB/s once; the exchanged server row is not
added to the client row.

| Bytes | Before GB/s | Prefetch GB/s | Prefetch Mpps |
| ---: | ---: | ---: | ---: |
| 2 | 0.001898 | 0.006407 | 3.205770 |
| 4 | 0.003806 | 0.012824 | 3.205208 |
| 8 | 0.007613 | 0.023729 | 2.965778 |
| 16 | 0.015215 | 0.051296 | 3.205851 |
| 32 | 0.030430 | 0.102530 | 3.204005 |
| 64 | 0.060859 | 0.205133 | 3.205208 |
| 128 | 0.121729 | 0.410968 | 3.210675 |
| 256 | 0.243458 | 0.831783 | 3.249143 |
| 512 | 0.486906 | 1.519533 | 2.967841 |
| 1024 | 0.973865 | 3.281886 | 3.204968 |
| 2048 | 1.947562 | 6.150727 | 3.003285 |
| 4096 | 3.895124 | 13.125907 | 3.204567 |
| 65536 | 49.230947 | 49.230654 | 0.751200 |

The source completed 26,624 WQEs and 26 SQ wraps, server posted zero WQEs.
Fabric completed 57,344 READ response TAs. Application bytes: 150,990,848;
legacy logical READ bytes count both request and response, so are not bandwidth
bytes. Checked datapath errors, retries, late responses and drops were zero;
two consecutive live-work drain samples passed. This is not global checkpoint
quiescence. Unknown queue writes still numbered 2,887; not every MMIO is claimed
implemented. Source RMA outstanding peaked at 506 across the whole session,
not necessarily at each payload size.

Descriptor reads fell from 26,624 to 1,664 logical bursts (16 WQEBBs/burst).
Prefetch bytes: 1,703,936; per-SQ in-flight peak 3; cache peak 48 WQEBBs.
`sq_dma_reads` / `sq_prefetch_dma_reads` count logical head/prefetch requests,
not all physical DMA transactions: token translation can split reads at pages,
and oversized WQE continuation reads are separate. `sq_prefetch_cache_hits`
counts WQEBBs presented to decode, including an ordered-head retry, not a cache
hit ratio. New counters are process observations, not checkpoint state.

Sweep wall time: 183.890 s (wrapper 183.93 s), shell ready 44.883 s,
first command 80.861 s including control setup; remaining commands approximately
4.1–4.3 s, except 64 KiB 6.295 s. This change improves modeled small-message
throughput, not experiment wall time. The runner fingerprints launch binaries
and images with SHA256; dynamic libraries are explicitly outside that fingerprint.

## Matched serial/prefetch control

To exclude changed build products and test order, the same binary ran the same
2 B / 4 KiB / 64 KiB sequence with 1/1/1 versus 64/16/4. All five executable/image
SHA256 records match. The resolved manifests differ only in these three SQ
parameters. Both strict audits pass, with 6,144 client WQEs, zero server WQEs,
six source SQ wraps and successful two-sample live-work drain.

| Bytes | Serial GB/s | Prefetch GB/s |
| ---: | ---: | ---: |
| 2 | 0.001898 | 0.006407 |
| 4096 | 3.895072 | 13.121964 |
| 65536 | 49.231807 | 49.229783 |

Evidence: `experiments/sq-serial-read-control-20261009` and
`experiments/sq-prefetch-read-matched-20261009`. Wall time: 148.612 / 151.618 s
(wrappers 148.65 / 151.67 s). Source descriptor bursts are 6,144 / 384.
The source session RMA peak is 436 / 506. The matched experiment reproduces
the old serial small-message limit and the approximately 3.37x improvement,
without a material change in large-message throughput. These are individual
bounded runs, not a statistical repeatability/confidence-interval study.

## Remaining diagnosis, not conclusions

Small messages improve roughly 3.4x but remain below the physical 20–23 Mpps.
Large-message throughput is essentially unchanged. Do not reduce arbitrary
latencies to close the remaining gap. Independent source review identifies:

- Remote READ requests are not globally serialized; each has its own token
  DMA. Source response writes also use per-transfer accounting, not one global
  READ execution lock.
- JFC publication does serialize consumer read followed by CQE write. CQ16
  amortizes this over a batch; its existence alone does not establish that it
  is the measured 3.2 Mpps bottleneck.
- Official provider WR construction and CQ polling execute as guest CPU
  instructions. QEMU icount is not a detailed server CPU/cache model. Timing
  MMIO doorbell writes are posted, so adding a synchronous host round trip per
  doorbell would be an incorrect explanation.

Next isolate provider/CPU, batching/CQ and host-link sensitivity with matched
experiments and per-stage measurements before changing another hardware path.
