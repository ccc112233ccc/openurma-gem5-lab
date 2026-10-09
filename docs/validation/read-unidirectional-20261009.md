# Official READ bandwidth without -B

User requested the existing bandwidth experiment without bidirectional mode.
Evidence: `experiments/qemu-read-unidirectional-20261009`.

The topology remains two QEMU TCG guests, two ports per endpoint,400G per port,
required conservative synchronization, icount shift0, and the lossless-lower-layer
CTP profile. No model build, timing constant or guest image change. The command
only omits `-B` relative to the corresponding bidirectional explicit-size case.
Thirteen sizes ran sequentially in one session, including the additional64 KiB
comparison. This does not change the topology into a one-port experiment.

```sh
urma_perftest read_bw -d udma0 --eid_idx 0 --ctp \
  -s 65536 -P 21422 -J 1 -I 0 -T 512 -R 512 -n 2048 -l 16 -Q 16 -p 0
# client adds: -S 10.0.0.1
```

Server does not post READ WQEs. Official UMDK run_bw_once (perftest_run_test.c)
waits for the client and prints its exchanged report with a zero local rate.
Both terminals therefore show the same single-direction measurement; do not
add them or halve the printed result.

| Bytes | Tool average MiB/s | Decimal GB/s |
| ---: | ---: | ---: |
|2|1.81|0.001898|
|4|3.63|0.003806|
|8|7.26|0.007613|
|16|14.51|0.015215|
|32|29.02|0.030430|
|64|58.04|0.060859|
|128|116.09|0.121729|
|256|232.18|0.243458|
|512|464.35|0.486906|
|1024|928.75|0.973865|
|2048|1857.34|1.947562|
|4096|3714.68|3.895124|
|65536|46950.29|49.230947|

These are measured values, not the previous bidirectional results divided by2.
The small-size message rate is approximately0.951 Mpps;64 KiB is0.751205 Mpps.
No per-port utilization claim follows from the aggregate tool output.

## Acceptance and cost

All13 client/server command pairs return zero with matching complete result
rows. Source node1 completes26624 WQEs,26 SQ consumer wraps, observed RMA peak436.
Server node0 posts zero SQ WQEs, with zero source RMA outstanding and SQ wraps.
Fabric completes26624 WQEs and57344 READ response TAs. Application bytes150990848;
legacy logical READ bytes301981696 are not used in the rate conversion.
Checked data-path failures, retries, late responses, drops and target DMA errors
are zero. Two-sample live-work drain observed, not global checkpoint quiescence.
All simulation processes stopped afterward.

Total181.155 s (command181.20 s), shell-ready44.190 s, first command79.806 s
including control setup. Later commands4.074–4.354 s except64 KiB5.842 s.

The runner now provides `--unidirectional-read` only for explicit-size READ,
rejecting other verbs, legacy scan mode and report diagnostics. The checker
verifies direction, asymmetric SQ counts, correct server/client headers,
single-direction byte/TA accounting and exact requested sizes. It rejects
mixed directions or unsupported single-direction variants. Existing bidirectional
audits remain valid. Checker52 tests passed in0.118 s; parent checker/runner63
tests passed in0.21 s. No native contract alone is substituted for this guest run.

```sh
python3 tools/run_qemu_timing_trial.py experiments/NEW --bw-verb read_bw \
  --unidirectional-read --sizes 2 4 8 16 32 64 128 256 512 1024 2048 4096 65536 \
  --iterations 2048 --depth 512 --post-list 16 --cq-mod 16 --inline 0 --drain-timeout 8
```

The original user-provided n16384/batch256/queue8192 command is still a separate
parameter set; this run deliberately keeps the immediately preceding comparison's
n2048/depth512/batch16/CQ16 settings and removes only bidirectionality.
