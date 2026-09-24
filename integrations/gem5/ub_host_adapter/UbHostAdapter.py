# SPDX-License-Identifier: Apache-2.0
from m5.objects.Device import DmaDevice
from m5.objects.Gic import ArmInterruptPin
from m5.params import NULL, Param


class UbHostAdapter(DmaDevice):
    """Thin gem5 side of UB-HOST.

    UDMA semantics live in the external device process. This object only
    forwards PIO, performs guest-memory DMA, and drives an interrupt pin.
    """

    type = "UbHostAdapter"
    cxx_class = "gem5::UbHostAdapter"
    cxx_header = "ub_host_adapter/UbHostAdapter.hh"

    pio_addr = Param.Addr("Device MMIO base")
    pio_size = Param.Addr(0x10000, "Device MMIO aperture size")
    pio_latency = Param.Latency("100ns", "PIO response latency")
    socket_path = Param.String("UB-HOST SimBricks socket")
    poll_interval = Param.Latency("1us", "Idle device-message poll interval")
    interrupt = Param.ArmInterruptPin(NULL, "Device interrupt output")
