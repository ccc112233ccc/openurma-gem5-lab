# KVM functional mode

The `kvm` profile accelerates **guest instruction execution** with gem5's
`ArmV8KvmCPU`. It does not replace the modeled UB hardware: UDMA/UMMU, MSI and
GIC delivery, SQ/CQ processing, peer adapters, and the switch stay in gem5 or
the selected switch process. Guest memory runs as `atomic_noncaching`, so KVM
is for bring-up and functional testing, not CPU/cache performance prediction.

## Prerequisites and launch

Use an ARM64 Linux host with readable and writable `/dev/kvm`. A native setup
needs no extra option. A Docker setup must pass the device while creating the
container:

```bash
./setup-native.sh --jobs 8
./run-dual-native.sh --profile kvm

# Or on an ARM64 Linux Docker host:
./setup-docker.sh --kvm --jobs 8
./run-dual.sh --profile kvm
```

The launcher checks `/dev/kvm`, host architecture, and gem5's generated
`USE_KVM=1` / `KVM_ISA="arm"` headers before starting any processes. KVM mode
is restricted to one vCPU because multi-vCPU host event queues have not been
validated with distributed UB synchronization.

The adapter synchronizer is active from tick zero and exclusively drains the
cross-process peer ring, waking the UDMA worker when DATA arrives. Therefore an
empty peer ring no longer creates a second 10-ns (or 1-ms) periodic DMA-worker
event. `OPENURMA_UDMA_POLL_INTERVAL` remains only for guest-level RNR retry and
for transports that do not use the synchronized switch adapter.

## Timer topology

Interpreted gem5 CPUs advertise the architected CP15 timer and hide the
platform MMIO timer. KVM does the reverse: the upstream configuration hides
the CP15 timer (otherwise the guest can observe the host counter), while the
compatibility wrapper retains the MMIO timer. Hiding both causes
`timer_probe: no matching timers found`; the wrapper logs the selected timer
topology for every run.

## Official provider caveat

`--profile kvm` defaults to `--provider udma`, the portable functional path.
An explicit `--provider official` is allowed but marked host-dependent in both
stderr and `run-manifest.txt`. `ArmV8KvmCPU` exposes host CPU ID registers.
OLK's `ummu_sva_supported()` requires the UMMU output-address width and ASID
width to cover the CPU values. The current modeled UMMU advertises a 40-bit
OAS; a wider host therefore causes OLK to clear `UMMU_FEAT_SVA` (bit 22).
Official `udma_alloc_dev_tid()` unconditionally enables KSVA and consequently
gets `-ENODEV` before `udma0` appears. Hardware dirty-bit support is correlated
on the reported host, but it is not the test that clears this bit. This is not
evidence that the modeled UB datapath failed. Compare against:

```bash
./run-dual-native.sh --profile kvm --provider udma
./run-dual-native.sh --profile fast --provider official
```

For a direct one-node diagnosis or boot-time measurement:

```bash
./tools/kvm-boot-test.sh kvm run-kvm-udma
./tools/measure-boot-time.sh kvm 600
./tools/drive-console.py 3456 "urma_admin show"
```
