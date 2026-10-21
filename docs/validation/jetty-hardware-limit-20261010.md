# Jetty count and simulator mapping, 2026-10-10

## What the repository proves

- Jetty is a URMA resource created by the application. `guest Jetty` and
  `native Jetty` in earlier simulator notes describe two representations of
  that resource across the gem5/UDMA/ns3ub boundary, not two hardware types.
  The openEuler [URMA User Guide](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20User%20Guide.ch.html)
  also lists `max_jetty` as the device capability for the number of Jettys
  supported by hardware.
- The checked-in OLK UDMA driver sets `dev_cap.max_jetty` to standard +
  public + user-controlled normal Jetty counts
  (`oe66/drivers/ub/urma/hw/udma/udma_main.c`, `udma_set_dev_caps`). The
  standard count is read from UBASE `jfs.max_cnt`, and the public range is
  read from firmware resource ranges. UBCORE exports this value as the
  read-only `/sys/class/ubcore/<device>/max_jetty` attribute.
- UDMA allocates standard IDs first, then user-controlled normal IDs, then
  public IDs. Allocation failure is returned to the caller; the driver does
  not alias an excess requested Jetty to an existing ID
  (`oe66/drivers/ub/urma/hw/udma/udma_jetty.c`,
  `udma_alloc_normal_jetty_id`).
- The pinned `urma_perftest` checks an explicitly requested duplex `-J`
  against `dev_cap.max_jetty` and errors on excess
  (`sources/umdk/src/urma/tools/urma_perftest/perftest_resources.c`). It
  creates exactly the accepted number. Its default `-J` may be chosen from
  capabilities elsewhere; that does not make an explicit `-J > max_jetty`
  silently clamp. Another upper-layer program could choose `min(requested,
  max_jetty)` before calling URMA, but no such behavior was established for
  this pinned perftest.

The previous simulated UBASE count 1024 plus public count 128 advertised
`max_jetty=1152` in guest sysfs, confirmed in
`experiments/read-batch64-cq64-icount1-20261009/capabilities.txt`. That
capability is inconsistent with the user's target-device limit of 47.
Also, the ns3ub adapter collapsed multiple guest-created Jettys onto one
native transport Jetty, which incorrectly shared ordering and retry state.

## Current correction and uncertainty

The modern one-port profile now advertises 47 total Jettys by modeling 46
standard IDs and one public ID. The standalone UDMA model also rejects a
raw create command after the configured identity pool is full. The split
46 + 1 is an assumption pending a passive read of the real device's
`max_jetty`, `max_jfs`, and reserved-ID range; 47 itself came from the user.
Other profile defaults retain 1152 for compatibility with historical
checkpoints. QEMU's physical-target runner defaults to 47.

A ready gem5 checkpoint contains the guest's previously discovered device
capabilities. The new manifest keys prevent an old 1152-Jetty checkpoint
from being selected as a modern 47-Jetty run. A new coordinated checkpoint
is required before full-system performance validation. All prior multi-Jetty
throughput measurements were made with the incorrect ns3ub mapping and
1152-Jetty advertised capability, so they are diagnostic observations, not
validated predictions for a 47-Jetty target.

## Validation

- Modern `--print-config` reports `max_jettys=47`, `public_jettys=1`.
- Model unit test checks UBASE and firmware counts and rejects the 48th
  raw create command with status 28; it passes.
- Linux Docker component build succeeds and all 12/12 component tests pass.
  The macOS process contract needed local Unix-domain socket permission;
  with that permission it also passed.

## Full-system validation

Linux Docker `./lab build udma-device` passed 12/12 tests. Linux Docker
`./lab build ns3ub` exited 0 after the envelope-aware process contracts
passed in both `off` and `required` synchronization modes. The ns3ub
single-Jetty and multi-Jetty tests, including a dropped READ response,
retry, deduplication and replay, passed. The production adapter now binds
the guest-created Jetty ID one-to-one to the native CTP Jetty ID. These
are simulator layers, not hardware Jetty categories.

No compatible modern checkpoint existed after the hardware capability
change. A single dual-node cold boot reached both shells and passed the
architected-timer/OOB checks; its start command returned in 6.68 s and
`./lab sync` took 96.51 s. Both guests then read
`/sys/class/ubcore/udma0/max_jetty=47` and `max_jfs=47`. The coordinated
checkpoint `shell-ready-modern-ddr5-jetty47-perjetty-20261010` saved in
126.68 s. `start-ready` selected that checkpoint and the restore `sync`
completed in 77.13 s. Each node had one 400-Gbit/s port, with a single
switch modeled at 500 ns per traversal.

The restored, synchronized, 128-byte one-way bandwidth smoke kept 4,096
*total* operations per case, JFS/JFR depth 8,192, JFC depth 524,288,
post list 256, and one physical port. Raw commands, guest transcripts,
fence checks, per-case wall times and results are in
`experiments/gem5-jetty47-perjetty-smoke-20261010/` and
`experiments/gem5-jetty47-perjetty-warm-j1-20261010/`.

| Jettys | READ Mpps | WRITE Mpps |
|---:|---:|---:|
| 1, restored first pass | 4.217 | 10.359 |
| 1, warmed repeat | 10.960 | 10.605 |
| 2 | 10.930 | 10.572 |
| 4 | 10.505 | 9.886 |

The low first READ is a checkpoint/guest warm-up effect and must not be
compared with warmed multi-Jetty rows. Under the fixed-operation benchmark,
there is no throughput improvement from 1 to 2 or 4 Jettys after warm-up.
This result differs from the older WRITE sweep because that sweep used the
shared native transport Jetty and the old serial source DMA behavior.

The upper-bound check with explicit `-J 48` printed
`Error: jettys: 48 out of range, max_jetty: 47.` on the client and exited
nonzero; the server needed its peer before it could reach that check. This
confirms the pinned perftest does not silently clamp an explicit excess
request. `-J 47` completed 12,032 operations in both READ (8.029 Mpps)
and WRITE (7.714 Mpps). Raw output and fence checks are in
`experiments/gem5-jetty47-boundary-20261010/`. The J=47 benchmark has only
256 operations per Jetty and is a functionality/boundary test, not a
steady-state throughput calibration. The 47-Jetty run took 157.17 s for
the two cases. All simulator processes were stopped afterward.

The exact physical reserved/public/standard ID split remains unknown. A
passive real-device read of `max_jetty`, `max_jfs` and the reserved Jetty ID
range would verify whether the modeled 1 + 46 partition matches hardware.
This verification cannot establish physical READ/WRITE fidelity from the
single-Jetty physical READ dataset alone. Immediate destroy/recreate of the
same native Jetty ID inside one ns3ub process still lacks an explicit wire
generation discriminator and was not exercised here.
