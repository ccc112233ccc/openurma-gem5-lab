# gem5 declarative topology validation (2026-10-20)

## Configurations

- `configs/topology-gem5-two-heads.json`: two modern gem5 heads, one UDMA
  device and one 400-Gbit/s port per head, 375-ns endpoint link propagation,
  one ns-3-UB switch with 500-ns service delay per pass.
- `configs/topology-gem5-asymmetric-links.json`: the same graph, with node1
  changed to 200 Gbit/s and 550 ns. Both files declare O3 LQ/SQ=64/64 and
  `stride_exclusive` L1D prefetching in their `machine` block.
- `execution.sync=required` compiles to `--roi-sync`: free boot followed by
  synchronized benchmark ROI. Each example uses `checkpoint=auto`.

## Actual run

1. The uniform example selected the existing
   `shell-ready-modern-ddr5-stock-lq64-sq64-link375-switch500-20261015`
   checkpoint. The first launch failed because the Docker ns-3 binary still
   accepted only `SOCKET,EID`; the current launcher passed
   `SOCKET,EID,DELAY_PS,RATE_GBPS`. Rebuilding `./lab --runtime=docker build
   ns3ub` passed its CTP and process contracts. The next launch and `sync`
   passed.
2. The initial synchronized SEND smoke exposed a UB-HOST lifecycle fence
   error. A completed timing DMA reply could remain in `dmaOperations` until
   the next poll, although its response was already sent. The fence now
   retires completed records before checking for genuinely outstanding work.
   Rebuilt `./lab --runtime=docker build gem5` successfully. The uniform
   `--sync-smoke` suite then passed all 5 cases in **53.327 s**, including
   SEND/WRITE/READ latency, 128-B SEND bandwidth, and 4096-B READ bandwidth.
   Both UARTs and all five fence participants were checked for each case.
3. The asymmetric example had no compatible checkpoint, as expected: the
   retained uniform checkpoints encode node1 as 400 Gbit/s / 375 ns. One
   cold boot reached both shells and passed guest timer and OOB checks. The
   coordinated checkpoint
   `shell-ready-modern-gem5-asymmetric-400-200-link375-550-20261020` was
   saved in **114 s**. The boot plus guest initialization was approximately
   **324 s** from the gem5 start timestamp to checkpoint initiation.
4. `./lab topology start configs/topology-gem5-asymmetric-links.json` then
   selected that checkpoint automatically. `./lab sync` passed; the same
   synchronized 5-case smoke suite passed in **64.670 s**, with no case or
   fence validation errors. The ns-3 log confirmed EID 256 at
   400 Gbit/s / 375 ns and EID 257 at 200 Gbit/s / 550 ns. Finally, the
   example JSON was run again with no extra environment variables; it
   restored the checkpoint and passed `sync` again.

Raw case reports and UART/fence excerpts are in
`experiments/gem5-topology-sync-smoke-retry-20261020/` and
`experiments/gem5-topology-asymmetric-sync-smoke-20261020/`.

These 8/16/64-operation smoke cases establish startup, communication, and
ROI synchronization. They are too short for a meaningful bandwidth or
physical-machine accuracy comparison. The 128-B READ latency smoke emits
only two measured samples, so its numerical mean is not a calibration result.

For a later run, start either JSON file with `./lab topology start FILE`,
then run `./lab sync`. Both examples now carry their CPU tuning in the JSON,
and both have a compatible ready checkpoint. Stop with `./lab stop`.
