# CTP pacing and recovery validation

Date: 2026-10-08. Platform: local macOS/ARM64, native ns-3-UB process contracts.
No guest boot or official driver modification is involved in this stage.

## Implementation

Patch `0006-ctp-recovery-shared-wakeup.patch` adds an optional additive recovery
policy to the CTP endpoint. Each recovery interval adds the configured rate
step, capped at the line rate. A new CNP first realizes elapsed recovery,
then halves the current rate (bounded by the minimum) and resets the epoch.
Calculation is lazy and uses ns-3 virtual time; there is no idle polling event.
Both adapter options must be supplied together:

```text
--ctp-recovery-interval-ps 100000 --ctp-recovery-step-bps 250000000
```

These are experimental model controls, disabled by default, not extracted
hardware constants or a complete implementation of a standard congestion
algorithm. The shared congestion key remains destination node/Entity/VL.
All blocked source Entities are retained for the next pacing wake-up; the old
single-waiter implementation could omit another source sharing the key.

## Evidence and cost

- Native rate-law test: repeated CNP, additive recovery at exact interval
  boundaries, new-CNP epoch reset, line-rate cap, minimum floor, shared source
  Entity state, VL isolation and disabled recovery all pass.
- A/B process contract: same 3-WQE NO/RO/SO workload and 2-Gbit/s line rate;
  first-to-second native fragment gap is **0 ps without CNP**, **256,000 ps
  with CNP**. CNP rate-cut counts are respectively 0 and 1. Both preserve
  ordering and report zero runtime drops. The total three-fragment span is
  deliberately not used as rate evidence because it also contains SO waiting.
- The revised A/B test took **0.36 seconds wall time**.
- Final complete patch reapplication, compilation, native recovery test and
  all six process contracts passed in **14.60 seconds wall time**.
- First clean patch reapplication/build and regression attempt took **14.50
  seconds**: native recovery plus five existing ordering/loss contracts passed;
  the new A/B script exposed a macOS Bash 3 empty-array issue. That script was
  corrected before the successful focused run above.

## Coverage boundary

The shared-waiter fix is code-reviewed and exercised by existing regression,
but there is not yet a dedicated multi-source starvation contract. Recovery is
unit-tested at the rate-law level, not yet a switch-feedback end-to-end test.
No claim is made that a queue buildup currently generates native CTP CNP:
the existing switch DCQCN marking path checks IPv4 headers, while this path
uses CNA16. Wiring real congestion feedback is a separate remaining step.

## Reproduce

```sh
UBSIM_EXECUTION_MODE=native ./scripts/build-ns3ub-adapter.sh
```

The builder applies the pinned ns-3 patches, builds and runs the native
recovery test, then runs the process contracts when the contract peer is
available. On macOS, build `ub-switch-sim` first to provide that peer.
