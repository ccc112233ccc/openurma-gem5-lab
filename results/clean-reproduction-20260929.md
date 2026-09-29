# Clean reproduction evidence — 2026-09-29

## Scope

- Fresh clone: `https://github.com/ccc112233ccc/openurma-gem5-lab.git`
- Remote commit tested: `2baecfc1d62eefce6c4a8fc52075044a637d0eea`
- Isolated container: `openurma-gem5-lab-repro`
- Isolated case-sensitive kernel volume: `openurma-gem5-lab-repro-kernel`
- Command: `./lab --runtime docker setup --jobs 2`
- Host: macOS on Apple M2; ARM64 Ubuntu 22.04 build container

## Result

The setup completed with exit code 0 and printed both `[build-all] PASS` and
`[setup] PASS`.  Starting from no generated build artifacts, it fetched and
revision-checked the pinned sources, then built:

- gem5 ARM `gem5.opt`, including the UB-HOST adapter;
- the simulator-neutral UDMA model and standalone UDMA process (3 tests);
- official UMMU and UMDK, including the official UDMA provider and
  `urma_perftest`;
- the pinned OLK 6.6 ARM64 kernel;
- official `ubfi`, `ubus`, `hisi_ubus`, `ummu-core`, `ummu`, `ubase`,
  `ubcore`, `uburma`, and `udma` modules;
- the simulation-only GICv2m interrupt-domain bridge;
- the ARM64 official-UDMA initramfs.

The clone was created at 14:41 and the final initramfs was written at 16:31,
so the cold reproduction took approximately 1 hour 50 minutes on this host.
The resource-constrained kernel/gem5 build completed at `--jobs 2`; the earlier
`--jobs 4` attempt was not used as evidence because it exceeded the Docker VM's
memory during generated ARM decoder compilation.

## Issues exposed by the clean run

The final UBUS/UMMU Kconfig closure triggers a second kernel link.  The
initramfs builder already refreshes the gem5 `vmlinux`, and the resulting
`vmlinux` hash matched the final kernel tree.  However, the published QEMU
`Image`, `kernel.config`, and module directory still described the earlier
pre-fragment kernel:

```text
published Image: fdb5737d276cf1a39f5ecb9b5a8afc1947f2414a4b4edfa3e14bc94720c5ea05
final Image:     089ed9058415e70e7bc9af7aaeb8eac575d61f6f155050207d5ce9519e78e862
published modules: ipv6.ko ubagg.ko ubcore.ko uburma.ko
```

The build now republishes `vmlinux`, `Image`, the final `.config`, IPv6, and
all official hardware modules as one coherent runtime bundle after the final
kernel configuration succeeds.  It also rebuilds IPv6 against that final
kernel.

The first cold-start attempt also exposed that `setup` fetched ns-3-UB but did
not invoke its adapter build, even though the default modular runtime requires
`ns3.44-ub-net-adapter`.  The all-components build now compiles that adapter,
runs both asynchronous and conservative process-contract tests, and checks the
executable as a required final artifact.

A subsequent no-checkpoint startup exposed a separate cold-boot-only issue in
`sync`: it waited for an early `arch_timer` line in `system.terminal`, while
gem5 discards PL011 bytes when no terminal client is attached.  Checkpoint
runs skipped that wait using recorded timer evidence, which had hidden the
problem.  `sync` now waits for the live shell and verifies the retained timer
line directly through guest `dmesg`; the same UART transaction then configures
the OOB interface.  The default cold-boot allowance is 1800 seconds.

## Startup-race regression

An immediate `./lab sync` after `./lab start` previously could fail with
`Connection refused` before gem5 opened its PL011 listener.  UART command
connections now retry within the caller's existing total timeout.  Mock tests
cover delayed-listener success and timeout diagnostics.  A real two-node
restore followed immediately by `./lab sync` completed on both UARTs with
return code 0 and configured both OOB addresses successfully.
