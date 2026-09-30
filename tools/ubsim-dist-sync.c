/* SPDX-License-Identifier: BSD-3-Clause */
#include "ubsim-m5ops.h"

int
main(void)
{
    return ubsim_m5ops_dist_toggle_sync() == 0 ? 0 : 1;
}
