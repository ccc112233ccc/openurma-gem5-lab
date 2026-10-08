# CTP source termination and remote DMA lifetime

## Full-system failure evidence

The latest full-TA/QEMU-device-wait binaries still fail synchronized
bidirectional 1 MiB WRITE. The baseline repeat is
`experiments/qemu-write-1m-device-wait-20261008`: total161.927 s,
command113.377 s before the remaining processes were explicitly stopped after
ns-3 exited. This is not a timeout measurement or a valid bandwidth result.

A bounded4096-entry diagnostic-only terminal history was added. It does not
admit, complete or suppress packets. The diagnostic rerun is
`experiments/qemu-write-1m-terminal-diagnostic-20261008`: total158.560 s,
command109.765 s, then explicit shutdown of the failed environment.

The log establishes the previously ambiguous terminal cause:

- task7/request7 is a1 MiB WRITE,256 admitted segments, TA1536–1791.
- It fails at3502466199840 ps, with source retry-exhaustions1,
  cumulative source retransmissions6920, and no target DMA currently pending
  for that task. Task8 then also exhausts retries.
- TA1767 of task7 reaches target execution at3502482596120 ps,16.39628 us
  after failure. The adapter has deleted the source task and throws
  `target execution cannot resolve external WQE`.
- Subsequent FLUSHED records are teardown fallout, not the original cause.

Source inspection shows retransmission timers begin at enqueue, not successful
link transmission. That can include local queue delay, but this run does not
yet isolate how much of the exhaustion comes from that delay versus target
service and network queues. Do not claim that merely increasing RTO fixes the
model. The fatal source-task/late-target lifetime dependency is established.

## Fast independent counterexample

`ctp-late-dma` uses two UB-NET contract peers and the real ns-3 adapter with
required synchronization. A32 B WRITE target delays its DMA completion by
300 us of virtual time. RTO25.6 us with7 retries produces exactly one source
failure at205 us; peers remain connected until400 us. At300.5 us, the delayed
target reply exposes a related bug: `target DMA completion lost its native WQE`.

The unfixed adapter fails this test in0.552 s, with pending_target_dma1.
Evidence is preserved under `experiments/ctp-late-dma-negative-20261008`.
This is a native process-boundary counterexample, not an official-driver run.
It avoids repeatedly booting guests to exercise this particular failure.

## Limited repair

Accepted target DMA now owns its completion association independently of the
source task: endpoint/EID, operation, Jetty/TPN, request/TA identity, range,
payload expectation and callback. Replies must match it exactly. If the source
task is active, READ data is saved for its normal completion; if already
terminal, the accepted target callback still drains, without recreating or
successfully completing the source WQE. Pending target work also participates
in idle advancement, lifecycle fences and shutdown checks.

The fixed WRITE counterexample passes in0.993 s: one submitted WQE, one failed,
zero completed, seven retransmissions, one post-terminal target drain, zero
pending target DMA. Native terminal diagnostics build/contracts took36.83 s;
the repair-only rebuild took2.28 s. An earlier build compiled successfully but
exited3 during the process contracts after42.38 s; the authorized rerun passed.

Independent acceptance extended this to delayed READ and malformed WRITE
completion length. Final counted READ/WRITE runs pass in0.050/0.054 s,
requiring exact submitted1/completed0/failed1/drained1/pending0 statistics.
The malformed-length test passes its negative expectation in0.053 s, requiring
the specific mismatch diagnostic and exit1, not just any simulator crash.
Logs are archived as `experiments/ctp-late-{read,write,invalid}-20261008`.
The normal official-driver no-sync suite also passes all12 cases after the
repair (`experiments/qemu-terminal-drain-functional-20261008`):28.310 s for
tests,34.29 s including launch and stop. This includes1 MiB READ/WRITE and
2 B–1 MiB WRITE scanning; it is not synchronized performance acceptance.

This repair does **not** close the full1 MiB failure: a first arrival after
source termination still needs source-independent request context. Response
cache expiry must also not permit repeated DMA, and late network packets need
their own in-flight tracking after the target callback drains. These are
explicit remaining gates, not waived by native or no-sync success.

Reproduce (use a new output directory on each invocation):

```sh
cmake -S components/ub-switch-sim -B /tmp/ubsim-late-peer -DBUILD_TESTING=ON
cmake --build /tmp/ubsim-late-peer --target ub-net-contract-peer -j 4
python3 integrations/ns3ub/tests/late_dma_contract.py \
  "$PWD/sources/ns-3-ub/build-macos/scratch/ns3.44-ub-net-adapter" \
  /tmp/ubsim-late-peer/ub-net-contract-peer /tmp/ubsim-late-result-new
```

On Linux use the corresponding built adapter path instead of `build-macos`.
Add `--profile ctp-late-read` for READ or `--profile ctp-late-dma-bad-length` for
the intentionally malformed WRITE completion. The default is `ctp-late-dma`.
