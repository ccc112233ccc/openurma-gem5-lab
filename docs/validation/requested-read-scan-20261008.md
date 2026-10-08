# Requested single-direction READ scan

The user requested `read_bw -a23 -J1 -R8192 -T8192 -C524288 -Q1 -l256
-n16384 --retry_num 6 --ack_timeout 31 --page_size 65536`.

Adjustments verified against the actual guest and official tool source:

- Device/peer become `udma0` / `10.0.0.1`; no `-B` is added.
- Both guests report `max_jfr_depth=1024`, `max_jfs_depth=1024`,
  `max_jfc_depth=4096`; use `-R1024 -T1024 -C4096`, not unsupported requests.
- This tool version has no `--page_size`. Its `--hugepage_size` only supports
  2 MiB/1 GiB; do not reinterpret the requested 64 KiB as a hugepage option.
- `retry_num`/`ack_timeout` feed only the optional user-TP configuration, which
  the requested command did not enable. They do not configure the fabric CTP
  RTO. Omit these flags and explicitly retain the fabric defaults: 25.6 us RTO,
  seven retries. Do not claim the requested ACK-timeout semantics were tested.
- Preserve 16384 iterations, batch256, CQ moderation1. `-a23` means23 lengths
  from2 B to8 MiB. Both endpoints print the client's single-direction result;
  the server's reporting function name does not make it bidirectional.

## Scan result: failed at2 B

`experiments/qemu-requested-read-scan-20261008/` preserves capabilities,
manifest, commands, complete terminal logs and process profiles. Both guests
booted with required synchronization in42.900 s. The command pair failed after
81.247 s, including control connection setup; no valid bandwidth row was
produced. Client: `Failed CR status 9, tot_scnt: 1024, tot_ccnt: 0`.

ns-3 recorded276 admitted/failed requests, zero native completions,1932
retransmissions/exactly276×7 exhausted retries, and1932 duplicate READ-response
replays. Independent source review identifies a logical/wire length mismatch:

1. `CalcCompactLength(2)` encodes0; target `BuildInboundTaOperation` decodes
   `64 << 0`, obtaining64 B.
2. The adapter issues a2 B remote DMA based on its original segment metadata.
3. Its default target-completion result lets `BuildTransactionResponse` use
   the decoded64 B logical length for the READ response.
4. The origin expects2 B and rejects64 B under exact response-size validation.
   Cached retries replay the same rejected response until exhaustion.

This is not evidence that the RTO is simply too short or the window too large.
Do not fix it by suppressing response validation or extending RTO. The model
needs an explicit distinction between guest logical length and CTP transfer
granularity, with2/4/8/16/32/64 B,65/127/129 B and non-aligned tail tests.
Protocol semantics must guide whether wire padding or exact length metadata
is appropriate. The complete scan remains unvalidated.

Reproduce the supported-parameter request with a bounded deadline:

```sh
python3 tools/run_qemu_timing_trial.py experiments/NEW-read-scan \
  --user-read-scan --timeout 300
```

The runner records actual queue limits and validates23 complete rows before
accepting a completed scan. `--read-size 4096` isolates an aligned-size control
without silently declaring that the original scan passed.

## Aligned4 KiB control: passed

Same parameters, replacing only `-a23` with `-s4096`, passed on both guests:

```text
bytes   iterations  BW peak[MB/sec]  BW average[MB/sec]  MsgRate[Mpps]
4096    16384       964.98           964.05              0.246796
```

The tool actually uses MiB/s;964.05 MiB/s is **1.01088 GB/s**. This remains a
single-direction result, not divided by two. ns-3 recorded16384 submitted and
completed WQEs/READ responses, zero failures/retransmissions, native outstanding
peak9. Thus configured depth1024 does not mean1024 actively executing network
requests. No isolated bottleneck attribution is claimed from this one run.

Evidence: `experiments/qemu-requested-read-4k-20261008/`. Shell ready43.789 s,
capability query2.719 s, test command pair83.123 s including control connection,
entire trial132.024 s. Both runs were stopped. No model binaries were changed
for these experiments. Parser/profile tests8/8 passed in0.08 s.
