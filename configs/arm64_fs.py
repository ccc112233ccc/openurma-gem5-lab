# SPDX-License-Identifier: Apache-2.0
#
# arm64_fs.py — lab-owned ARM full-system configuration.
#
# The guest-visible UDMA aperture is owned exclusively by UbHostAdapter.
# Device behavior lives in the standalone UDMA process and wire behavior in
# the UB-NET fabric process; this file contains no in-process SystemC NIC.
#                       (interrupt pin raised on CQE arrival)

import argparse
import os
import time

import m5
from m5.objects import *
from m5.util import addToPath
from m5.util.convert import anyToLatency
from m5.util.fdthelper import (
    FdtNode,
    FdtProperty,
    FdtPropertyStrings,
    FdtPropertyWords,
)

_lab_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
_gem5_root = os.environ.get("UBSIM_GEM5_ROOT", os.path.join(_lab_root, "gem5"))
addToPath(os.path.join(_gem5_root, "configs"))
addToPath(os.path.join(_gem5_root, "configs", "example", "arm"))

from common import SysPaths
import devices

IOMEM_BASE = 0x2D000000
OFFICIAL_UDMA_IOMEM_SIZE = 0x1000000
PL011_UART0_BASE = 0x1C090000
VEXPRESS_GEM5_V1_M5OPS_BASE = 0x10010000
OFFICIAL_UDMA_UBRT = 0x2D010000
OFFICIAL_UDMA_UBC_IRQ = 104
OFFICIAL_UDMA_V2M_BASE = 0x2C1C0000
OFFICIAL_UDMA_V2M_SIZE = 0x1000
OFFICIAL_UDMA_V2M_SPI_BASE = 256
OFFICIAL_UDMA_V2M_SPI_COUNT = 64
EXTERNAL_UDMA_MISC_SPI = 104
EXTERNAL_UDMA_AEQ_SPI = 105
EXTERNAL_UDMA_CEQ_SPI = 106


def _omit_mmio_timer_from_dtb(_timer, _state):
    return iter(())


def _official_udma_device_tree(original):
    def generate(system, state):
        root = original(system, state)
        chosen = FdtNode("chosen")
        chosen.append(FdtPropertyWords(
            "linux,ubios-information-table", state.addrCells(OFFICIAL_UDMA_UBRT)
        ))
        root.append(chosen)

        v2m = FdtNode(f"msi-controller@{OFFICIAL_UDMA_V2M_BASE:x}")
        v2m.append(FdtPropertyStrings("compatible", ["arm,gic-v2m-frame"]))
        v2m.append(FdtProperty("msi-controller"))
        v2m.append(FdtPropertyWords(
            "reg", state.addrCells(OFFICIAL_UDMA_V2M_BASE)
            + state.sizeCells(OFFICIAL_UDMA_V2M_SIZE)
        ))
        v2m.append(FdtPropertyWords("arm,msi-base-spi", [OFFICIAL_UDMA_V2M_SPI_BASE]))
        v2m.append(FdtPropertyWords("arm,msi-num-spis", [OFFICIAL_UDMA_V2M_SPI_COUNT]))
        v2m.appendPhandle(system.realview.gicv2m)
        root.append(v2m)

        ubc = FdtNode("ubc@0")
        ubc.append(FdtPropertyStrings("compatible", ["ub,ubc"]))
        ubc.append(FdtPropertyWords("index", [0]))
        ubc.append(FdtPropertyWords("msi-parent", [state.phandle(system.realview.gicv2m)]))
        ubc.append(FdtPropertyWords("interrupts", [0, OFFICIAL_UDMA_UBC_IRQ - 32, 4]))
        root.append(ubc)

        ummu = FdtNode("ummu@0")
        ummu.append(FdtPropertyStrings("compatible", ["ub,ummu"]))
        ummu.append(FdtPropertyWords("index", [0]))
        ummu.append(FdtPropertyWords("msi-parent", [state.phandle(system.realview.gicv2m)]))
        root.append(ummu)
        return root

    return generate


def positive_int(value):
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def percentage(value):
    parsed = int(value)
    if parsed < 0 or parsed > 100:
        raise argparse.ArgumentTypeError("must be between 0 and 100")
    return parsed


def aligned_physical_address(value):
    try:
        parsed = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer address") from error
    if parsed <= 0 or parsed > (1 << 64) - 0x10000:
        raise argparse.ArgumentTypeError("must name a non-zero 64-bit range")
    if parsed & 0xffff:
        raise argparse.ArgumentTypeError("must be aligned to the 64-KiB m5ops range")
    return parsed


def cache_latency_triplet(value):
    """Parse TAG,DATA,RESPONSE cache latencies expressed in CPU cycles."""
    try:
        parsed = tuple(int(field) for field in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "must contain three positive integers: TAG,DATA,RESPONSE"
        ) from error
    if len(parsed) != 3 or any(field <= 0 for field in parsed):
        raise argparse.ArgumentTypeError(
            "must contain three positive integers: TAG,DATA,RESPONSE"
        )
    return parsed


def configure_cache(cache, size, assoc, latencies, mshrs, targets_per_mshr,
                    write_buffers, clusivity):
    cache.size = size
    cache.assoc = assoc
    cache.tag_latency, cache.data_latency, cache.response_latency = latencies
    cache.mshrs = mshrs
    cache.tgts_per_mshr = targets_per_mshr
    cache.write_buffers = write_buffers
    cache.clusivity = clusivity
    cache.demand_mshr_reserve = 1
    cache.sequential_access = False
    cache.write_allocator = NULL
    # Keep the baseline deterministic and make the inherited ARM example
    # policy explicit.  Prefetch sensitivity is a separate experiment rather
    # than an undocumented source of traffic in the default server profile.
    cache.prefetcher = NULL
    cache.replacement_policy = LRURP()


