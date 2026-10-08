# Independent request lifetime and actual late-response progress

This follows `ctp-terminal-drain-20261008.md`; it does not claim complete CTP
recovery or valid1 MiB timing bandwidth by itself.

## Implementation

Patch0009 adds a bounded simulator-only context value and segment offset,
copied WQE→segment→serialized PacketTag→received segment. READ carries it even
without explicit payload. The adapter uses it for external request identity
and routing rather than dereferencing a live source task. The actual UB wire
still supplies opcode, address, token and length; the adapter checks these
against the original requested range. Context neither adds wire bytes nor
changes modeled link service time. It is a replacement for existing simulation
association metadata, not a fabricated hardware header or a raw pointer.

The adapter now observes the real default MapScheduler event queue. A source
task count of zero is not sufficient for time fast-forward, fencing or exit.
`Simulator::IsFinished()` cannot substitute: after `Stop()` it returns true
even when queued events remain. All remaining events, including maintenance,
are conservatively executed; no timer duration is guessed or discarded.

This exposed a second actual bug: a late response reaching the source parsed
its16-bit TA number through a null transaction context. macOS crash stacks
identify `GetCompleteUna`/`ResolveWireTaSsn`, not the new context tag. Patch0010
looks up existing peer/source-group state only, traces receipt, and ignores
responses without a matching outstanding operation. It does not create a new
context or advance a new transaction's acknowledgment state. FlowTag task
association additionally prevents an old response completing a different
simulated operation; that check is not a claim of full wire-protocol security.

## Independent native acceptance

`experiments/ctp-request-context-native-20261009` contains the four process
contracts. Both peers use required synchronization; all delays are virtual.

| Case | Wall seconds | Evidence |
| --- | ---: | --- |
| Late WRITE DMA |0.062| One late response actually received at source |
| Late READ DMA |0.078| One late response actually received at source |
| First arrival after failure |0.109| One target execution; eight late responses received |
| Invalid DMA completion length |0.070| Specific mismatch rejection, not arbitrary crash |

The first-arrival case injects300 us **native switch delay**, not IPC
lookahead. Source fails at205.1 us; target first executes at300.4 us and
immediately acknowledges. Seven retransmissions are suppressed/replayed, not
executed again in this case. Peers continue through1 ms. All three valid cases
require submitted1, failed1, completed0, drained1 and pending target DMA0.
Real orphan scheduler events are observed (29 for late-DMA,159 for first
arrival). Maintenance events remain, so this is not scheduler-quiescence proof.

The exact-length native test runs17 READ sizes twice, with and without context,
including4097 B's offset4096 tail. Wire address/token/length checks remain.
Packet copy and serialize/deserialize preserve context independently after
the original tag/copy is removed, and context does not affect packet byte size.
An initial test crash was repeated `Time::SetResolution` across two simulator
runs; moving it to main once repaired the test without removing the roundtrip.
Final34-case plus roundtrip execution:1.32 s. Null-context repair rebuild2.44 s.
Reproducible full build and existing native contracts pass in16.51 s; the
initial build before fixes took17.82 s and failed the new test.

Use `late_dma_contract.py --profile ctp-late-first` with a newly built contract
peer to run the first-arrival case. Other commands remain as in the prior
record. The native tests do not execute an official guest driver.

## Full-system acceptance and remaining gates

`experiments/qemu-request-context-functional-20261009` passes all12 official
no-sync driver/provider regressions, including1 MiB READ/WRITE and the WRITE
size sweep. Suite29.452 s; launch, suite and stop35.57 s. These are functional
results, not synchronized bandwidth measurements.

The synchronized1 MiB repeat (`qemu-write-1m-independent-context-20261009`)
still fails, now with `invalid external target DMA completion`, rather than
the former missing-source-task exception. Total177.929 s, command129.702 s,
shell-ready45.688 s; remaining processes were explicitly stopped after ns-3
exited. There is no valid bandwidth result and devices were not quiescent.
Retry exhaustion still occurs, then target logs report remote WRITE translation
failures (e.g. token10, request15, address0xffffa277f000,4096 B). The target
model emits `RmaError` on that path, while adapter `IsTargetCompletion` accepts
only WriteAck/ReadResponse. Error completion handling remains a concrete gap;
it must not be converted to successful TAACK or ignored. Source failure and
guest resource cleanup timing need to be checked alongside this path.

Duplicate-response cache expiry still must not permit re-executing stale DMA
outside the tested horizon; retry start at local enqueue, congestion/queue
behavior and throughput limits remain separate investigations. Do not increase
RTO just to hide them. Native first-arrival acceptance establishes that branch,
not complete full-system recovery in all cleanup/error states.
