# READ small-message bottleneck after SQ prefetch

This is a diagnosis, not another dataplane change. All experiments keep the
same QEMU, UDMA, ns-3, kernel and initrd builds. No latency was fitted to physical
measurements. SQ prefetch remains 64/16/4; READ is single-direction, n2048,
depth512, sizes 2 B then 4 KiB, with required virtual-time synchronization.
Host latency is 500 ns each way and the two-port topology remains 400G per port.

## Completed batch/CQ controls

| Post list | Q | 2 B Mpps | 4 KiB average GB/s | Trial wall s |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 16 | 3.203845 | 13.121964 | 151.618 (earlier three-size control) |
| 64 | 16 | 3.255258 | 13.313895 | 138.837 |
| 64 | 64 | 4.214547 | 17.065553 | 155.678 |
| 64 | 1 | 0.237664 | 0.973456 | 151.632 |

Evidence directories under `experiments/`: `sq-prefetch-read-matched-20261009`
(first two sizes precede its 64 KiB case), `read-batch64-cq16-20261009`,
`read-batch64-cq64-20261009`, `read-batch64-cq1-20261009`.
The three new runs have identical manifests and all five executable/image
SHA256 fingerprints match. Only the perftest Q command differs among them.
Every new run passes the exact-size strict checker, retires 4096 client WQEs,
zero server WQEs, four source SQ wraps and both live-work drain samples.
Checked datapath errors/retries/drops are zero. Source descriptor reads are
256 bursts and SQ doorbells 64 in each, with session RMA peak128.
Wrappers take 138.88 / 155.73 / 151.67 s respectively; no build was performed.

Reproduce each in a separate fresh evidence directory (Q=1,16,64):

```sh
python3 tools/run_qemu_timing_trial.py experiments/NEW --bw-verb read_bw \
  --unidirectional-read --sizes 2 4096 --iterations 2048 --depth 512 \
  --post-list 64 --cq-mod 16 --inline 0 --drain-timeout 8
python3 tools/check_qemu_write_trial.py experiments/NEW --verb read \
  --expected-sizes 2 4096
```

Interpretation:

- A fourfold larger submission batch at fixed Q16 improves small-message rate
  only about 1.6%; simply reducing doorbell count is not the main remaining fix.
- At fixed batch64, Q1→Q16 improves rate about13.7x. Q1's reciprocal throughput
  is about4.21 us per operation, consistent with the serialized completion
  chain's structural lower bound. This is throughput evidence, not a direct
  measurement of each callback's service time.
- Q16→Q64 improves only about1.29x, not4x. Completion overhead matters, but
  another limit emerges near4.2 Mpps. It would be wrong to attribute the
  entire remaining gap to CQ serialization.

Printed MiB/s was converted to decimal GB/s; no bidirectional halving or
summation is applied. These are bounded individual trials, not confidence
intervals or long-run saturation proofs.

## Guest instruction-time control

At L64/Q64, changing only `UBSIM_QEMU_ICOUNT_SHIFT=0` to `1` gives:

| icount shift | Instruction-time scale | 2 B Mpps | 4 KiB GB/s |
| ---: | ---: | ---: | ---: |
| 0 | 1 ns/instruction | 4.214547 | 17.065553 |
| 1 | 2 ns/instruction | 2.117733 | 8.673947 |

`sources/qemu-11.1.1/accel/tcg/icount-common.c:156` converts instruction count
by a left shift. This is an instruction-time sensitivity experiment, not a
physical CPU-frequency calibration; virtual timers/idle progression also
participate in QEMU time. No CPU instruction, provider or hardware-model code
was changed. All five artifact SHA256 records match, and the two manifests
differ only in `icount_shift`.

Evidence: `experiments/read-batch64-cq64-icount1-20261009`. Strict audit passes
both sizes, both live-work drain snapshots pass, and wall time is94.285 s
(wrapper94.33 s). The first command takes37.343 s including control setup,
versus94.420 s in the shift0 comparison. Wall setup time is not the virtual
bandwidth denominator and is not claimed to improve throughput.

