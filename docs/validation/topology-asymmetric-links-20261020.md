# Per-endpoint UB links — 2026-10-20

The topology compiler now accepts different line rates and propagation delays
for different UDMA endpoints connected to one ns-3-UB switch. One device's
multiple ports still share a rate/delay pair. The native adapter accepts both
its original `SOCKET,EID` endpoint syntax and the new
`SOCKET,EID,DELAY_PS,RATE_GBPS` form. Each endpoint uses its own SimBricks
boundary delay; the endpoint and corresponding switch `UbPort` use its own
line rate; the native CTP service receives the same rate for congestion pacing.

The QEMU launcher passes each device its matching UB-NET delay and writes
per-node values into `run-manifest.txt`. The gem5 launcher does the same and
includes those values in `--print-config` so checkpoint selection compares
the complete requested topology. Old uniform checkpoints remain compatible
only with uniform per-node settings that equal their saved global values.

`./lab --runtime native build ns3ub` rebuilt the adapter and passed native
CTP plus process boundary contracts. Its first in-sandbox contract attempt
failed because the sandbox denied Unix socket creation; the repeated run with
local socket access exited 0. The build transcript is retained in
`experiments/topology-asymmetric-ns3-build-20261020.log`.

`configs/topology-qemu-asymmetric-links.json` launched two QEMU guests, two
UDMA device processes, and one ns-3-UB fabric. Both guests reached the shell.
The switch log reported `eid=256 delay_ps=375000 rate_gbps=400` and
`eid=257 delay_ps=550000 rate_gbps=200`. The official URMA 4-KiB integrity
smoke accepted SEND, WRITE, and READ (3/3, 7.04 s workload wall time). Run
artifacts are in `experiments/topology-qemu-asymmetric-links-20261020/` and
the result is in `experiments/topology-qemu-asymmetric-links-integrity-20261020/report.json`.
All five simulator processes were stopped. This functional test verifies
connection and data integrity, not achieved rate or latency accuracy.
