# Source revision manifest

This file ties the orchestration repository to the independent source
repositories used by the current checkpoint.  It is updated whenever a stage
changes code in one of those repositories.

| Component | Path | Upstream baseline | Stage commit |
| --- | --- | --- | --- |
| OpenURMA integration and scaffold | `../OpenURMA` | `0ae5dce300154d761f97095864bda0cf2546b265` | `1242e79` |
| vendored official UMDK | `../OpenURMA/integration/umdk/vendor/umdk` | `4eab3e4ad170b06bfe5d5c1014341e81edb9bf58` | `6878bcb` |
| gem5 | `./gem5` | `b1a44b89c7bae73fae2dc547bc1f871452075b85` | `36b6c5b99` |
| OpenEuler OLK 6.6 | `./oe66` | `5078a3a23a1e1825ec136485173ec98668cdd640` | unchanged; local case-folding noise is not committed |
| UMMU userspace dependency | `./deps/ummu` | `f1930d006e08bbe96dfa6fa037ff8a386f535425` | unchanged |

All repositories changed by the current checkpoint use the annotated tag
`stage/2026-09-17-ubus-resource-discovery`.
