# gem5 SE support assessment (2026-10-12)

## Current state

The pinned ARM gem5 binary can run a normal Linux userspace AArch64 program
in syscall-emulation (SE) mode. A static AArch64 Hello program was compiled
and run through upstream `gem5/configs/deprecated/example/se.py`; it received
`probe` as `argv[1]`, printed `SE hello: argc=2, first=probe`, and exited
without a simulated nonzero-code message. Its `config.ini` says
`full_system=false`; `stats.txt` records 4,195,000 simulated ticks, 7,012
simulated instructions, 8,391 CPU cycles and 15 emulated syscalls. Source,
binary, console output and gem5 output are under
`experiments/gem5-se-aarch64-smoke-20261012/`. This test uses a deprecated
upstream example script and is a basic gem5 capability check, not a supported
lab command or URMA test.

The lab itself has **no integrated SE launch path for URMA**. `ubsim_lab/cli.py`
registers FS start/start-ready and QEMU commands, but no SE command.
`configs/arm64_fs.py` unconditionally creates `ArmFsLinux`, loads a kernel and
initramfs, sets `Root(full_system=True)`, and adds a platform `UbHostAdapter`
with MMIO, DMA, MSI and a device tree. `scripts/run/run-dual.sh` launches two
of those FS hosts, two standalone UDMA processes, a UB fabric and an OOB
control network.

The official userspace stack cannot simply be given to generic SE. UMDK
discovers devices through `/sys/class/ubcore`, opens `/dev/uburma`, uses the
`URMA_CMD` ioctl ABI for resources, and maps the UDMA doorbell from the
device fd. `urma_perftest` also creates Jettys, registers segments, posts
work, polls JFCs, and uses a peer control channel. Generic SE provides no
running OLK UBCORE/UBURMA/UDMA kernel modules or their MMIO/DMA/interrupt
path. gem5 has an `EmulatedDriver` extension for `/dev` `open`/`ioctl`/`mmap`,
but this repository does not implement a UB variant; this hook alone does not
provide sysfs discovery, memory registration/translation, peer control, or
all official driver semantics.

## What an SE workload would consume and produce

For a **CPU-only** SE experiment today, the input is an AArch64 ELF binary,
its argv/env/stdin and any ordinary input files, plus a gem5 SE machine
configuration (CPU, cache, DRAM, run limit). The output is the program's
stdout/stderr and exit cause, `config.ini/json`, `stats.txt`, and optional
ROI statistics. This can study isolated posting-loop computation but cannot
claim URMA READ latency, bandwidth, queue behavior or UB link utilization.

For a future **two-node URMA SE** experiment, a usable workload contract
would need two AArch64 endpoint processes (initiator and responder), each
with its role/EID, plus operation, size, Jetty count, queue depths, iteration
count, registered memory, warm-up, and the one-port 400-Gbit/s/switch topology.
Its output should include the same perftest READ/WRITE latency and bandwidth
rows, together with gem5 ROI CPU/cache/memory stats, UDMA WQE/DMA/CQE
counters, UB wire/switch counters, and a manifest tying all inputs to the
result. All timing must use coordinated simulated time. This input/output
contract is a design recommendation, **not an implemented CLI**.

There are three practical levels of work:

1. An SE CPU microbenchmark or a transaction-trace driver is relatively
   small, but omits the official user/kernel/device path and does not answer
   end-to-end performance fidelity.
2. A high-level URMA shim could call the standalone UDMA model, but would
   replace rather than preserve much of the official provider/driver work.
3. Running the official `urma_perftest` in SE would require a UB-specific
   emulated device/provider boundary, sysfs and ioctl behavior, mapped
   doorbells, registered-memory translation, completion/notification handling,
   peer setup and multi-process virtual-time synchronization. It is a separate
   implementation project and would need comparison against the existing FS
   path at fixed parameters.

For the current goal of matching physical small READ and large-message
behavior, the existing shell-ready FS checkpoint and detailed O3 ROI retain
the real OLK/UMDK path while avoiding repeated cold boots. SE is most useful
first as a controlled CPU-side diagnostic, not a drop-in replacement for the
current performance result.

References: [gem5 SE/FS guide](https://www.gem5.org/documentation/learning_gem5/part1/simple_config/),
[gem5 statistics guide](https://www.gem5.org/documentation/learning_gem5/part1/gem5_stats/),
`sources/umdk/src/urma/lib/urma/core/urma_device.c`,
`sources/umdk/src/urma/lib/urma/core/urma_cmd_tlv.c`,
`sources/umdk/src/urma/hw/udma/udma_u_db.c`,
`gem5/src/sim/emul_driver.hh`.
