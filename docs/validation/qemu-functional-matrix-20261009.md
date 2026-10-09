# QEMU functional acceptance: data, queues and multiple resources

The user paused performance tuning. All full-system cases here use no-sync;
perftest bandwidth columns are not performance evidence. Official UMDK/provider
and kernel driver code are unchanged. A separate `urma_integrity` application
uses the official URMA API to validate bytes and completion records.

## Reproduction

On the existing Linux ARM64/cross builder with UMDK built:

```sh
bash scripts/build/build-urma-integrity.sh
```

On the host, start `./lab start-qemu-dual`, wait for both shell-ready banners,
and run `ubsim-net-up` in both guests. With localhost SSH available:

```sh
UBSIM_RMA_SUITE=outstanding512 ./lab qemu-rma-regression experiments/new-depth512
UBSIM_RMA_SUITE=boundaries ./lab qemu-rma-regression experiments/new-boundaries
UBSIM_RMA_SUITE=multi UBSIM_RMA_JETTYS=4 ./lab qemu-rma-regression experiments/new-multi
python3 tools/run_qemu_integrity.py experiments/new-integrity --suite boundaries
python3 tools/run_qemu_integrity.py experiments/new-wrap --suite wrap
python3 tools/run_qemu_integrity.py experiments/new-concurrent --suite concurrent
python3 tools/run_qemu_integrity.py experiments/new-shared --suite shared
```

Output directories must be new. The integrity runner transfers only the test
binary using localhost SSH; no guest SCP/SFTP package is required. It records
binary hash, commands, UART transcripts and wall time. Its final acceptance
requires exact per-endpoint markers and zero process exit codes.

## Official perftest results

| Matrix | Cases passed | Wall seconds | Evidence under experiments/ |
| --- | ---: | ---: | --- |
| SEND/READ/WRITE, depth512, batch/CQ1,16,512 | 9/9 | 23.607 | functional-depth512-20261009 |
| SEND through4096 B; READ/WRITE through65536 B, non-power-of-two boundaries | 36/36 | 80.662 | functional-boundaries-20261009 |
| Four Jettys, independent JFRs,128/4096 B, all three verbs | 6/6 | 16.337 | functional-multijetty4-fixed-20261009 |

Boundary sizes:1,2,63,64,65,127,128,129,4095,4096 B; RMA additionally4097,
65535,65536 B. Each perftest boundary case performs64 operations per direction.
Depth512 performs2048 operations per direction per case. Four-Jetty cases
perform512 operations per Jetty per direction and validate printed totals2048.

The first four-Jetty SEND attempt failed with provider status12 before traffic:
this perftest revision preposts `jfr_depth` receives for *each* Jetty, but its
default gives all Jettys one shared JFR. `--jettys_pre_jfr 1` explicitly gives
each Jetty its own JFR and permits the unchanged official benchmark to run.
Increasing the shared depth alone would not fix that proportional overposting.
The failed evidence remains in `functional-multijetty4-20261009`.

Saved depth+boundary device logs (`functional-matrix-20261009`) show20736 WQEs
and20736 completions per endpoint,22 SQ wraps, actual pending-RMA peak512,
zero SQ decode/depth/UBASE errors and fully drained final state. This peak is
aggregate across the suite, not a per-verb/per-case assertion.

Four-Jetty logs show12288 WQEs/completions each and both physical ports used:
TX port0/1=6144/14336 frames; RX port0/1=8192/12288 frames on each endpoint.
Total peer TX and RX agree; per-number equality is not required because
destination-port selection is not necessarily same-number wiring. These
adapter wire counters include UDMA headers, not physical framing. Counters
prove traffic use, not payload integrity by themselves.

## Byte-level probe contract

The probe checks a nonzero byte pattern derived from offset,round,slot and
session port. TCP exchanges metadata/barriers only, never the tested payload.
READ alternates which side supplies data; SEND/WRITE alternate initiator.
Both sides verify the full payload and32-byte guards after each round.
Distinct port seeds make concurrent process cross-delivery detectable.

Completion checks require success, expected send/receive direction, unique
`user_ctx` within the round, receive length, Jetty flag/local ID/object pointer,
and no extra completion after the batch. Every round drains before buffer
reuse. Resource deletion/unregistration results are checked before PASS.

The shared suite rotates two Jettys through the same JFR and send/receive CQs
in direction-pairs. It validates shared-resource reuse and identities, not
simultaneous cross-Jetty arrival ordering. The concurrent suite runs two
independent processes/contexts on each guest with different data seeds.

Initial probe acceptance (before strengthened completion identity checks):
smoke3/3 in6.928 s; boundaries36/36 in79.933 s; batch512 wrap3/3 in11.348 s;
two-process concurrency3/3 in7.163 s. Saved `integrity-matrix-20261009` logs
show actual pending-RMA peak512 on both endpoints,6234 WQEs/completions,
six SQ wraps and fully drained state with zero notification/CI/UBASE errors.
These are retained as preliminary evidence, not substituted for the final
identity-checking probe's results.

## SEND overlength error discovered and repaired

The first4097 B SEND probe received API status1 (`UNSUPPORTED_OPCODE_ERR`)
at the sender. Its positive-only receiver waited for nonexistent successful
data and hit the30 s host deadline. This was not a sender that never completed.
The generic network-adapter failure path reported the wrong error category.

