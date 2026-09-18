# Source revision manifest

This file ties the orchestration repository to the independent source
repositories used by the current checkpoint.  It is updated whenever a stage
changes code in one of those repositories.

| Component | Path | Upstream baseline | Stage commit |
| --- | --- | --- | --- |
| OpenURMA integration and scaffold | `../OpenURMA` | `0ae5dce300154d761f97095864bda0cf2546b265` | `536f6b1` (official RMA READ/WRITE plus streamed large-WRITE fragmentation) |
| vendored official UMDK | `../OpenURMA/integration/umdk/vendor/umdk` | `4eab3e4ad170b06bfe5d5c1014341e81edb9bf58` | `6d89296` |
| gem5 | `./gem5` | `b1a44b89c7bae73fae2dc547bc1f871452075b85` | `36b6c5b99` |
| OpenEuler OLK 6.6 | `./oe66` | `5078a3a23a1e1825ec136485173ec98668cdd640` | unchanged; local case-folding noise is not committed |
| UMMU userspace dependency | `./deps/ummu` | `f1930d006e08bbe96dfa6fa037ff8a386f535425` | unchanged |

The bandwidth checkpoint was runtime-validated with the official UDMA path on
2026-09-17. The previous latency checkpoint remains available as
`stage/2026-09-17-official-dual-node-perftest`; a new bandwidth tag pins the
revisions above. Stage tags are kept in this orchestration repository.