def configure_o3(cpu, args):
    """Apply the explicitly recorded generic O3 microarchitecture contract."""
    for name in (
        "fetchWidth", "decodeWidth", "renameWidth", "dispatchWidth",
        "issueWidth", "wbWidth", "commitWidth", "squashWidth",
    ):
        setattr(cpu, name, args.o3_width)
    cpu.numROBEntries = args.o3_rob_entries
    cpu.numIQEntries = args.o3_iq_entries
    cpu.LQEntries = args.o3_lq_entries
    cpu.SQEntries = args.o3_sq_entries
    cpu.cacheLoadPorts = args.o3_load_ports
    cpu.cacheStorePorts = args.o3_store_ports
    cpu.fetchBufferSize = args.o3_fetch_buffer_bytes
    cpu.fetchQueueSize = args.o3_fetch_queue_entries
    cpu.numPhysIntRegs = args.o3_phys_int_regs
    cpu.numPhysFloatRegs = args.o3_phys_float_regs
    cpu.numPhysVecRegs = args.o3_phys_vec_regs
    cpu.numPhysVecPredRegs = args.o3_phys_vec_pred_regs
    cpu.numPhysMatRegs = args.o3_phys_mat_regs
    # Preserve ArmO3CPU's stock 5x condition-code provision so changing the
    # integer rename file cannot accidentally create a CC bottleneck.
    cpu.numPhysCCRegs = args.o3_phys_int_regs * 5
    cpu.fuPool = DefaultFUPool()
    cpu.branchPred = TournamentBP(numThreads=1)


def configure_coherent_xbar(bus, args, system_bus=False):
    if system_bus:
        bus.frontend_latency = args.memory_bus_frontend_latency
        bus.forward_latency = args.memory_bus_forward_latency
        bus.response_latency = args.memory_bus_response_latency
        bus.snoop_response_latency = args.memory_bus_snoop_response_latency
        bus.header_latency = args.memory_bus_header_latency
    else:
        bus.frontend_latency = args.coherent_bus_frontend_latency
        bus.forward_latency = args.coherent_bus_forward_latency
        bus.response_latency = args.coherent_bus_response_latency
        bus.snoop_response_latency = args.coherent_bus_snoop_response_latency
        bus.header_latency = args.coherent_bus_header_latency
    bus.width = args.fabric_width_bytes


