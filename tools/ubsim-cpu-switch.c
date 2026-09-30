/* SPDX-License-Identifier: BSD-3-Clause */
#include "ubsim-m5ops.h"

int
main(void)
{
    return ubsim_m5ops_switch_cpu() == 0 ? 0 : 1;
}