Doubling instruction time approximately halves the high-Q rate. Together with
the Q sweep this supports two limits: completion publication/notification at
frequent CQ, and guest CPU/software execution time after amortizing completion
cost. It does not identify the exact split between provider WR construction,
perftest polling, interrupt handlers and other guest instructions. A function-
level profile and a suitable detailed CPU model are needed for that split;
changing icount solely to match the physical23 Mpps would not validate it.

## Source-backed distinctions

`sources/umdk/src/urma/tools/urma_perftest/perftest_run_test.c:1002` sets
`complete_enable` on every Q-th WQE. The provider copies the flag into the
descriptor (`udma_u_jfs.c:742`). `UdmaModel::RetireSq` skips CQ publication for
unsignaled WQEs. Therefore `sq_completions` counts retired WQEs, not CQEs;
N2048/Q16 implies 128 signaled completions, not 2048.

The serialized modeled completion path is:

```
read CQ consumer -> write CQE -> write CEQ event -> MSI write completion
                                                        |
                           release SQ/JFC publication <-+
```

Relevant functions in `components/udma-model/src/udma_model.cc`:
`RetireSq` (publishing gate), `WriteCqeNow` (consumer read/CQE write),
`ProcessCompletionEvent` (CEQ write), and `RaiseInterrupt` (MSI completion).
In the hot-cache case this contains four host round trips. The approximately
4 us lower bound at 500 ns one-way is a structural estimate, not measured
per-stage profiling. Translation misses and scheduling can add work.
`TranslateIoVirtual` and `TranslateToken` have caches; it is incorrect to say
every CEQ necessarily repeats a full page-table walk.

Polling does not prove the notification is erroneous. The official kernel
`oe66/drivers/ub/urma/hw/udma/udma_main.c:39,1194` defaults `jfc_arm_mode=0`,
documented as Always ARM; `udma_jfc.c:28` encodes `ALWAYS_ARMED`. The actual
packaged initrd's `/init` was read and loads `udma.ko` without a parameter
override. This is source/image evidence, not a sampled live sysfs value.
The model does not yet interpret all arming states; that broader gap does not
establish incorrect default notification for this experiment.

## Safe follow-on design, not implemented here

CQE visibility and notification completion need not share one global lifetime,
but decoupling them must not hide outstanding work from measurement or teardown:

1. Preserve per-JFC slot allocation and CQE commit order. Register notification
   ownership before releasing CQ completion to avoid reentrant destroy races.
2. Keep CEQ-write-before-MSI ordering, bounded event queues and full
   checkpoint/destroy tracking. Notification failure must not roll back or
   replay an already committed CQE.
3. Model CEQ consumer credits before allowing more notification backlog.
   Current CEQ state has producer but no consumer backpressure. The official
   `ubase_update_eq_db` in `oe66/drivers/ub/ubase/ubase_eq.c` reports consumed
   entries. Independent review traced resource1 through `ubase_ubus.h/.c`
   to the model's resource offset0x200000 (`UbiosConfigRead`, offset0x40050).
   Source unknown-MMIO counts are5165/1325/1133 for Q1/16/64: subtracting
   4096/Q gives the same1069 baseline. This agrees with the CEQ consumer
   doorbell path, but does not prove any event was overwritten in these runs.
   Bounding a software queue alone does not prevent overwriting unconsumed
   hardware ring entries.
4. Treat caching CQ consumer credits or parallelizing CQE writes as separate
   changes, with wrap, shared-JFC, failure and visibility-order tests.

Increasing Q is a diagnostic change, not a substitute for correct hardware
behavior. Likewise QEMU icount sensitivity is not a calibrated physical-server
CPU/cache measurement. Do not claim a unique bottleneck from source structure
alone or from the session-wide RMA peak.
