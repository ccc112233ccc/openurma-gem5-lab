# ns-3-UB process adapters

The production boundary is `ub-net-adapter.cc`. It connects independent UDMA
device processes through the simulator-neutral UB-NET v1 ABI and uses native
ns-3-UB `UbSwitch`, `UbPort`, and `UbLink` objects for routing, serialization,
queueing, and packet delivery. UB-NET boundary timestamps own propagation
delay and conservative lookahead, so the internal links do not charge it a
second time. UDMA frames remain opaque to the network process. The adapter
supports both asynchronous execution and SimBricks-style synchronization.

`ub-net-adapter.CMakeLists.txt` is installed as a self-contained ns-3 scratch
subdirectory. It compiles the portable SimBricks transport in the same process
without introducing a dependency on gem5.

The following files are retained temporarily for the current full-system
launcher while its endpoint is moved out of gem5:

- `ub-gem5-adapter.cc` is the standalone ns-3 process.
- `ub-external-adapter-protocol.h` is its shared-memory ring ABI.
- `register-adapter-header.patch` registers the ABI header with the upstream
  ns-3 module build.

The upstream ns-3-UB checkout is a generated dependency under
`sources/ns-3-ub` at the revision recorded in `SOURCE_REVISIONS.md`. During
`./lab build ns3ub`, the checked-in overlays are installed into that checkout
and both adapters are built. The build then runs UB-NET process contracts with
synchronization disabled and required. This proves the new boundary without
pretending that the legacy full-system launcher has already been cut over.

Keeping the complete adapter here makes the integration reviewable from this
repository. It also avoids depending on unpublished commits in a sibling
checkout. The adapter files retain their GPL-2.0-only SPDX declaration.
