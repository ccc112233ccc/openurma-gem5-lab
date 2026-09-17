/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef OPENURMA_GEM5_M5OPS_H
#define OPENURMA_GEM5_M5OPS_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <gem5/m5ops.h>
#include "m5_mmap.h"

/*
 * gem5-native CPUs recognize the traditional magic instruction, while KVM
 * does not.  KVM exits on an access to System::m5ops_base instead, allowing
 * gem5 to execute the same pseudo operation.  Select the transport at run
 * time so one initramfs works with both CPU models.
 */
static inline int
ou_m5ops_address_mode(void)
{
    const char *mode = getenv("OPENURMA_M5OPS_MODE");

    if (mode == NULL || mode[0] == '\0' || strcmp(mode, "inst") == 0) {
        return 0;
    }
    if (strcmp(mode, "addr") == 0) {
        return 1;
    }
    (void)fprintf(stderr,
        "Unsupported OPENURMA_M5OPS_MODE='%s' (expected inst or addr).\n",
        mode);
    return -1;
}

static inline int
ou_m5ops_prepare_address(void)
{
    static int mapped;
    const char *value;
    char *end = NULL;
    unsigned long long parsed;

    if (mapped != 0) {
        return 0;
    }

    value = getenv("OPENURMA_M5OPS_BASE");
    if (value == NULL || value[0] == '\0') {
        (void)fprintf(stderr,
            "OPENURMA_M5OPS_BASE is required in address mode.\n");
        return -1;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
        (parsed & 0xffffULL) != 0 || parsed > UINT64_MAX - 0xffffULL) {
        (void)fprintf(stderr,
            "Invalid OPENURMA_M5OPS_BASE='%s' (must be a non-zero, "
            "64-KiB-aligned physical address).\n", value);
        return -1;
    }

    m5op_addr = (uint64_t)parsed;
    map_m5_mem();
    if (m5_mem == MAP_FAILED) {
        (void)fprintf(stderr, "Failed to map the gem5 m5ops MMIO range: %s.\n",
            strerror(errno));
        m5_mem = NULL;
        return -1;
    }
    mapped = 1;
    return 0;
}

static inline int
ou_m5ops_dist_toggle_sync(void)
{
    int address_mode = ou_m5ops_address_mode();

    if (address_mode < 0) {
        return -1;
    }
    if (address_mode != 0) {
        if (ou_m5ops_prepare_address() != 0) {
            return -1;
        }
        m5_dist_toggle_sync_addr();
    } else {
        m5_dist_toggle_sync();
    }
    return 0;
}

static inline int
ou_m5ops_switch_cpu(void)
{
    int address_mode = ou_m5ops_address_mode();

    if (address_mode < 0) {
        return -1;
    }
    if (address_mode != 0) {
        if (ou_m5ops_prepare_address() != 0) {
            return -1;
        }
        m5_switch_cpu_addr();
    } else {
        m5_switch_cpu();
    }
    return 0;
}

static inline int
ou_m5ops_reset_stats(void)
{
    int address_mode = ou_m5ops_address_mode();

    if (address_mode < 0) {
        return -1;
    }
    if (address_mode != 0) {
        if (ou_m5ops_prepare_address() != 0) {
            return -1;
        }
        m5_reset_stats_addr(0, 0);
    } else {
        m5_reset_stats(0, 0);
    }
    return 0;
}

static inline int
ou_m5ops_dump_stats(void)
{
    int address_mode = ou_m5ops_address_mode();

    if (address_mode < 0) {
        return -1;
    }
    if (address_mode != 0) {
        if (ou_m5ops_prepare_address() != 0) {
            return -1;
        }
        m5_dump_stats_addr(0, 0);
    } else {
        m5_dump_stats(0, 0);
    }
    return 0;
}

#endif /* OPENURMA_GEM5_M5OPS_H */
