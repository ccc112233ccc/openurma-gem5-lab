"""Local listener allocation for a QEMU topology."""

from __future__ import annotations

import os


def qemu_ports(count: int) -> dict[str, list[int]]:
    if not 2 <= count <= 8:
        raise ValueError("QEMU supports 2–8 hosts")
    uart0 = int(os.environ.get("UBSIM_QEMU_UART0", "3560"))
    uart1 = int(os.environ.get("UBSIM_QEMU_UART1", "3570"))
    ssh0 = int(os.environ.get("UBSIM_QEMU_SSH0_PORT", "2220"))
    ssh1 = int(os.environ.get("UBSIM_QEMU_SSH1_PORT", "2221"))
    oob = int(os.environ.get("UBSIM_QEMU_OOB_PORT", "4560"))
    if uart1 <= uart0 or ssh1 <= ssh0:
        raise ValueError("QEMU UART and SSH second ports must exceed first ports")
    ports = {
        "uart": [uart0 + i * (uart1 - uart0) for i in range(count)],
        "ssh": [ssh0 + i * (ssh1 - ssh0) for i in range(count)],
        "oob": [oob + i for i in range(count)] if count > 2 else [oob],
    }
    flat = sum(ports.values(), [])
    if any(not 1024 <= port <= 65535 for port in flat) or len(set(flat)) != len(flat):
        raise ValueError("QEMU local ports must be unique and within 1024–65535")
    return ports