The model now rejects supported single-SGE SEND/SEND_IMM lengths above4096
before payload DMA/network submission. CQE raw status2/substatus1 is preserved
through ordered SQ retirement and asynchronous JFC publication; the official
provider maps it to API `URMA_CR_LOC_LEN_ERR` (2). This keeps the project's
existing SEND capability, not a claim about all physical UB implementations.
Signaled requests are covered; unsignaled-error notification policy is unchanged.

`--suite send-overlimit` is now a strict negative test: exact length error plus
completion identity, receiver bytes unchanged, no receive completion, normal
teardown on both peers. Native tests also run4096→4097→4096 on the same SQ
for SEND and SEND_IMM, including an invalid payload address on the rejected
descriptor to prove it is not DMA-read. Independent review confirmed the
official raw/API mapping and callback ownership. Independent native rebuild
3.14 s/tests5.85 s; production rebuild7.13 s/tests12/12 in5.53 s.

## Final post-fix full-system acceptance

All77 selected full-system cases pass on the rebuilt model in one no-sync
guest lifetime; launch-to-stop wrapper206 s. This includes the rejected
oversize SEND followed by ordinary traffic, not just fresh-boot success.

| Suite | Cases | Wall seconds | Evidence directory under experiments/ |
| --- | ---: | ---: | --- |
| Correct SEND4097 length error, unchanged receiver, clean teardown | 1 | 2.449 | integrity-fixed-send-overlimit-20261009 |
| Byte/guard/completion identity boundaries | 36 | 79.893 | integrity-fixed-boundaries-20261009 |
| Eight rounds ×512 batch,4096 B, three verbs | 3 | 11.522 | integrity-fixed-wrap-20261009 |
| Two concurrent processes per node, distinct data seeds | 3 | 7.218 | integrity-fixed-concurrent-20261009 |
| Two Jettys sharing JFR/CQs, eight direction-paired rounds | 3 | 11.594 | integrity-fixed-shared-20261009 |
| READ/WRITE65537 B and1 MiB byte checks | 4 | 9.404 | integrity-fixed-large-20261009 |
| Official perftest depth512 × batch/CQ1,16,512 | 9 | 24.572 | functional-fixed-depth512-20261009 |
| Official perftest four Jettys | 6 | 16.850 | functional-fixed-multi-20261009 |
| Original latency/bandwidth/fragment/scan matrix | 12 | 30.356 | functional-fixed-full-20261009 |

Saved final logs and manifest: `functional-fixed-matrix-20261009`. Endpoint0/1
report44579/44746 WQEs, exactly matching their respective completion counts,
137 SQ wraps each, actual pending-RMA peaks512 each, zero decode/depth/UBASE
errors, zero CEQ consumer rejects/notification failures and quiescent final
state with no host DMA, RMA, notification-slot or destroy ownership. Both
physical ports carry traffic. Mixed large-RMA adapter TX/RX frame counts need
not match one-to-one across network segmentation/reassembly; do not use them
as a packet-loss oracle. These aggregate counters do not identify each verb's
peak separately. Python checker/runner/CLI regression79/79 passes in0.317 s.

## Independent native acceptance

- New `udma_outstanding512` holds512 distinct READ or WRITE WQEs (unique request
  IDs), submits in eight batches, completes out of order, stalls CQ DMA and
  releases64 at a time. Another Jetty/JFC/port can progress without modifying
  the first READ destination. Two ring generations and over-capacity producer
  rejection are checked. This is not512 segments belonging to one WQE.
- Remote-service tests cover14 READ/WRITE cases at address8191 with sizes1,
  4095,4096,4097,65535,65536,65537: exact DMA slices, byte patterns, reverse
  callback order and no early completion. These test DMA pages, not network MTU.
- Existing CEQ credit tests cover full event rings, invalid consumer advances,
  fair admission and delayed callbacks; SQ tests explicitly stall CQ DMA.
  These remain native controlled
  backpressure evidence, not a claim that the full-system run forced every
  queue-full condition. CQ DMA backpressure is not proof of JFC full-ring
  credit recovery: the latter currently fails closed instead of retrying.
- Adapter port-counter tests count only successful TX publication and each
  received wire fragment, with exact byte totals.

Final device/native suite12/12 passes in0.82 s (local socket permissions needed).
Production counter build1.36 s; independent512 fixture build1.83 s/full native
suite5.68 s; final integrated fixture build5.62 s. Remote DMA test build1.21 s,
test0.37 s; counter test build1.88 s,test0.48 s. An initial sandboxed process
contract failed to connect its local sockets; rerun with socket permission
passed, and the failed attempt is not counted as model acceptance.

## Boundaries of the claim

The SEND4097 signaled negative case now establishes the correct length error;
passing other upper supported sizes does not establish every oversized-request
or unsignaled-error behavior.
Historical1 MiB RMA and the exploratory large suite exceed the advertised64 KiB
message capability; passing bytes does not silently raise that capability.
Complete concurrency/fault combinations, all shared-JFR arrival interleavings,
and all possible port-routing policies are not exhaustively proved by a finite
matrix. No physical performance conclusions are made.

An added independent JFC depth2 counterexample explicitly confirms the open
overflow behavior: after two CQEs, the third does not overwrite them, retains
SQ ownership and blocks checkpoint; even after consumer advance and scheduler
retry it does not resume. Build1.80 s/test0.47 s pass *as a characterization*,
not acceptance of credit recovery. Official interfaces expose JFC error events
and error state, but this stage does not implement/validate their overflow
notification/recovery sequence. The complete functional scope remains open
on this point; shared-JFR simultaneous interleavings also remain unvalidated.
