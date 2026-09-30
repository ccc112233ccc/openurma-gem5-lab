/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_UBSIM_UB_HOST_H
#define HW_MISC_UBSIM_UB_HOST_H

#include "hw/core/sysbus.h"

#define TYPE_UBSIM_UB_HOST "ubsim-ub-host"

void ubsim_ub_host_create(const char *socket_path, hwaddr mmio_base,
                             hwaddr mmio_size, DeviceState *gic,
                             unsigned irq_base);

#endif
