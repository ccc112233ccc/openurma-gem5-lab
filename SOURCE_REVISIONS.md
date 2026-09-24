# Source revision manifest

This file ties the orchestration repository to the independent source
repositories used by the current checkpoint.  It is updated whenever a stage
changes code in one of those repositories.

| Component | Path | Upstream baseline | Stage commit |
| --- | --- | --- | --- |
| OpenURMA integration and scaffold | `sources/OpenURMA` | `0ae5dce300154d761f97095864bda0cf2546b265` | `0381d0b61a17c3614446b13a4e0d842045d69c01` |
| vendored official UMDK | `sources/OpenURMA/integration/umdk/vendor/umdk` | `4eab3e4ad170b06bfe5d5c1014341e81edb9bf58` | `f84b90b8ddd8173b851334f55d332783d248bfc7` |
| gem5 | `./gem5` | `b1a44b89c7bae73fae2dc547bc1f871452075b85` | `54c9d7cc2c6c3cb3bc215716ba1e632df18d84e4` |
| OpenEuler OLK 6.6 | `./oe66` | `5078a3a23a1e1825ec136485173ec98668cdd640` | unchanged; local case-folding noise is not committed |
| UMMU userspace dependency | `./deps/ummu` | `f1930d006e08bbe96dfa6fa037ff8a386f535425` | unchanged |
| ns-3-UB fabric | `sources/ns-3-ub` | `d6aa9e242d5a93f5bbd1ad54f39b1620c1b8757b` | unchanged; complete lab adapter is under `integrations/ns3ub/` |
| Mooncake native URMA benchmark | sibling `../Mooncake` | `1a0c0a44214ff61a8a4b2e9d90dfb023dd4703ed` | unchanged; built out of tree with `USE_UB=ON` |

The current checkpoint was runtime-validated with four full-system nodes,
arbitrary peer selection through official TP/EID setup, concurrent sessions,
and destination-routed official SEND/READ/WRITE paths on 2026-09-21.
Reviewable patch series and exact Git bundles live in `patches/source/`; the
bootstrap verifies their SHA-256 digests before restoring the commits. Stage
tags remain in this orchestration repository.
