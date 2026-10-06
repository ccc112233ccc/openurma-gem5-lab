# ns-3-UB process adapters

The production boundary is `ub-net-adapter.cc`. It connects independent UDMA
device processes through the simulator-neutral UB-NET v1 ABI and uses native
ns-3-UB `UbSwitch`, `UbPort`, and `UbLink` objects for routing, serialization,
queueing, and packet delivery. UB-NET boundary timestamps own propagation
delay and conservative lookahead, so the internal links do not charge it a
second time. The device endpoint supplies one MTU-sized transaction segment.
The adapter admits it through the endpoint's native
`UbCtpTransactionContext`, which owns the per-Entity TASSN sequence, sliding
completion window, and backpressure, before encoding native compact
CTP/UPI/EID/TA headers. WRITE TAACKs and READ responses retire the native
window entry. The current external SEND ABI has no TAACK message, so its entry
is retired at target-port delivery. Compact EIDs are registered as multi-port
CTP Entities, so wildcard destinations use ns-3-UB Entity member selection.
The adapter supports both asynchronous execution and SimBricks-style
synchronization.

This is an endpoint transaction-state integration, not yet the final Jetty
integration. The external UDMA process still splits a WQE into MTU-sized
segments. The planned next boundary sends one WQE descriptor to the adapter,
lets `UbCtpTransportService`/Jetty perform segmentation and ordering, and uses
`UbTargetExecutor` only to call the external target DMA engine.

`ub-net-adapter.CMakeLists.txt` is installed as a self-contained ns-3 scratch
subdirectory. It compiles the portable SimBricks transport in the same process
without introducing a dependency on gem5.

The upstream ns-3-UB checkout is a generated dependency under
`sources/ns-3-ub` at the revision recorded in `SOURCE_REVISIONS.md`. During
`./lab build ns3ub`, the checked-in overlays are installed into that checkout
and the UB-NET adapter is built. The build then runs UB-NET process contracts
with synchronization disabled and required.

Keeping the complete adapter here makes the integration reviewable from this
repository. It also avoids depending on unpublished commits in a sibling
checkout. The adapter files retain their GPL-2.0-only SPDX declaration.