def create(args):
    atomic_modes = ("atomic", "atomic_hot", "atomic_fast", "atomic_cache")
    use_timing  = (args.cpu in ("timing", "timing_nocache", "timing_full"))
    kvm_boot = args.cpu in ("kvm", "kvm_server_o3")
    server_switch = args.cpu in ("server_o3", "kvm_server_o3")
    use_caches  = (args.cpu in (
        "atomic_cache", "timing", "timing_full", "o3", "server_o3",
        "kvm", "kvm_server_o3"
    ))
    if kvm_boot:
        mem_mode = "atomic_noncaching"
    else:
        mem_mode = "atomic" if args.cpu in atomic_modes or server_switch else "timing"
    if args.cpu == "o3":
        try:
            cpu_cls = O3CPU  # gem5 ARM ISA's default O3 alias
        except NameError:
            cpu_cls = ArmO3CPU
    elif kvm_boot:
        # Pure kvm stays here for functional validation; kvm_server_o3 uses
        # this CPU only for boot/idle and switches to ArmO3 for its ROI.
        cpu_cls = ArmV8KvmCPU
    elif server_switch:
        # Boot quickly with architecturally equivalent CPUs, then use the
        # guest-triggered switchcpu pseudo operation after resource setup and
        # immediately before benchmark warm-up.  The measured loop runs
        # entirely on ArmO3.
        cpu_cls = AtomicSimpleCPU
    elif use_timing:
        cpu_cls = TimingSimpleCPU
    else:
        cpu_cls = AtomicSimpleCPU
    system = devices.SimpleSystem(
        use_caches, args.mem_size, mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
        readfile=args.script,
    )
    if kvm_boot:
        system.kvm_vm = KvmVM()
        system.release = ArmDefaultRelease.for_kvm()
        # Nested KVM under Apple's Virtualization.framework exposes VM/vCPU
        # acceleration but not KVM_CREATE_DEVICE for an ARM VGICv2.  Keep
        # interrupt delivery in gem5 and use the platform MMIO timer for both
        # KVM modes, matching gem5's userspace-GIC regression configuration.
        system.realview.gic.simulate_gic = True
        GenericTimer.generateDeviceTree = SimObject.generateDeviceTree
    system.cache_line_size = args.cache_line_size
    system.fabric_voltage_domain = VoltageDomain(voltage="1.0V")
    system.fabric_clk_domain = SrcClockDomain(
        clock=args.fabric_freq,
        voltage_domain=system.fabric_voltage_domain,
    )
    system.membus.clk_domain = system.fabric_clk_domain
    configure_coherent_xbar(system.membus, args, system_bus=True)
    system.membus.snoop_filter.max_capacity = \
        args.membus_snoop_filter_capacity
    # DMA reaches IOCache through this shared non-coherent front-side bus.
    # Tie its bandwidth and pipeline explicitly to the modeled fabric instead
    # of silently inheriting IOXBar's 1 GHz / 16-byte class defaults.
    system.iobus.clk_domain = system.fabric_clk_domain
    system.iobus.width = args.fabric_width_bytes
    system.iobus.frontend_latency = args.io_bus_frontend_latency
    system.iobus.forward_latency = args.io_bus_forward_latency
    system.iobus.response_latency = args.io_bus_response_latency
    system.iobus.header_latency = args.io_bus_header_latency
    if use_caches:
        # The stock ARM IOCache is the coherence-protocol adapter for
        # non-caching DMA requestors.  Keep its cost explicit: direct device
        # WriteReq packets are not valid snoops when injected below it at the
        # coherent membus, while routing through this cache produces the
        # ownership/invalidation traffic expected by the cache hierarchy.
        configure_cache(
            system.iocache, args.io_cache_size, args.io_cache_assoc,
            args.io_cache_latency, args.io_cache_mshrs,
            args.io_cache_targets_per_mshr, args.io_cache_write_buffers,
            "mostly_incl",
        )
        system.iocache.clk_domain = system.fabric_clk_domain
    from common import MemConfig
    MemConfig.config_mem(args, system)
    # DDR timing, bank geometry, and device width come from --mem-type.  The
    # controller policy and finite queues below are independent performance
    # inputs, so record them explicitly instead of inheriting library defaults.
    for ctrl in system.mem_ctrls:
        ctrl.clk_domain = system.fabric_clk_domain
        ctrl.dram.clk_domain = system.fabric_clk_domain
        ctrl.mem_sched_policy = args.mem_sched_policy
        ctrl.write_high_thresh_perc = args.mem_write_high_thresh
        ctrl.write_low_thresh_perc = args.mem_write_low_thresh
        ctrl.min_writes_per_switch = args.mem_min_writes_per_switch
        ctrl.min_reads_per_switch = args.mem_min_reads_per_switch
        ctrl.static_frontend_latency = args.mem_ctrl_frontend_latency
        ctrl.static_backend_latency = args.mem_ctrl_backend_latency
        ctrl.command_window = args.mem_ctrl_command_window
        ctrl.dram.read_buffer_size = args.mem_read_buffer_size
        ctrl.dram.write_buffer_size = args.mem_write_buffer_size
        ctrl.dram.page_policy = args.mem_page_policy
        ctrl.dram.max_accesses_per_row = args.mem_max_accesses_per_row
        row_buffer_bytes = (ctrl.dram.device_rowbuffer_size.value *
                            ctrl.dram.devices_per_rank.value)
        mapping = ctrl.dram.addr_mapping.value
        if mapping == "RoRaBaChCo":
            if args.mem_channels_intlv != row_buffer_bytes:
                raise ValueError("RoRaBaChCo requires --mem-channels-intlv "
                                 "to equal the full rank row-buffer size")
        elif args.mem_channels_intlv > row_buffer_bytes:
            raise ValueError("--mem-channels-intlv exceeds the DRAM rank "
                             "row-buffer size")
        if ctrl.command_window.value < ctrl.dram.tCK.value:
            raise ValueError("--mem-ctrl-command-window must be at least one "
                             "DRAM tCK")

    # Optional Ethernet side channel for interactive two-node runs. UB payload
    # traffic is external to this machine; stock tools use this interface only
    # for their TCP control plane. Keep that control channel on
    # the host relay so setup can run freely; a separate packet-idle
    # DistEtherLink carries the conservative global barrier used during the
    # measured UB ping-pong interval.
    if getattr(args, "eth_tap_socket", ""):
        system.ethernet = IGbE_e1000(hardware_address=args.eth_mac)
        system.attach_pci(system.ethernet)
        system.ethertap = EtherTapStub(port=args.eth_tap_socket)
        system.etherlink = EtherLink(
            speed=args.eth_link_speed, delay=args.eth_link_delay)
        system.etherlink.int0 = system.ethernet.interface
        system.etherlink.int1 = system.ethertap.tap

    if getattr(args, "dist_size", 0) > 0:
        if args.dist_size < 2:
            raise ValueError("--dist-size must be at least 2")
        system.dist_sync_stub = EtherSwitch()
        system.dist_sync_link = DistEtherLink(
            speed=args.eth_link_speed,
            delay=args.eth_link_delay,
            dist_rank=args.dist_rank,
            dist_size=args.dist_size,
            server_name=args.dist_server_name,
            server_port=args.dist_server_port,
            sync_start=args.dist_sync_start,
            sync_repeat=args.dist_sync_repeat,
            dist_sync_on_pseudo_op=args.dist_sync_on_pseudo_op,
        )
        system.dist_sync_link.int0 = system.dist_sync_stub.interface[0]

    system.connect()
    # One one-core cluster per CPU gives every core a private L2.  All clusters
    # feed the same optional L3 and memory fabric, and all cores are reported as
    # one socket.  This is a reduced-core server slice, not a claim that the
    # modeled server has only four physical cores.
    cpu_clusters = []
    for _ in range(args.num_cpus):
        cluster = devices.ArmCpuCluster(
            system, 1, args.cpu_freq, "1.0V", cpu_cls,
            devices.L1I if use_caches else None,
            devices.L1D if use_caches else None,
            devices.L2 if use_caches else None,
            tarmac_gen=False, tarmac_dest=None,
        )
        cluster.cpus[0].socket_id = 0
        if kvm_boot:
            # Perf access is commonly restricted inside otherwise valid KVM
            # containers and is unnecessary for fast-forward correctness.
            cluster.cpus[0].usePerf = False
            cluster.cpus[0].usePerfOverflow = False
        cpu_clusters.append(cluster)
    # Assign the non-empty SimObject vector in one operation.  An empty list
    # cannot establish a dynamic SimObject child parameter on SimpleSystem.
    system.cpu_cluster = cpu_clusters
    # gem5's ARM release advertises FEAT_HCX (in id_aa64mmfr1_el1, HCX bits
    # 40-43) but does NOT implement the HCRX_EL2 system register. Newer kernels
    # (e.g. openEuler OLK-6.6) write HCRX_EL2 in init_el2 *before* the exception
    # vectors (VBAR_EL1) are installed; the unimplemented msr traps to the
    # not-yet-set vector base (0) and spins forever at vector offset 0x200.
    # Drop FEAT_HCX from the ARM release so the kernel's feature check skips the
    # write. (5.10 never touches HCRX_EL2, so this is a no-op for it.)
    try:
        system.release.extensions = [
            e for e in system.release.extensions if "FEAT_HCX" not in str(e)
        ]
    except Exception:
        pass
    system.addCaches(use_caches, last_cache_level=args.last_cache_level)
    if use_caches:
        for cluster in system.cpu_cluster:
            if args.last_cache_level >= 2:
                configure_coherent_xbar(cluster.toL2Bus, args)
                cluster.toL2Bus.snoop_filter.max_capacity = \
                    args.core_bus_snoop_filter_capacity
            cpu = cluster.cpus[0]
            configure_cache(
                cpu.icache, args.l1i_size, args.l1i_assoc,
                args.l1i_latency, args.l1i_mshrs,
                args.l1i_targets_per_mshr, args.l1i_write_buffers,
                "mostly_incl",
            )
            configure_cache(
                cpu.dcache, args.l1d_size, args.l1d_assoc,
                args.l1d_latency, args.l1d_mshrs,
                args.l1d_targets_per_mshr, args.l1d_write_buffers,
                "mostly_incl",
            )
            if args.last_cache_level >= 2:
                configure_cache(
                    cluster.l2, args.l2_size, args.l2_assoc,
                    args.l2_latency, args.l2_mshrs,
                    args.l2_targets_per_mshr, args.l2_write_buffers,
                    "mostly_excl",
                )
            if args.cpu == "o3":
                configure_o3(cpu, args)
        if args.last_cache_level >= 3:
            configure_cache(
                system.l3, args.l3_size, args.l3_assoc,
                args.l3_latency, args.l3_mshrs,
                args.l3_targets_per_mshr, args.l3_write_buffers,
                "mostly_excl",
            )
            # The shared LLC belongs to the uncore/fabric rather than to the
            # first core cluster.  ArmSystem initially inherits cluster 0's
            # clock here, so make the server-profile clock contract explicit.
            system.l3.clk_domain = system.fabric_clk_domain
            system.toL3Bus.clk_domain = system.fabric_clk_domain
            configure_coherent_xbar(system.toL3Bus, args)
            system.toL3Bus.snoop_filter.max_capacity = \
                args.l3_bus_snoop_filter_capacity

    if server_switch:
        try:
            switch_cls = O3CPU
        except NameError:
            switch_cls = ArmO3CPU
        switch_cpus = []
        switch_pairs = []
        for cluster in system.cpu_cluster:
            boot_cpu = cluster.cpus[0]
            switch_cpu = switch_cls(
                switched_out=True,
                cpu_id=boot_cpu.cpu_id,
                socket_id=0,
            )
            switch_cpu.clk_domain = boot_cpu.clk_domain
            switch_cpu.createThreads()
            configure_o3(switch_cpu, args)
            switch_cpus.append(switch_cpu)
            switch_pairs.append((boot_cpu, switch_cpu))
        system.switch_cpus = switch_cpus
        system._switch_pairs = switch_pairs
    # AtomicSimpleCPU defaults to ignoring memory-system latency. Keep data
    # stalls enabled in every atomic mode so memory hierarchy delays are folded
    # into simulated time. The ``atomic`` mode also retains instruction stalls.
    # ``atomic_hot`` disables only instruction stalls to approximate a hot L1I
    # as a diagnostic A/B point. ``atomic_fast`` disables both stall classes
    # and caches for the shortest functional-validation cold boot; it must not
    # be used for latency measurements. ``atomic_cache`` routes both instruction
    # and data traffic through the standard L1I/L1D/L2 hierarchy and retains
    # both stall types. Timing/O3 CPUs consume memory timing natively.
    if args.cpu in atomic_modes or args.cpu == "server_o3":
        for cluster in system.cpu_cluster:
            for cpu in cluster.cpus:
                # server_o3 is only a functional fast-forward phase.  Charging
                # AtomicCPU stalls there would slow boot without affecting the
                # O3 ROI, so only the explicitly atomic modes consume them.
                cpu.simulate_data_stalls = (
                    not server_switch and args.cpu != "atomic_fast"
                )
                cpu.simulate_inst_stalls = (
                    not server_switch and
                    args.cpu not in ("atomic_hot", "atomic_fast")
                )

    host_socket = os.environ.get("UBSIM_UDMA_HOST_SOCKET", "")
    if not host_socket:
        raise RuntimeError(
            "UBSIM_UDMA_HOST_SOCKET is required; UDMA runs as an external process"
        )
    interrupt_pins = []
    for spi in (EXTERNAL_UDMA_MISC_SPI, EXTERNAL_UDMA_AEQ_SPI,
                EXTERNAL_UDMA_CEQ_SPI):
        pin = ArmSPI(num=spi)
        pin.platform = system.realview
        interrupt_pins.append(pin)
    system.external_udma = UbHostAdapter(
        pio_addr=IOMEM_BASE,
        pio_size=OFFICIAL_UDMA_IOMEM_SIZE,
        pio_latency=os.environ.get("UBSIM_UDMA_HOST_PIO_LATENCY", "100ns"),
        socket_path=host_socket,
        poll_interval=os.environ.get("UBSIM_UDMA_HOST_POLL_INTERVAL", "1us"),
        sync=os.environ.get("UBSIM_UDMA_HOST_SYNC", "0") == "1",
        lifecycle_sync=os.environ.get(
            "UBSIM_UDMA_HOST_LIFECYCLE_SYNC", "0") == "1",
        link_latency=os.environ.get("UBSIM_UDMA_HOST_LINK_LATENCY", "50ns"),
        sync_interval=os.environ.get("UBSIM_UDMA_HOST_SYNC_INTERVAL", "50ns"),
        interrupt_misc=interrupt_pins[0],
        interrupt_aeq=interrupt_pins[1],
        interrupt_ceq=interrupt_pins[2],
    )
    system.external_udma.pio = system.membus.mem_side_ports
    system.external_udma.dma = system.iobus.cpu_side_ports
    system.external_udma.msi = system.membus.cpu_side_ports

    # ARM platform plumbing.
    # Assign deterministic, non-overlapping listener ports.  VExpress_GEM5_V1
    # contains four PL011s; uart0 inherits system.terminal while uart1..3 own
    # separate Terminal objects.  Pinning all four prevents their default 3456
    # listeners from stealing a peer node's console port.
    system.terminal.port = args.terminal_port
    for idx, uart in enumerate(system.realview.uart[1:], start=1):
        uart.device.port = args.terminal_port + idx
    system.realview.setupBootLoader(system, SysPaths.binary)
    # setupBootLoader supplies the platform default; assign the explicit CLI
    # contract afterwards so the guest and simulator use exactly one address.
    system.m5ops_base = args.m5ops_base
    system.workload.dtb_filename = os.path.join(m5.options.outdir,
                                                "system.dtb")
    original_timer_dtb = GenericTimerMem.generateDeviceTree
    original_system_dtb = ArmSystem.generateDeviceTree
    if not kvm_boot:
        GenericTimerMem.generateDeviceTree = _omit_mmio_timer_from_dtb
    ArmSystem.generateDeviceTree = _official_udma_device_tree(
        original_system_dtb)
    try:
        system.generateDtb(system.workload.dtb_filename)
    finally:
        GenericTimerMem.generateDeviceTree = original_timer_dtb
        ArmSystem.generateDeviceTree = original_system_dtb
    if args.initrd:
        system.workload.initrd_filename = args.initrd
    cmdline_extras = []
    if args.cpu in ("timing", "timing_nocache", "o3", "server_o3",
                    "kvm", "kvm_server_o3"):
        # Tell tiny_init to use the reduced-N "fast" mode so urma_smoke
        # finishes within wall-clock budget under TimingSimpleCPU +
        # caches (~100x slower than AtomicSimpleCPU) and O3CPU (slower
        # still). "timing_full" deliberately omits this for a complete
        # sweep — expect multi-hour wall-clock.
        cmdline_extras.append("urma_fast")
    if server_switch:
        cmdline_extras.append("ubsim_cpu_switch=server_o3")
    if kvm_boot:
        # The KVM modes are intentionally validated as one-vCPU systems.  A
        # multi-vCPU ARM KVM run needs per-vCPU host event queues, whose PDES
        # quantum conflicts with this experiment's 100-ns distributed-link
        # synchronization and has not been proven safe with the SystemC NIC.
        cmdline_extras.extend([
            "maxcpus=1",
            "ubsim_m5ops_mode=addr",
            f"ubsim_m5ops_base=0x{args.m5ops_base:x}",
            f"earlycon=pl011,mmio32,0x{PL011_UART0_BASE:x}",
            "keep_bootcon",
            "ignore_loglevel",
            "loglevel=8",
            "nokaslr",
        ])
    if args.benchmark_cpu >= 0:
        cmdline_extras.append(f"ubsim_bench_cpu={args.benchmark_cpu}")
    if getattr(args, "extras", False):
        cmdline_extras.append("urma_extras")
    if getattr(args, "extra_cmdline", None):
        cmdline_extras.append(args.extra_cmdline)
    # Disable glibc's rseq init for init and all its children.
    # Kernel 4.14 returns -ENOSYS for syscall 293 (rseq); glibc 2.35+
    # then executes `brk #0xf` which triggers a 30-line kernel
    # register-dump printk per process. Under TimingCPU each printk
    # serial char costs many cycles, so this single trap adds
    # ~30 minutes to wall-clock boot time. Linux init passes any
    # cmdline token containing "=" to the init process's envp, so
    # this single cmdline token disables rseq for init + every
    # descendant via inherited environment.
    cmdline_extras.append("GLIBC_TUNABLES=glibc.pthread.rseq=0")
    system.workload.command_line = " ".join([
        "console=ttyAMA0", f"lpj={args.loops_per_jiffy}", "norandmaps",
        f"root={args.root_device}", "rw",
        f"mem={args.guest_mem_limit or args.mem_size}",
    ] + cmdline_extras)
    return system


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="vmlinux")
    parser.add_argument("--initrd", default=None)
    parser.add_argument("--root-device", default="/dev/ram")
    parser.add_argument("--mem-size", default="2GB")
    parser.add_argument("--guest-mem-limit", default=None,
        help="optional Linux mem= limit; the modeled DRAM capacity remains "
             "--mem-size")
    parser.add_argument("--cpu", default="atomic",
        choices=["atomic", "atomic_hot", "atomic_fast", "atomic_cache",
                 "timing", "timing_nocache",
                 "timing_full", "o3", "server_o3", "kvm",
                 "kvm_server_o3"],
        help="atomic: AtomicSimpleCPU without caches, with instruction "
             "and data stalls; "
             "atomic_hot: AtomicSimpleCPU without caches, data stalls only "
             "(hot-L1I diagnostic); "
             "atomic_fast: AtomicSimpleCPU without caches or memory stalls "
             "for functional validation only; "
             "atomic_cache: AtomicSimpleCPU + L1I/L1D/L2, with instruction "
             "and data stalls; "
             "timing: TimingSimpleCPU + L1/L2 + DDR (most realistic, "
             "~100× slower); "
             "timing_nocache: TimingSimpleCPU on a bare membus (mid); "
             "timing_full: timing caches with the full workload; "
             "o3: O3CPU for boot and workload; "
             "server_o3: AtomicSimpleCPU boot, then guest-triggered O3 ROI; "
             "kvm: one online ArmV8KvmCPU throughout for fast functional "
             "validation; kvm_server_o3: KVM boot/idle, then O3 ROI")
    parser.add_argument("--kvm-max-run-slice", default="1ms",
        help="maximum simulated time for one KVM fast-forward slice; this "
             "periodically returns control to gem5 when the device event "
             "queue otherwise has no finite near-term deadline")
    parser.add_argument("--m5ops-base", type=aligned_physical_address,
        default=VEXPRESS_GEM5_V1_M5OPS_BASE,
        help="physical base of the VExpress_GEM5_V1 64-KiB gem5 pseudo-op "
             "MMIO range")
    parser.add_argument("--cpu-freq", default="3GHz")
    parser.add_argument("--loops-per-jiffy", type=positive_int,
        default=19988480,
        help="Linux lpj= calibration override; scale this with --cpu-freq "
             "for functional low-frequency boot, while keeping the server "
             "profile value for performance experiments")
    parser.add_argument("--num-cpus", type=positive_int, default=1,
        help="visible CPU cores; cache-enabled configurations use private L2s")
    parser.add_argument("--benchmark-cpu", type=int, default=-1,
        help="CPU recorded in the guest environment for benchmark affinity; "
             "-1 disables affinity")
    parser.add_argument("--o3-width", type=positive_int, default=8,
        help="generic O3 front/back-end width")
    parser.add_argument("--o3-rob-entries", type=positive_int, default=192)
    parser.add_argument("--o3-iq-entries", type=positive_int, default=64)
    parser.add_argument("--o3-lq-entries", type=positive_int, default=32)
    parser.add_argument("--o3-sq-entries", type=positive_int, default=32)
    parser.add_argument("--o3-load-ports", type=positive_int, default=4,
        help="maximum cache loads accepted per O3 cycle")
    parser.add_argument("--o3-store-ports", type=positive_int, default=2,
        help="maximum cache stores accepted per O3 cycle")
    parser.add_argument("--o3-fetch-buffer-bytes", type=positive_int,
        default=64)
    parser.add_argument("--o3-fetch-queue-entries", type=positive_int,
        default=32)
    parser.add_argument("--o3-phys-int-regs", type=positive_int, default=256)
    parser.add_argument("--o3-phys-float-regs", type=positive_int,
        default=256)
    parser.add_argument("--o3-phys-vec-regs", type=positive_int, default=256)
    parser.add_argument("--o3-phys-vec-pred-regs", type=positive_int,
        default=32)
    parser.add_argument("--o3-phys-mat-regs", type=positive_int, default=2)
    parser.add_argument("--last-cache-level", type=int, choices=[1, 2, 3],
        default=2)
    parser.add_argument("--cache-line-size", type=positive_int, default=64)
    parser.add_argument("--l1i-size", default="48kB")
    parser.add_argument("--l1i-assoc", type=positive_int, default=3)
    parser.add_argument("--l1i-latency", type=cache_latency_triplet,
        default=(1, 1, 1), metavar="TAG,DATA,RESPONSE")
    parser.add_argument("--l1i-mshrs", type=positive_int, default=8)
    parser.add_argument("--l1i-targets-per-mshr", type=positive_int,
        default=8)
    parser.add_argument("--l1i-write-buffers", type=positive_int, default=8)
    parser.add_argument("--l1d-size", default="32kB")
    parser.add_argument("--l1d-assoc", type=positive_int, default=2)
    parser.add_argument("--l1d-latency", type=cache_latency_triplet,
        default=(2, 2, 1), metavar="TAG,DATA,RESPONSE")
    parser.add_argument("--l1d-mshrs", type=positive_int, default=16)
    parser.add_argument("--l1d-targets-per-mshr", type=positive_int,
        default=16)
    parser.add_argument("--l1d-write-buffers", type=positive_int, default=16)
    parser.add_argument("--l2-size", default="1MB")
    parser.add_argument("--l2-assoc", type=positive_int, default=16)
    parser.add_argument("--l2-latency", type=cache_latency_triplet,
        default=(12, 12, 5), metavar="TAG,DATA,RESPONSE")
    parser.add_argument("--l2-mshrs", type=positive_int, default=32)
    parser.add_argument("--l2-targets-per-mshr", type=positive_int,
        default=16)
    parser.add_argument("--l2-write-buffers", type=positive_int, default=16)
    parser.add_argument("--l3-size", default="16MB")
    parser.add_argument("--l3-assoc", type=positive_int, default=16)
    parser.add_argument("--l3-latency", type=cache_latency_triplet,
        default=(20, 20, 20), metavar="TAG,DATA,RESPONSE")
    parser.add_argument("--l3-mshrs", type=positive_int, default=64)
    parser.add_argument("--l3-targets-per-mshr", type=positive_int,
        default=16)
    parser.add_argument("--l3-write-buffers", type=positive_int, default=32)
    parser.add_argument("--fabric-freq", default="1GHz")
    parser.add_argument("--fabric-width-bytes", type=positive_int, default=16)
    parser.add_argument("--coherent-bus-frontend-latency", type=int,
        default=1, help="coherent-crossbar arbitration latency in cycles")
    parser.add_argument("--coherent-bus-forward-latency", type=int,
        default=0, help="coherent-crossbar request forwarding latency")
    parser.add_argument("--coherent-bus-response-latency", type=int,
        default=1, help="coherent-crossbar response latency")
    parser.add_argument("--coherent-bus-snoop-response-latency", type=int,
        default=1, help="coherent-crossbar snoop-response latency")
    parser.add_argument("--coherent-bus-header-latency", type=int,
        default=1, help="coherent-crossbar header occupancy")
    parser.add_argument("--memory-bus-frontend-latency", type=int, default=3)
    parser.add_argument("--memory-bus-forward-latency", type=int, default=4)
    parser.add_argument("--memory-bus-response-latency", type=int, default=2)
    parser.add_argument("--memory-bus-snoop-response-latency", type=int,
        default=4)
    parser.add_argument("--memory-bus-header-latency", type=int, default=1)
    parser.add_argument("--core-bus-snoop-filter-capacity", default="2MiB")
    parser.add_argument("--l3-bus-snoop-filter-capacity", default="8MiB")
    parser.add_argument("--membus-snoop-filter-capacity", default="64MiB",
        help="maximum cache footprint tracked by the system-bus snoop filter")
    parser.add_argument("--io-bus-frontend-latency", type=int, default=2,
        help="I/O request arbitration latency in fabric cycles")
    parser.add_argument("--io-bus-forward-latency", type=int, default=1,
        help="I/O request forwarding latency in fabric cycles")
    parser.add_argument("--io-bus-response-latency", type=int, default=2,
        help="I/O response latency in fabric cycles")
    parser.add_argument("--io-bus-header-latency", type=int, default=1,
        help="I/O bus occupancy per packet header in fabric cycles")
    parser.add_argument("--io-cache-size", default="1kB",
        help="capacity of the coherent I/O protocol adapter")
    parser.add_argument("--io-cache-assoc", type=positive_int, default=8)
    parser.add_argument("--io-cache-latency", type=cache_latency_triplet,
        default=(1, 1, 1), metavar="TAG,DATA,RESPONSE",
        help="I/O coherence-adapter latency in fabric cycles")
    parser.add_argument("--io-cache-mshrs", type=positive_int, default=32)
    parser.add_argument("--io-cache-targets-per-mshr", type=positive_int,
        default=32)
    parser.add_argument("--io-cache-write-buffers", type=positive_int,
        default=32)
    parser.add_argument("--script", default=None)
    parser.add_argument("--mem-type", default="DDR3_1600_8x8")
    parser.add_argument("--mem-channels", type=int, default=1)
    parser.add_argument("--mem-channels-intlv", type=positive_int, default=128,
        help="physical-address interleave granularity across memory channels")
    parser.add_argument("--mem-addr-mapping",
        choices=["RoRaBaChCo", "RoRaBaCoCh", "RoCoRaBaCh"],
        default="RoRaBaCoCh",
        help="DRAM address decoding order, applied before channel ranges are built")
    parser.add_argument("--mem-ranks", type=int, default=None)
    parser.add_argument("--mem-read-buffer-size", type=positive_int,
        default=32, help="read-burst queue entries per memory channel")
    parser.add_argument("--mem-write-buffer-size", type=positive_int,
        default=64, help="write-burst queue entries per memory channel")
    parser.add_argument("--mem-page-policy",
        choices=["open", "close", "open_adaptive", "close_adaptive"],
        default="open_adaptive")
    parser.add_argument("--mem-max-accesses-per-row", type=positive_int,
        default=16)
    parser.add_argument("--mem-sched-policy", choices=["fcfs", "frfcfs"],
        default="frfcfs")
    parser.add_argument("--mem-write-high-thresh", type=percentage, default=85)
    parser.add_argument("--mem-write-low-thresh", type=percentage, default=50)
    parser.add_argument("--mem-min-writes-per-switch", type=positive_int,
        default=16)
    parser.add_argument("--mem-min-reads-per-switch", type=positive_int,
        default=16)
    parser.add_argument("--mem-ctrl-frontend-latency", default="10ns")
    parser.add_argument("--mem-ctrl-backend-latency", default="10ns")
    parser.add_argument("--mem-ctrl-command-window", default="10ns")
    parser.add_argument("--external-memory-system", default=None)
    parser.add_argument("--xor-low-bit", type=int, default=0)
    parser.add_argument("--extra-cmdline", default=None,
        help="appended verbatim to the kernel cmdline (e.g. urma_tenants=128)")
    parser.add_argument("--terminal-port", type=int, default=3456,
        help="TCP port for ttyAMA0 (uart1..3 use the next three ports)")
    parser.add_argument("--eth-tap-socket", default="",
        help="EtherTapStub Unix socket for a guest Ethernet OOB link")
    parser.add_argument("--eth-mac", default="02:00:00:00:00:01",
        help="MAC address for the optional e1000 device")
    parser.add_argument("--eth-link-speed", default="10Gbps",
        help="speed of the optional local Ethernet link")
    parser.add_argument("--eth-link-delay", default="1us",
        help="delay of the optional local Ethernet link")
    parser.add_argument("--dist-rank", type=int, default=0,
        help="This gem5 process rank in a synchronized distributed run")
    parser.add_argument("--dist-size", type=int, default=0,
        help="Number of node gem5 processes; 0 disables DistEtherLink")
    parser.add_argument("--dist-server-name", default="127.0.0.1",
        help="Host running the dist-gem5 switch")
    parser.add_argument("--dist-server-port", type=int, default=2200,
        help="TCP port of the dist-gem5 switch")
    parser.add_argument("--dist-sync-start", default="5200000000000t",
        help="Initial distributed synchronization tick")
    parser.add_argument("--dist-sync-repeat", default="50ns",
        help="Conservative synchronization quantum; must be no greater than "
             "the minimum cross-process link latency")
    parser.add_argument("--dist-sync-on-pseudo-op", action="store_true",
        help="Boot independently and enable synchronization collectively with "
             "the m5 dist-toggle-sync pseudo operation")
    parser.add_argument("--restore-from", default=None,
        help="restore from a checkpoint directory (skips boot)")
    args = parser.parse_args()

    if args.benchmark_cpu >= args.num_cpus:
        parser.error("--benchmark-cpu must be smaller than --num-cpus")
    if args.m5ops_base != VEXPRESS_GEM5_V1_M5OPS_BASE:
        parser.error("VExpress_GEM5_V1 reserves its m5ops MMIO range at "
                     "0x10010000; another address requires another platform "
                     "memory-map contract")
    if args.cpu in ("kvm", "kvm_server_o3") and \
            args.benchmark_cpu not in (-1, 0):
        parser.error("KVM fast-forward currently brings only CPU0 online; "
                     "use --benchmark-cpu=0")
    if args.cpu in ("kvm", "kvm_server_o3") and args.num_cpus != 1:
        parser.error("KVM modes currently require --num-cpus=1")
    if args.cache_line_size & (args.cache_line_size - 1):
        parser.error("--cache-line-size must be a power of two")
    if args.o3_width > 12:
        parser.error("--o3-width exceeds this gem5 build's MaxWidth=12")
    if args.o3_fetch_buffer_bytes > args.cache_line_size or \
            args.cache_line_size % args.o3_fetch_buffer_bytes:
        parser.error("--o3-fetch-buffer-bytes must divide and not exceed "
                     "--cache-line-size")
    if args.o3_phys_int_regs <= 42:
        parser.error("--o3-phys-int-regs must exceed the ARM architectural "
                     "minimum of 42")
    if args.o3_phys_vec_regs <= 44:
        parser.error("--o3-phys-vec-regs must exceed the ARM architectural "
                     "minimum of 44")
    if args.o3_phys_vec_pred_regs <= 18:
        parser.error("--o3-phys-vec-pred-regs must exceed the ARM "
                     "architectural minimum of 18")
    if args.o3_phys_mat_regs <= 1:
        parser.error("--o3-phys-mat-regs must exceed the ARM architectural "
                     "minimum of 1")

    if min(args.io_bus_frontend_latency, args.io_bus_forward_latency,
           args.io_bus_response_latency, args.io_bus_header_latency) < 0:
        parser.error("I/O bus latencies must be non-negative")
    if min(args.coherent_bus_frontend_latency,
           args.coherent_bus_forward_latency,
           args.coherent_bus_response_latency,
           args.coherent_bus_snoop_response_latency,
           args.coherent_bus_header_latency) < 0:
        parser.error("coherent bus latencies must be non-negative")
    if min(args.memory_bus_frontend_latency,
           args.memory_bus_forward_latency,
           args.memory_bus_response_latency,
           args.memory_bus_snoop_response_latency,
           args.memory_bus_header_latency) < 0:
        parser.error("memory bus latencies must be non-negative")
    if args.mem_write_low_thresh >= args.mem_write_high_thresh:
        parser.error("--mem-write-low-thresh must be smaller than "
                     "--mem-write-high-thresh")
    if args.mem_channels <= 0 or args.mem_channels & (args.mem_channels - 1):
        parser.error("--mem-channels must be a positive power of two")
    if args.mem_ranks is not None and (args.mem_ranks <= 0 or
            args.mem_ranks & (args.mem_ranks - 1)):
        parser.error("--mem-ranks must be a positive power of two")
    if args.mem_channels_intlv & (args.mem_channels_intlv - 1):
        parser.error("--mem-channels-intlv must be a power of two")
    if args.mem_channels_intlv < args.cache_line_size:
        parser.error("--mem-channels-intlv must be at least one cache line")
    cache_enabled = args.cpu in (
        "atomic_cache", "timing", "timing_full", "o3", "server_o3",
        "kvm", "kvm_server_o3"
    )
    if cache_enabled:
        cache_geometries = [
            ("L1I", args.l1i_size, args.l1i_assoc),
            ("L1D", args.l1d_size, args.l1d_assoc),
            ("L2", args.l2_size, args.l2_assoc),
            ("L3", args.l3_size, args.l3_assoc),
            ("IOCache", args.io_cache_size, args.io_cache_assoc),
        ]
        for label, size, assoc in cache_geometries:
            cache_bytes = int(Addr(size))
            set_span = args.cache_line_size * assoc
            if cache_bytes < set_span or cache_bytes % set_span:
                parser.error(f"{label} size must be an integral number of "
                             "sets for its line size and associativity")
            sets = cache_bytes // set_span
            if sets & (sets - 1):
                parser.error(f"{label} must contain a power-of-two number "
                             "of sets")
        if int(Addr(args.core_bus_snoop_filter_capacity)) < \
                int(Addr(args.l1i_size)) + int(Addr(args.l1d_size)):
            parser.error("--core-bus-snoop-filter-capacity is smaller than "
                         "one core's private L1 footprint")
        if args.last_cache_level >= 3 and \
                int(Addr(args.l3_bus_snoop_filter_capacity)) < \
                args.num_cpus * int(Addr(args.l2_size)):
            parser.error("--l3-bus-snoop-filter-capacity is smaller than "
                         "the aggregate private-L2 footprint")
        if args.last_cache_level == 3:
            directly_snooped_cache_bytes = (
                int(Addr(args.l3_size)) + int(Addr(args.io_cache_size))
            )
        elif args.last_cache_level == 2:
            directly_snooped_cache_bytes = (
                args.num_cpus * int(Addr(args.l2_size)) +
                int(Addr(args.io_cache_size))
            )
        else:
            directly_snooped_cache_bytes = (
                args.num_cpus * (int(Addr(args.l1i_size)) +
                                 int(Addr(args.l1d_size))) +
                int(Addr(args.io_cache_size))
            )
        if int(Addr(args.membus_snoop_filter_capacity)) < \
                directly_snooped_cache_bytes:
            parser.error("--membus-snoop-filter-capacity is smaller than "
                         "the aggregate cache footprint directly visible "
                         "to the system bus")

    root = Root(full_system=True)
    root.system = create(args)
    root.system.init_param = 0
    # `m5 readfile` (pseudo_inst) reads system.readfile — used to hand a per-restore
    # test command to the guest init so one checkpoint serves many experiments.
    root.system.readfile = args.script or ""

    if args.restore_from:
        print(f"[arm64-fs] restoring from {args.restore_from}")
        m5.instantiate(args.restore_from)
    else:
        m5.instantiate()
    kvm_max_run_slice_ticks = None
    if args.cpu in ("kvm", "kvm_server_o3"):
        try:
            kvm_max_run_slice_seconds = anyToLatency(args.kvm_max_run_slice)
        except (TypeError, ValueError) as error:
            parser.error(f"invalid --kvm-max-run-slice: {error}")
        if kvm_max_run_slice_seconds <= 0:
            parser.error("--kvm-max-run-slice must be positive")
        kvm_max_run_slice_ticks = m5.ticks.fromSeconds(
            kvm_max_run_slice_seconds
        )
        if kvm_max_run_slice_ticks <= 0:
            parser.error("--kvm-max-run-slice is below one gem5 tick")
        print("[arm64-fs] KVM maximum run slice: "
              f"{args.kvm_max_run_slice} ({kvm_max_run_slice_ticks} ticks)")
    print(f"[arm64-fs] booting kernel={args.kernel}")
    # Re-simulate loop: when the guest triggers an m5 checkpoint, write it to the
    # outdir and continue. This lets us snapshot after the stack loads, then restore
    # per-experiment (--restore-from) to skip the boot.
    switched_to_o3 = False
    while True:
        # Adapter synchronization runs continuously in the C++ event queue.
        # Old guest images may still contain dist-toggle-sync instrumentation;
        # it is a compatibility no-op and never changes the adapter state.
        event = (m5.simulate(kvm_max_run_slice_ticks)
                 if kvm_max_run_slice_ticks is not None else m5.simulate())
        cause = event.getCause()
        if kvm_max_run_slice_ticks is not None and \
                cause == "simulate() limit reached":
            continue
        print(f"[arm64-fs] exited @ tick {m5.curTick()} because {cause}")
        if cause == "ubsim lifecycle sync fence":
            if not hasattr(root.system, "external_udma"):
                raise RuntimeError(
                    "lifecycle synchronization requires the external UDMA adapter"
                )
            root.system.external_udma.toggleLifecycleSync()
            print("[arm64-fs] modular lifecycle synchronization toggled")
            continue
        if "checkpoint" in cause.lower():
            if args.cpu in ("kvm", "kvm_server_o3"):
                raise RuntimeError(
                    "checkpoints are not supported by the KVM/O3 switch configuration"
                )
            if switched_to_o3:
                raise RuntimeError(
                    "server_o3 checkpoints are supported only before the "
                    "AtomicSimpleCPU-to-ArmO3CPU switch"
                )
            cdir = os.path.join(m5.options.outdir, "cpt")
            m5.checkpoint(cdir)
            print(f"[single_node_fs_clean] checkpoint written to {cdir}")
            if os.environ.get("UBSIM_EXIT_AFTER_CHECKPOINT", "0") == "1":
                print("[single_node_fs_clean] stopping at coordinated checkpoint")
                break
            continue
        if cause == "switchcpu" and args.cpu in ("server_o3", "kvm_server_o3"):
            boot_name = ("KVM" if args.cpu == "kvm_server_o3" else "Atomic")
            switch_direction = (f"O3-to-{boot_name}" if switched_to_o3
                                else f"{boot_name}-to-O3")
            print(f"[arm64-fs] switching CPUs at drained boundary: {switch_direction}")
            if switched_to_o3:
                print("[arm64-fs] switching all ArmO3 CPUs back to boot CPUs")
                reverse_pairs = [
                    (new_cpu, old_cpu)
                    for old_cpu, new_cpu in root.system._switch_pairs
                ]
                m5.switchCpus(root.system, reverse_pairs)
                switched_to_o3 = False
                boot_name = ("ArmV8KvmCPU" if args.cpu == "kvm_server_o3"
                             else "AtomicSimpleCPU")
                print(f"[arm64-fs] {boot_name} fast mode active")
            else:
                print("[arm64-fs] switching all boot CPUs to ArmO3CPU")
                m5.switchCpus(root.system, root.system._switch_pairs)
                switched_to_o3 = True
                print("[arm64-fs] ArmO3CPU server ROI model active")
            continue
        break


if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
