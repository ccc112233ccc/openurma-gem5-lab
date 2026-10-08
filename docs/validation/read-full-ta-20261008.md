# Exact-length URMA CTP transactions

## Protocol evidence and implementation boundary

Source: Huawei/UnifiedBus Base Specification2.0 Chinese, published through
[the official portal](https://www.unifiedbus.com/zh), inspected from the
[PDF mirror](https://github.com/codehubcloud/UnifiedBus/blob/master/UB-Base-Specification-2.0-zh.pdf).
SHA256: `ebed6871cd4681c49d672a2548067358dc23f1d97d258fe64277b912b9a458eb`.
Pages197–198 discuss full/compact TA formats,203 requires matching response
format,206–207 define full64-bit address/32-bit byte length versus compact
64-byte-aligned power-of-two length,229 describes READ requests/responses,
and153 specifies transport padding. The complete page207 table/diagram was
also rendered and visually checked. No new private hardware timing constants
were inferred from this document.

Production CTP incorrectly used compact TA encoding for arbitrary URMA WQEs.
Small/non-power-of-two READ lengths were rounded on wire; the target generated
a rounded response that the initiator correctly rejected. Fix0008 consistently
uses full BTAH/ATAH/MAETAH, not a size-specific hidden tag or relaxed completion
check. It retains the existing CTP/UPI/EID network envelope. Padding to4-byte
transport alignment is real packet overhead and is removed before parsing
response data. Full headers also carry requester ID and order/presence flags.
Unsupported error/poison/context responses cannot retire a successful WQE.

The short-tail test exposed another issue: ordinary READ/WRITE segments copied
the WQE base address without adding their offset. The common segment generator
now advances linear memory requests and updates their memory-access fields.
An adapter-side address/length/token consistency check prevents the external
DMA metadata from concealing a future wire mismatch. This is not a claim of
complete UB wire conformance; other unsupported protocol operations remain out
of this performance-path scope.

## Native acceptance (not official-driver execution)

`ctp-read-length-contract` sends17 READ WQEs through native CTP and a switch:
1/2/4/8/16/32/63/64/65/127/128/129/4095/4096/4097/4098/8193 B. Target checks
exact segment length/address/token/requester ID, including unaligned addresses
and high address bits, and requires one completion per WQE. It does not perform
real guest DMA or establish buffer-content integrity.

Five injected READ responses exercise error status, invalid context type,
poison, incorrect requester ID and incorrect length. A check before any valid
response can return requires no premature completion. The marked wrong-length
response must still generate congestion feedback; this caught/repaired a
READ-response FECN regression during review.

The first build/test failed on4097 B's tail address (length was already exact),
which led to the segment-offset repair. After repair, all17 cases and negative
checks passed. Existing recovery, feedback baseline/two-source/single-source,
ordering, request-loss retransmission, lost-TAACK suppression, retry exhaustion,
lost-READ-response replay and CNP pacing contracts all pass. Independent review
confirmed the checks are active in release builds and the identified defects
were addressed.

Wall time: first build/test15.28 s (failed expected address check), incremental
repair build2.53 s and length test0.50 s, final reproducible native build plus
all contracts15.25 s. An initial launcher attempt selected Docker (unavailable,
0.25 s); rerun used `UBSIM_EXECUTION_MODE=native` without changing containers.

## Full-system acceptance

`experiments/qemu-full-ta-functional-20261008` passed all12 official guest
tests: SEND latency/bandwidth, SEND ring wrap, READ/WRITE latency,4 KiB,
64 KiB and1 MiB READ/WRITE bandwidth, and bidirectional WRITE size scanning
from2 B to1 MiB. The no-sync suite took28.226 s; launch, suite and stop took
34.22 s. No-sync success establishes functional regression coverage, not
bandwidth accuracy or the absence of synchronization failures.

The first synchronized scan, `experiments/qemu-read-full-ta-20261008`, passed
2/4/8/16 B (0.47/0.94/1.88/3.77 MiB/s from perftest).32 B timed out after
180.211 s; subsequent sizes were not run. Total334.378 s, shell-ready46.598 s.
This removes the former2 B wire-format rejection but does not close the
continuous-scan timing gate. Two live device snapshots showed identical virtual
time4340998791000 ps,76800 fetched/75891 retired,909 ready entries and one
pending host DMA read. macOS QEMU samples are preserved alongside this run.

The client sample repeatedly enters MMIO suspend/instruction restore. Ordinary
`halted` is released by pending IRQ even when the guest cannot yet take that
interrupt. Retrying an unfinished MMIO restores icount; CPU non-idleness then
prevents warp to the device completion timer. An independent device-wait state
is under full-system validation; it preserves IRQs and releases only on the
device completion. The previous poll-SYNC repair remains relevant but did not
eliminate this distinct scheduling failure.
