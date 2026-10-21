# Heterogeneous QEMU topology validation — 2026-10-19

The declarative topology launcher started two QEMU/TCG guests, two standalone
UDMA processes, and one ns-3-UB switch from
`configs/topology-qemu-heterogeneous.json`. Each UDMA used one 400-Gbit/s
physical port, 47 Jettys, a 375-ns per-direction UB-NET boundary delay, and
the switch applied 500 ns per pass. Synchronization was off, so this is a
functional run and makes no latency or bandwidth accuracy claim.

The run manifest records node0 as Cortex-A72, one core, 1024 MiB, and node1
as Neoverse-N1, two cores, 1536 MiB. Both guests reached the shell. Guest
inspection confirmed CPU part 0xd08 and online CPU 0 with 928164 kB `MemTotal`
on node0; CPU part 0xd0c and online CPUs 0-1 with 1441600 kB `MemTotal` on
node1. The difference between configured memory and `MemTotal` is guest
reserved memory.

The existing 4-KiB official URMA integrity smoke passed SEND, WRITE and READ
(3/3, about seven seconds of workload wall time). The run artifacts are under
`experiments/topology-qemu-heterogeneous-20261019/` and the integrity report
is `experiments/topology-qemu-heterogeneous-integrity-20261019/report.json`.
All five simulator processes were stopped after the check.
