/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_UBSIM_TCG_SYNC_H
#define HW_MISC_UBSIM_TCG_SYNC_H

#include "qemu/osdep.h"

typedef struct CPUState CPUState;

/* The generic TCG MMIO path records the host return address around every
 * dispatch.  A timing device can then unwind the current translated block,
 * leave the guest instruction uncommitted, and retry it after its simulated
 * completion becomes visible. */
void ubsim_tcg_set_retaddr(uintptr_t retaddr);
G_NORETURN void ubsim_tcg_suspend_cpu(CPUState *cpu);
void ubsim_tcg_resume_cpu(CPUState *cpu);

#endif
