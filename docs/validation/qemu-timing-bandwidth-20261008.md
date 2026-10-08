# Synchronized QEMU bandwidth: first post-pipeline measurements

Model revision: `d9a9e2c`. Both guests use TCG icount shift0; UB-HOST,
UDMA and ns-3 boundaries require synchronization from time zero. CNTVCT and
the host adapter derive timestamps from QEMU_CLOCK_VIRTUAL; ns-3 uses ps.
These are network/device timing-model results, not no-sync throughput.

## Controlled experiment

Two physical ports per endpoint, each configured at the listed rate; CTP
wildcard member selection is enabled and its context line-rate setting also
uses that rate. No per-port byte counters are available, so do not call this
single-port utilization or infer equal traffic on each member/direction.
Host adapter delay500 ns, peer100 ns, switch50 ns, DMA window256,
IOTLB4096, retransmission on, RTO25.6 us, max retransmissions7.

Official command (client adds `-S 10.0.0.1`; ports21410 onward):

```sh
urma_perftest write_bw -d udma0 --eid_idx 0 --ctp -B \
  -s SIZE -P PORT -J 1 -I 64 -T 64 -n 64 -l 16 -Q 16 -p 0
```

The output label MB/s actually uses 2^20 bytes (MiB/s). `-B` sums the local
and remote bandwidth reports. Direction average below is half that sum, not
independently measured directional equality.

| Port rate | Payload | Bidirectional average MiB/s | Aggregate Gbit/s | Direction average Gbit/s |
| --- | --- | ---: | ---: | ---: |
| 400 Gbit/s | 4 KiB | 3348.33 | 28.09 | 14.04 |
| 400 Gbit/s | 64 KiB | 53120.86 | 445.61 | 222.81 |
| 200 Gbit/s | 4 KiB | 3346.36 | 28.07 | 14.04 |
| 200 Gbit/s | 64 KiB | 32905.46 | 276.03 | 138.02 |

Both UARTs produced matching rows, actual JFS depth64 and status0; transcripts
were checked using `validate_depth_output`. Rate sensitivity is visible for
64 KiB, not 4 KiB. Small-payload operation overhead is a plausible explanation,
not yet an isolated bottleneck diagnosis. These single 64-operation runs do
not establish steady-state saturation or statistical repeatability.

## 1 MiB failure: not a valid bandwidth result

The 400G 1 MiB trial failed before a completed result. ns-3 exited with
`target execution cannot resolve external WQE`; remaining test processes were
stopped rather than allowed to time out. The 200G trial deliberately covered
only the two passing sizes. It did not bypass the error to claim 1 MiB support.

Independent source review identifies a lifecycle gap: adapter
`OnNativeWqeTerminal` erases `native_tasks_` immediately, but already queued or
in-flight CTP packets may still reach `OnTargetExecute`. RTO starts at enqueue,
and transaction termination does not cancel those queued/in-flight packets.
Target duplicate-cache expiry is also finite. The available log does not
distinguish retry exhaustion from a late duplicate after success in this run.
Next work needs terminal/task/TA/tick diagnostics and lifetime-safe handling
of late traffic, with high-queue-delay and duplicate-expiry counterexamples.
Increasing RTO or ignoring unknown tasks is not an accepted fix.

## Cost and reproducibility

```sh
python3 tools/run_qemu_timing_trial.py experiments/NEW-400g --timeout 180
UBSIM_PEER_LINK_RATE_GBPS=200 python3 tools/run_qemu_timing_trial.py \
  experiments/NEW-200g --timeout 180 --sizes 4096 65536
```

| Wall time | 400G | 200G |
| --- | ---: | ---: |
| Shell ready since launch | 43.362 s | 43.453 s |
| 4 KiB command pair, including control connection | 80.092 s | 85.960 s |
| 64 KiB command pair | 4.283 s | 4.553 s |
| 1 MiB failed command pair until stop | 71.370 s | not run |
| Entire trial including shutdown/log collection | 201.357 s | 136.554 s |

Evidence directories: `experiments/qemu-timing-bandwidth-20261008/` and
`experiments/qemu-timing-bandwidth-200g-20261008/`. Each preserves commands,
wall times, manifests, full UART transcripts and component logs. Both runs were
stopped after collection; no gem5 boot or rebuild was used.

QEMU currently supplies immediate memory accesses plus adapter delay, not a
server cache/DRAM bandwidth model. Stage3 has initial synchronized measurements
and parameter sensitivity, but is not complete while the 1 MiB failure,
steady-state/concurrency sweeps and measurement repeatability remain open.
