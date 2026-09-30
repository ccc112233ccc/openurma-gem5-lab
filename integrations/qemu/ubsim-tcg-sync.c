/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "accel/tcg/cpu-loop.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "hw/misc/ubsim-tcg-sync.h"

static __thread uintptr_t ubsim_tcg_retaddr;

void ubsim_tcg_set_retaddr(uintptr_t retaddr)
{
    ubsim_tcg_retaddr = retaddr;
}

G_NORETURN void ubsim_tcg_suspend_cpu(CPUState *cpu)
{
    uintptr_t retaddr = ubsim_tcg_retaddr;

    g_assert(cpu != NULL);
    cpu->halted = 1;
    cpu->exception_index = EXCP_HALTED;
    if (retaddr != 0) {
        cpu_loop_exit_restore(cpu, retaddr);
    }
    cpu_loop_exit(cpu);
}

void ubsim_tcg_resume_cpu(CPUState *cpu)
{
    g_assert(cpu != NULL);
    cpu->halted = 0;
}
