# CEQ credits and asynchronous completion notifications

This stage follows `read-cq-diagnosis-20261009.md`. It implements hardware-model
behavior without changing the official driver/provider or fitting latencies.

## Behavior

- Decode official CEQ consumer doorbells at resource1 base0x200000, type2,
  EQN bits7:0 and CI bits55:32. Validate reserved bits and24-bit forward
  progress against published/reserved entries; reject stale/oversized updates.
  Ring-depth validation excludes values ambiguous in that CI space.
- Full CEQs wait for consumer credit instead of overwriting unread events.
  Already queued events for another CEQ with credit can progress.
- If guest visibility precedes the DMA completion response, preserve the
  consumer update for the reserved entry, but spend it only on DMA success.
  A failed DMA combined with early consumption is ambiguous and faults the
  CEQ rather than reusing its slot or checkpointing it as clean.
- CQ commit remains ordered: consumer read, CQE write, producer commit.
  Notification ownership is then registered before the CQ completion callback.
  CEQ DMA and MSI continue independently, in order. Notification failure does
  not roll back the visible CQE or replay its WQE.
- `UBSIM_COMPLETION_EVENT_SLOTS` (default64, range1..4096) bounds all reserved
  CQ work plus queued/active notifications. It is a model capacity, not a claim
  about a physical UDMA device. JFC admission rotates fairly across waiting
  JFCs. A full shared pool can still backpressure new work for other CEQs;
  this is not a claim of fully isolated per-CEQ resources.
- Slots blocked behind notifications cannot leave a past moderation deadline
  spinning at the head of virtual time. Slot release explicitly retries timed
  notifications. Destroy and checkpoint observe all accepted work.

Checkpoint format3 persists CEQ consumer state and rejects format2. Both host
launchers record format3 and the notification-slot budget in the manifest;
`start-ready` rejects incompatible older snapshots. No gem5 cold boot was used.
CQ consumer caching, parallel CQE writes and general arming-mode support were
not added in this stage.

## Independent counterexamples and cost

New native `udma_ceq_credit` covers small-ring fullness/no overwrite,24-bit
wrap and invalid CI, early visibility/ack, bounded reservations, cross-CEQ
progress, CQ versus CEQ/MSI failure, queued/active destroy, synchronous callback
reentrancy, timer blocking and checkpoint round-trip/version rejection.

Independent review found two real failures after an initial passing suite:

1. A continuously busy JFC could repeatedly take the sole free notification
   slot and starve an already waiting JFC. Central round-robin admission fixed
   it; completion callbacks no longer bypass that scheduler.
2. CEQ destroy could wait forever after CQ DMA failed or completed without a
   notification, because publication-idle did not wake the EQ destroy path.
   A temporary source copy with the fix removed reproduces the failed assertion.

Final independent core suite8/8 passes in2.88 s. The new test also passes ASAN
and UBSAN (build2.99 s, run0.58 s). Final full-device build+CTest costs9.42 s,
11/11 tests pass in5.08 s. Earlier superseded device build+tests cost15.08 and
14.49 s; they are not substituted for acceptance of the final counterexamples.
Python launcher/checker/CLI/runner tests81/81 pass in0.210 s; Bash syntax and
`git diff --check` pass. Native contracts alone do not establish driver support.

## Official-driver functionality

`experiments/ceq-pipeline-functional-20261009`:12/12 no-sync dual-QEMU cases
pass, suite29.641 s, launch-to-stop35.52 s. SEND/READ/WRITE include latency,
bandwidth, ring wrap,4 KiB/64 KiB/1 MiB fragmentation and the inline WRITE sweep.
As before,1 MiB exceeds the reported64 KiB capability; this test does not change
the advertised capability. No-sync output is not used for timing claims.

Saved device logs report CEQ consumer updates1281/1150, successfully published
events1538/1396, event queue peaks4/7, and zero consumer rejects, notification
failures, SQ decode/depth rejects and UBASE errors. Consumer-update count need
not equal event count: one interrupt handler can consume multiple CEQ entries.
Unknown MMIO writes remain1209 per endpoint, so this is not all-MMIO coverage.

## Synchronized Q1 control

`experiments/ceq-pipeline-read-q1-20261009` keeps the earlier
`read-batch64-cq1-20261009` command unchanged: single-direction READ,
sizes2 B/4096 B, n2048, depth512, L64/Q1, shift0, host500 ns, network100 ns,
400G per port, SQ prefetch64/16/4. Among five executable/image fingerprints
only the UDMA binary changes; the manifest adds only the event-slot budget
and state-format fields. No driver/image, CPU or latency changes were made.

| Payload | Before average GB/s | After average GB/s | Before/after Mpps |
| ---: | ---: | ---: | ---: |
| 2 B | 0.000472 | 0.000944 | 0.237664 / 0.472703 |
| 4096 B | 0.973456 | 1.936080 | 0.237659 / 0.472675 |

The near2x improvement is measured from official perftest output, not a
projected speedup. Small-byte GB/s reflects rounding in its printed MiB/s row.
The strict format3 audit and independent review pass. Client completes4096
WQEs/four SQ wraps, server posts zero. All4096 CEQ events are successfully
published;4087 consumer updates may each consume multiple events. Event queue
peak4 is within64, with zero rejected CI or notification failures. Both live
drain samples and the final snapshot report no event-slot ownership, event
work, RMA or host DMA pending. These counters do not assert that each guest
event was individually acknowledged, or establish global checkpoint quiescence.

Wall133.626 s (wrapper133.67 s), shell ready42.836 s, first command80.769 s,
second4.665 s. Full-ring backpressure, simultaneous multi-JFC fairness and
fault recovery have native counterexample evidence, not separate all-feature
full-system acceptance from this single-Jetty trial.

## Synchronized Q16 control

`experiments/ceq-pipeline-read-q16-20261009` repeats the earlier
`read-batch64-cq16-20261009` commands with the same L64/depth512/n2048
and sizes2 B/4096 B. Only the UDMA executable fingerprint changes.

| Payload | Before/after Mpps | Before/after average GB/s |
| ---: | ---: | ---: |
| 2 B | 3.255258 / 3.277597 | 0.006512 / 0.006554 |
| 4096 B | — / 3.270395 | 13.313895 / 13.395537 |

The changes are only0.69% in2 B message rate and0.61% in4 KiB bandwidth.
One trial does not establish a significant improvement. Decoupling clearly
helps Q1, but Q16 is essentially unchanged; CQ serialization is not the only
bottleneck. Guest instruction/IRQ attribution is the next investigation,
rather than assuming further notification pipelining will raise throughput.

Strict audit and independent acceptance pass:4096 WQEs/four SQ wraps,
256 published CEQ events and256 consumer updates, queue peak4, zero CI
rejects, notification failures or full waits. Two live drain samples and the
final snapshot have no notification, RMA, host DMA or fabric work pending.
Wall142.501 s (wrapper142.54 s), shell ready45.122 s, first command87.176 s,
second4.559 s. Both timing trials use required conservative synchronization,
fixed TCG icount shift0 and unchanged driver images; no-sync functionality
does not supply timing evidence.

## Measurement counters

`event_slots_in_use` includes pre-CQE reservations, queued and active work.
`event_queue_peak` counts only queued+active notifications, not pre-CQE
reservations. `events_published` counts successful CEQ DMA acknowledgements,
not WQEs or necessarily guest-consumed events. `ceq_full_waits` counts scans
finding no eligible CEQ, not time or unique full-ring episodes. The format3
performance checker requires zero notification errors/rejected CI, zero final
slot ownership and an event-queue peak within the configured bound.
