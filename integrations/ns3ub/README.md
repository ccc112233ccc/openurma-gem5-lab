# ns-3-UB process adapters

Request association now travels by value with native packets (patch0009), so
target execution does not require a live source WQE. The adapter validates
wire ranges and reply identity, drains accepted DMA independently, and checks
the actual ns-3 scheduler before idle time fast-forward. Patch0010 ignores
late replies without an outstanding source operation rather than creating a
new context. See [lifetime acceptance](../../docs/validation/ctp-request-context-20261009.md)
for fast failure contracts, official-driver evidence and remaining1 MiB timing
gaps; these changes do not constitute complete error/replay recovery support.

The production boundary is `ub-net-adapter.cc`. It connects independent UDMA
device processes through the simulator-neutral UB-NET v1 ABI and uses native
ns-3-UB `UbSwitch`, `UbPort`, and `UbLink` objects for routing, serialization,
queueing, and packet delivery. UB-NET boundary timestamps own propagation
delay and conservative lookahead, so the internal links do not charge it a
second time. UB-NET UDMA wire ABI v3 carries one complete WQE envelope from
the standalone device process; large payloads may use IPC-ring chunks, but
those chunks are not transport segments. The adapter reassembles the envelope
and submits the WQE to a prepared native `UbJetty`.

`UbCtpTransportService` then owns 4-KiB transaction segmentation, per-Entity
TASSN allocation, the sliding completion window, ordering, member-port
selection, CTP/compact UPI/EID headers, full transaction headers, response processing, and the
virtual-time timeout/retransmission state for WRITE and READ requests. The
adapter's `UbTargetExecutor` calls the external target UDMA for the physical
DMA action of each native segment. WRITE TAACKs and READ responses retire the
native window entry; actual READ bytes remain outside ns-3 and return to the
initiator only after native response completion. Compact EIDs are registered
as multi-port CTP Entities, so wildcard destinations use ns-3-UB Entity member
selection. Native CBFC protects multi-segment bursts from switch-queue loss.
The adapter supports both asynchronous execution and SimBricks-style
synchronization.

Official UDMA SQE `place_odr` values are carried through UB-NET flags and map
directly to native NO, RO and SO WQEs. The build runs a focused independent-
process contract that submits one WQE of each type, checks three native
submissions/completions with no packet drop, and verifies that SO does not
overtake the previously submitted RO. This supplements the stock
`urma_perftest` matrix, whose per-WQE placement order remains NO.

The adapter itself defaults retransmission to off. The QEMU and gem5 launchers
resolve `UBSIM_CTP_RETRANSMISSION=auto`: it is enabled only with conservative
synchronization required from virtual time zero, and disabled in asynchronous
functional or lifecycle-only runs. This prevents host scheduling delay at an
external UDMA process from masquerading as a simulated CTP timeout.
`UBSIM_CTP_RTO_PS` and `UBSIM_CTP_MAX_RETRANSMISSIONS` configure the
virtual-time policy; explicit `on` and `off` overrides remain available.

`ub-net-adapter.CMakeLists.txt` is installed as a self-contained ns-3 scratch
subdirectory. It compiles the portable SimBricks transport in the same process
without introducing a dependency on gem5.

The upstream ns-3-UB checkout is a generated dependency under
`sources/ns-3-ub` at the revision recorded in `SOURCE_REVISIONS.md`. During
`./lab build ns3ub`, the script restores the pinned CTP sources, applies
`patches/0001-ctp-timeout-retransmission.patch` and
`patches/0002-ctp-write-duplicate-suppression.patch` followed by
`patches/0003-ctp-retry-exhaustion.patch` and
`patches/0004-ctp-read-response-replay.patch` followed by
`patches/0005-ctp-cnp-pacing.patch`, installs the checked-in
adapter overlay, and builds it. It then runs UB-NET contracts with
synchronization disabled and required, the native ordering contract, and a
fault-injection contract that drops the first WRITE request and requires one
timeout retransmission followed by exactly one WQE completion. A second fault
contract drops the first TAACK and proves duplicate WRITE suppression plus
TAACK replay. A third contract permanently drops one request, exhausts a
two-retry budget, and requires a failed native WQE plus an initiator-visible
RMA error completion. A fourth contract drops the first READ response and
requires one retransmission, one duplicate READ suppression, one cached
response replay, and exactly one target-side READ execution.

Subsequent patches0006/0007 add congestion recovery/shared wakeups and queue
feedback. Patch0008 uses full BTAH/ATAH/MAETAH for URMA CTP traffic, including
exact per-segment byte length,64-bit address and requester context. CTP is a
transport choice, not a requirement to use compact transaction headers.
The separate Load/Store compact codecs are unchanged. CTP tail padding is
serialized and removed at receive; it is not application payload. Ordinary
READ/WRITE segment addresses now advance by their WQE offset. The external
target checks decoded address/length/token against the DMA request range,
preventing adapter metadata from masking a wire-encoding error.

The build also runs `ctp-read-length-contract`:17 sizes including unaligned
addresses and short tail fragments, plus malformed-response and marked READ
response feedback checks. This native contract does not run official drivers;
full-system evidence is recorded under `docs/validation/`.

Native CTP CNP reception now changes data-plane behavior instead of only
recording a counter. Congestion state is isolated by destination node, EID and
VL; each CNP halves that context's current rate down to a minimum derived from
the configured port rate. Subsequent CTP data fragments are released on that
virtual-time pacing schedule, while CNP and ACK control traffic bypasses the
data pacing gate. `cnp_pacing_contract.sh` injects one CNP after the first of
three ordered native segments at a configured 2-Gbit/s port rate. It observes
one rate cut and at least 256,000 ps between the first and last segment-send
events, proving that the state changes the virtual-time data path rather than
only telemetry. Rate recovery remains the next control-law step.

Keeping the complete adapter here makes the integration reviewable from this
repository. It also avoids depending on unpublished commits in a sibling
checkout. The adapter files retain their GPL-2.0-only SPDX declaration.
