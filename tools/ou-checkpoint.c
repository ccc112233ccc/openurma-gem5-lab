/* SPDX-License-Identifier: BSD-3-Clause */
#include "ou-m5ops.h"

int
main(void)
{
    return ou_m5ops_checkpoint() == 0 ? 0 : 1;
}
