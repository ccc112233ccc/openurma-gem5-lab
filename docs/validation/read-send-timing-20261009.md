# Synchronized READ/SEND performance-path acceptance

## Scope and reproducibility

This stage extends the existing synchronized WRITE runner and independent
counter audit to explicit-size bidirectional READ and SEND. No model constants,
official driver/provider code or binaries are changed. Functional no-sync
coverage remains valid but is not used as timing evidence. Each trial owns and
stops its two QEMU guests, two UDMA processes and ns-3 fabric; no gem5 cold boot.

The runner accepts `--bw-verb`, `--iterations`, `--depth`, `--post-list`,
`--cq-mod` and `--inline`; old `--write-*` aliases remain. READ explicitly uses
`-I 0`, as required by official perftest, and READ/SEND explicitly set `-R`.
Duplicate sizes, silently reduced depth and incompatible batch settings are
rejected. The audit checks actual guest return codes, UART result rows,
command/printed parameters and operation-specific WQE/segment/DMA counters.
Its PASS is a final-counter audit, not full hardware conformance or a global
checkpoint. Two live drained snapshots are separately preserved by the runner.

## Capability discrepancy found before issuing READ

`experiments/qemu-read-1m-depth512-long-20261009` was rejected by the new
capability preflight: both guests advertise `max_msg_size=65536`. No workload
was issued, so its drained state is not a passed READ experiment. Shell readiness
took47.127 s; capability query2.712 s; whole diagnostic52.610 s.

This is not inferred from a binary ABI mismatch. Official source
`oe66/drivers/ub/urma/hw/udma/udma_main.c` defines `MAX_MSG_LEN=0x10000`,
assigns it to `caps.max_msg_len` and exports it as `dev_cap.max_msg_size`.
`sources/umdk/src/urma/tools/urma_admin/admin_cmd_show.c` reads this attribute
from sysfs; `urma_types.h` documents it as the public maximum message size.
The current perftest does not enforce this limit, which explains why earlier
1 MiB tests could complete. Those logs remain genuine successful transfers,
but they are **outside the official advertised capability**, not evidence that
the official driver declares 1 MiB support. Do not remove the check merely
because posting a larger WQE happens to work. Default trial sizes are now
4 KiB and64 KiB; SEND is additionally limited to4096 B.

## Accounting and known limits

READ's legacy fabric `payload_bytes` counter adds logical request bytes and
actual response bytes. Consequently it is twice application bytes for a clean
READ run. It is neither wire utilization nor the bandwidth numerator. The
audit reports application bytes separately and converts perftest's historical
`MB/sec` (MiB/s) output once: `GB/s = MiB/s * 1048576 / 1e9`. A `-B` row already
combines the two directions; exchanged endpoint rows must not be added again.

READ uses `ctp_read_responses`, WRITE uses `ctp_taacks`, and SEND uses
`ctp_native_send_delivery_completions`. SEND has no measured WQE-outstanding
high watermark in `rma_pending_peak`; configured depth512 and native TA window
512 cannot fill that evidence gap.

Independent source review located an additional limitation: official
`oe66/drivers/ub/ubase/ubase_eq.c` writes AEQ/CEQ consumer doorbells at the
mapped offset0x200000. The model currently ignores those writes and lacks EQ
consumer/fullness enforcement; event-driven interrupt/backpressure correctness
is not accepted. Ordinary provider CQ polling updates a RAM consumer index,
which `WriteCqeNow` reads by DMA: that is a different path. Historical
`unknown_queue_writes` only preserve the last address, so their entire count
cannot be attributed to EQ without per-address evidence. Port1's invalid admin
attributes are also retained as a diagnostic, not explained away as ABI skew.

## SEND4 KiB: functional completion, timing gate rejected

Evidence: `experiments/qemu-send-4k-depth512-long-20261009`, including an
independent `acceptance.txt`. Required synchronization,400G configured port
rate, `send_bw -B -s4096 -J1 -I64 -T512 -R512 -n16384 -l16 -Q16 -p0`.
Both guests return0;32768/32768 WQEs and32768 SEND delivery completions account
for128 MiB. Each UDMA completes16384 SQ entries with16 consumer wraps; all
recorded retries and failure counters are zero. Two live work snapshots drain
and final host DMA is empty. Native TA outstanding peaks at1; this does not
establish512 concurrent SEND WQEs. Unknown EQ writes and cleanup warnings remain.

The actual exchanged perftest row is peak1953.12 MiB/s, average2432.10 MiB/s,
message rate0.622617 Mpps. Average converts numerically to2.5502416896 GB/s,
but **is not an accepted bandwidth measurement**: average exceeds peak and the
checker rejects it. In the current `calculate_opt_delta`, i=0/j=16383 includes
the complete average interval. With common scale/units, taking its minimum
per-operation interval requires peak>=average. Post-list16 and CQ16 do not
explain the inconsistency.

Independent provenance inspection found the same SHA256 for the actual
initramfs ELF, build ELF and manifest:
`7724a94b9e10970bf18a40f45be11a36e19d316fec4a9b0d0f7a0444de9fa84e`.
Actual ELF disassembly includes the full-interval candidate, both2^20 unit
constants, the same start timestamp address and correct dual report addition.
An old CMake source path alone is not evidence of the cause. Runtime intermediate
values are needed; no formula or check has been relaxed to force a pass.

Wall time: shell47.323 s, command84.396 s, drain0.252 s, total137.081 s.

## READ64 KiB: high-concurrency retry failure

Evidence: `experiments/qemu-read-64k-depth512-long-20261009`.
`read_bw -B -s65536 -J1 -I0 -T512 -R512 -n2048 -l16 -Q16 -p0` passes both
capability checks but fails both guests with CR status9. Node0 stops posting
at832 (reported completed320); node1 at672 (completed160). No BW row exists.

The first fabric failure is source1/request162/task234 at3596771822680 ps,
with the first retry exhaustion and no pending target DMA. Its target node0
has no UBASE/translation error. Later target errors accompany guest failure
cleanup; aggregate DMA errors must not be mistaken for the established first
failure. Fabric final totals are1393 submitted,493 completed,900 failed,
64310 actual retries and64310 duplicate READ replays. Final work drains after
error retirement; this is not a successful workload. Model-local RMA peak512
is observed but is not accepted512-depth performance. The generic RmaError
mapping also makes guest status9 insufficient to distinguish failure causes.

Wall time: shell47.667 s, command83.840 s, drain0.256 s, total136.903 s.
Next diagnostic is a bounded native concurrent READ replay/response trace,
preserving25.6 us RTO and7 retries rather than hiding the failure with a larger
timeout. Full-system rerun follows a reproduced and independently tested fix.

## Stage status

This stage adds reproducible harness coverage and detects two failures; it
does not close READ/SEND timing acceptance.35 checker parser tests,9 runner
parameter tests and4 drain tests pass (about0.09/0.06/1.38 s respectively).
These are harness tests, not driver execution. No binary rebuild was required
for the trials. Remaining performance gates include these two failures,
controlled parameter sensitivity, timed reuse, data integrity and multi-Jetty/
multiport experiments. EQ events and invalid port1 admin attributes remain
explicit limitations, not prerequisites for unrelated management coverage.
