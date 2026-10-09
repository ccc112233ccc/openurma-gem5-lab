# Continuous READ small-size sweep and link-rate sensitivity

## Full-system sweep

Evidence: `experiments/qemu-read-2b-4k-profile-20261009`.
Two QEMU TCG guests, official provider/driver/perftest, standalone UDMA and
ns-3; required conservative synchronization and the corrected
`ctp-lossless-lower-layer-assumed` profile. Per-port configured rate400G.
All12 sizes ran sequentially in one guest session, without simulator restart.

```sh
python3 tools/run_qemu_timing_trial.py experiments/NEW \
  --bw-verb read_bw --sizes 2 4 8 16 32 64 128 256 512 1024 2048 4096 \
  --iterations 2048 --depth 512 --post-list 16 --cq-mod 16 --inline 0 \
  --drain-timeout 8
```

The generated command uses `--ctp -B -J1 -T512 -R512 -n2048 -l16 -Q16 -I0`
and one explicit `-s` size per process pair. These are not the user's earlier
unidirectional16384-iteration/batch256 parameters. Results below are official
perftest output with `-B`, converted once from MiB/s to decimal GB/s.

| Size (B) | Average (MiB/s) | Average (GB/s) |
| ---: | ---: | ---: |
|2|3.63|0.003806|
|4|7.26|0.007613|
|8|14.51|0.015215|
|16|29.02|0.030430|
|32|58.05|0.060870|
|64|116.10|0.121740|
|128|232.18|0.243458|
|256|464.36|0.486917|
|512|928.72|0.973834|
|1024|1857.36|1.947583|
|2048|3714.70|3.895145|
|4096|7429.14|7.790018|

Reported message rate remains approximately1.902 Mpps, so bandwidth scales
almost linearly with payload in this range. This is evidence of an
operation-rate-limited region, not a unique identification of CPU versus
posting/CQ/DMA cost. Configured SQ depth512 does not imply512 active RMA
operations: the observed UDMA pending peak is16 per endpoint in this sweep.

All49152 WQEs and49152 READ TAs complete; each endpoint completes24576 WQEs and
records24 SQ consumer wraps. Application bytes33546240; the adapter's legacy
READ payload counter67092480 includes logical request and response bytes and
is not used to calculate bandwidth. Zero checked data-path failures, retries,
late responses, runtime drops or target DMA errors. Final live-work drain
observed, not whole-system checkpoint quiescence.

Continuous success establishes repeated resource creation/destruction in one
session, not retention of the same Jetty object across tests, nor an independent
payload-content check. Native strict peers provide separate payload evidence;
do not substitute that for perftest validating data contents.

Shell-ready45.493 s; first2 B command81.331 s including initial control setup;
subsequent commands4.016–4.489 s each. Total178.059 s, command178.09 s.
All processes stopped. Historical small-size failures and interrupted scans
remain in their earlier evidence; this run does not overwrite them.

## Complete-sweep audit

The checker now accepts `--expected-sizes` and requires exact completed order
when supplied. This prevents an otherwise internally consistent prefix from
being labelled the requested full sweep. Empty, duplicate, noninteger and
out-of-range expected sizes are rejected. Legacy callers retain their earlier
behavior and report expected_sizes=null.

```sh
python3 tools/check_qemu_write_trial.py experiments/qemu-read-2b-4k-profile-20261009 \
  --verb read --expected-sizes 2 4 8 16 32 64 128 256 512 1024 2048 4096
```

Full sweep audit passes. Independent checker tests47/47 passed in0.13 s;
parent combined checker/policy/runner tests60/60 passed in0.19 s. Parser tests
are not additional hardware acceptance.

## Link-rate comparison

Matched-sequence evidence: `experiments/qemu-read-rate-400-20261009` and
`experiments/qemu-read-rate-200-20261009`. Each starts fresh guests and tests
2 B,4096 B,65536 B in that order, with the same n2048/depth512/batch16/CQ16
settings as above. Both pass the complete expected-size audit.

| Size |400G average MiB/s|400G average GB/s|200G average MiB/s|200G average GB/s|
| ---: | ---: | ---: | ---: | ---: |
|2 B|3.63|0.003806|3.63|0.003806|
|4096 B|7429.50|7.790395|7428.57|7.789420|
|65536 B|89261.20|93.597152|45844.22|48.071149|

The4 KiB change is about0.0125%; the64 KiB rate ratio is1.94705. These
results support the expected trend: small transfers are limited elsewhere,
whereas the large transfer responds strongly to modeled link rate. They do
not uniquely identify the small-transfer bottleneck or validate all physical
timing constants. Values are the official tool's `-B` output, not independent
common-ROI link-utilization measurements.

The manifests differ only in peer_link_rate_gbps. Required synchronization,
icount_shift0, host latency500 ns, peer latency100 ns, switch delay50 ns,
DMA maximum256, IOTLB4096 and effective retry off are unchanged. Input-image
SHA and command sequence are checked independently. No simulator build or
model edit occurred between the runs. The launcher uses one cortex-a72 vCPU,
1 GiB per guest and two ports per endpoint. Its rate setting applies to all
four endpoint ports and four corresponding switch ports, not a lone physical
link. The manifest does not yet independently fingerprint all shared libraries
or encode every fixed CPU/port field; source inspection supplies those facts.

Each run completes12288/12288 WQEs and73728 READ TAs, with285220864 application
bytes. Each endpoint completes6144 WQEs and six SQ wraps. Session RMA pending
peaks are496 at400G and504 at200G. Zero checked data-path errors/retries;
live-work drain observed and all processes stopped. The earlier stand-alone
64 KiB400G result92.54 GB/s is retained, not replaced by93.60: its preceding
workload history differs and it is not the matched baseline used here.

400G total145.713 s (shell46.905 s; commands81.638/4.348/7.587 s).
200G total148.765 s (shell47.436 s; commands84.526/4.374/6.938 s).
Combined wrapper294.63 s. Host wall times are validation costs, not the virtual
bandwidth measurement. This is one matched pair, not a statistical confidence
interval or a determinism claim.

## Next performance-path gates

Extend strict auditing to the user's original single-direction READ command
before calling it an exact reproduction. Inspect batching/CQ moderation and
observed outstanding sensitivity, then multi-Jetty/port contention. Keep the
CTP extension-on failure and lossless-profile limitations explicit; neither
this sweep nor the rate comparison establishes LLR or general fault recovery.
