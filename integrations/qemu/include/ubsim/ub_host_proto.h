/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UBSIM_QEMU_UB_HOST_PROTO_H
#define UBSIM_QEMU_UB_HOST_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "simbricks/base/generic.h"

#define UBSIM_UB_HOST_PROTOCOL_ID UINT64_C(0x5542484f53540001)
#define UBSIM_UB_HOST_VERSION 1
#define UBSIM_UB_HOST_MAX_REGIONS 8
#define UBSIM_UB_HOST_MAX_IRQS 64

enum UbSimUbHostH2DType {
    UBSIM_H2D_MMIO_READ = 0x40,
    UBSIM_H2D_MMIO_WRITE = 0x41,
    UBSIM_H2D_DMA_READ_COMPLETION = 0x42,
    UBSIM_H2D_DMA_WRITE_COMPLETION = 0x43,
    UBSIM_H2D_DEVICE_CONTROL = 0x44,
    UBSIM_H2D_LIFECYCLE = 0x45,
};

enum UbSimUbHostD2HType {
    UBSIM_D2H_MMIO_COMPLETION = 0x40,
    UBSIM_D2H_DMA_READ = 0x41,
    UBSIM_D2H_DMA_WRITE = 0x42,
    UBSIM_D2H_INTERRUPT = 0x43,
    UBSIM_D2H_LIFECYCLE = 0x44,
};

enum UbSimUbHostAddressKind {
    UBSIM_ADDRESS_GUEST_PHYSICAL = 0,
    UBSIM_ADDRESS_IO_VIRTUAL = 1,
    UBSIM_ADDRESS_MSI = 2,
};

enum UbSimUbHostInterruptAction {
    UBSIM_INTERRUPT_LOWER = 0,
    UBSIM_INTERRUPT_RAISE = 1,
    UBSIM_INTERRUPT_PULSE = 2,
};

struct UbSimUbHostDeviceIntro {
    uint32_t version;
    uint32_t region_count;
    uint32_t irq_count;
    uint32_t port_count;
    uint64_t device_id;
    uint64_t feature_bits;
    struct {
        uint64_t size;
        uint32_t type;
        uint32_t flags;
    } regions[UBSIM_UB_HOST_MAX_REGIONS];
};

struct UbSimUbHostIntro {
    uint32_t version;
    uint32_t address_bits;
    uint64_t feature_bits;
    uint64_t mmio_base;
    uint64_t mmio_size;
};

struct UbSimUbHostMmioRequest {
    uint64_t request_id;
    uint64_t offset;
    uint64_t value;
    uint32_t length;
    uint16_t region;
    uint16_t byte_enable;
    uint8_t reserved[16];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

struct UbSimUbHostCompletion {
    uint64_t request_id;
    uint64_t value;
    uint32_t length;
    uint16_t status;
    uint8_t reserved[26];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

struct UbSimUbHostDmaRequest {
    uint64_t request_id;
    uint64_t address;
    uint32_t length;
    uint32_t pasid;
    uint16_t flags;
    uint8_t address_kind;
    uint8_t reserved0;
    uint8_t reserved[20];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

struct UbSimUbHostInterrupt {
    uint64_t sequence;
    uint32_t vector;
    uint8_t action;
    uint8_t reserved[35];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

struct UbSimUbHostLifecycle {
    uint64_t generation;
    uint8_t action;
    uint8_t enabled;
    uint8_t reserved[38];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

union UbSimUbHostH2DMessage {
    union SimbricksProtoBaseMsg base;
    struct UbSimUbHostMmioRequest mmio;
    struct UbSimUbHostCompletion completion;
    struct UbSimUbHostLifecycle lifecycle;
};

union UbSimUbHostD2HMessage {
    union SimbricksProtoBaseMsg base;
    struct UbSimUbHostCompletion completion;
    struct UbSimUbHostDmaRequest dma;
    struct UbSimUbHostInterrupt interrupt;
    struct UbSimUbHostLifecycle lifecycle;
};

struct UbSimUbHostInterface {
    struct SimbricksBaseIf base;
};

SIMBRICKS_BASEIF_GENERIC(UbSimUbHostH2D, UbSimUbHostH2DMessage,
                         UbSimUbHostInterface)
SIMBRICKS_BASEIF_GENERIC(UbSimUbHostD2H, UbSimUbHostD2HMessage,
                         UbSimUbHostInterface)

static inline void ubsim_ub_host_default_params(
    struct SimbricksBaseIfParams *params)
{
    SimbricksBaseIfDefaultParams(params);
    params->upper_layer_proto = UBSIM_UB_HOST_PROTOCOL_ID;
    params->in_entries_size = 8192 + 64;
    params->out_entries_size = 8192 + 64;
}

_Static_assert(sizeof(struct UbSimUbHostMmioRequest) == 64,
               "UB-HOST MMIO ABI mismatch");
_Static_assert(sizeof(struct UbSimUbHostDmaRequest) == 64,
               "UB-HOST DMA ABI mismatch");
_Static_assert(sizeof(union UbSimUbHostH2DMessage) == 64,
               "UB-HOST H2D ABI mismatch");
_Static_assert(sizeof(union UbSimUbHostD2HMessage) == 64,
               "UB-HOST D2H ABI mismatch");

#endif
