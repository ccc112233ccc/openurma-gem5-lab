# Fast profile without a checkpoint (2026-10-21)

The JSON workbench now treats `execution.checkpoint=auto` as restore if a
compatible gem5 checkpoint exists, otherwise cold boot. `checkpoint=none`
always cold boots. The availability endpoint returns the selected launch mode
and effective host CPU, cache, memory, and guest kernel details.

Validation used two gem5 hosts, each with the Fast profile, 4 GB modeled
memory, required ROI synchronization, one 400 Gbit/s UB port and the existing
375 ns links plus 500 ns switch. No compatible Fast checkpoint was available.
The launch started two independent `AtomicSimpleCPU` guests from the 64 KiB
OLK 6.6 kernel and official initramfs, with no `--restore-from` argument or
UDMA `--state-in` file. `./lab sync` completed for both guests: both UARTs
returned a shell, the architecture timer check passed, and both had unique
active OOB addresses. Elapsed time from launch to completed sync was about
two and a half minutes. The guest simulation was stopped after validation.

The workbench's existing three-host Fast task was inspected in the browser.
Its launch button was enabled, the status stated that it would cold boot, and
the host inspector showed the Fast model's 1 CPU at 3 GHz, L1I 48 kB, L1D
32 kB, L2 1 MB, DDR3-1600 controller, 4 GB modeled memory / 1 GB guest
limit, and Linux 6.6 with 64 KiB pages. A three-host cold boot was not run
because the two-host test covered the fallback path and the host has 16 GB
total memory. The launch guard remains responsible for admission and live
memory pressure protection.

This validates launch and shell readiness, not URMA benchmark accuracy in
Fast mode. The built-in workbench performance recipe still requires the
Modern ArmO3 profile and synchronized ROI timing.
