/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENURMA_QEMU_UB_HOST_PROTO_H
#define OPENURMA_QEMU_UB_HOST_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "simbricks/base/generic.h"

#define OPENURMA_UB_HOST_PROTOCOL_ID UINT64_C(0x5542484f53540001)
#define OPENURMA_UB_HOST_VERSION 1
#define OPENURMA_UB_HOST_MAX_REGIONS 8
#define OPENURMA_UB_HOST_MAX_IRQS 64

enum OpenUrmaUbHostH2DType {
    OPENURMA_H2D_MMIO_READ = 0x40,
    OPENURMA_H2D_MMIO_WRITE = 0x41,
    OPENURMA_H2D_DMA_READ_COMPLETION = 0x42,
    OPENURMA_H2D_DMA_WRITE_COMPLETION = 0x43,
    OPENURMA_H2D_DEVICE_CONTROL = 0x44,
    OPENURMA_H2D_LIFECYCLE = 0x45,
};

enum OpenUrmaUbHostD2HType {
    OPENURMA_D2H_MMIO_COMPLETION = 0x40,
    OPENURMA_D2H_DMA_READ = 0x41,
    OPENURMA_D2H_DMA_WRITE = 0x42,
    OPENURMA_D2H_INTERRUPT = 0x43,
    OPENURMA_D2H_LIFECYCLE = 0x44,
};

enum OpenUrmaUbHostAddressKind {
    OPENURMA_ADDRESS_GUEST_PHYSICAL = 0,
    OPENURMA_ADDRESS_IO_VIRTUAL = 1,
    OPENURMA_ADDRESS_MSI = 2,
};

enum OpenUrmaUbHostInterruptAction {
    OPENURMA_INTERRUPT_LOWER = 0,
    OPENURMA_INTERRUPT_RAISE = 1,
    OPENURMA_INTERRUPT_PULSE = 2,
};

struct OpenUrmaUbHostDeviceIntro {
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
    } regions[OPENURMA_UB_HOST_MAX_REGIONS];
};

struct OpenUrmaUbHostIntro {
    uint32_t version;
    uint32_t address_bits;
    uint64_t feature_bits;
    uint64_t mmio_base;
    uint64_t mmio_size;
};

struct OpenUrmaUbHostMmioRequest {
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

struct OpenUrmaUbHostCompletion {
    uint64_t request_id;
    uint64_t value;
    uint32_t length;
    uint16_t status;
    uint8_t reserved[26];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

struct OpenUrmaUbHostDmaRequest {
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

struct OpenUrmaUbHostInterrupt {
    uint64_t sequence;
    uint32_t vector;
    uint8_t action;
    uint8_t reserved[35];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

struct OpenUrmaUbHostLifecycle {
    uint64_t generation;
    uint8_t action;
    uint8_t enabled;
    uint8_t reserved[38];
    uint64_t timestamp;
    uint8_t pad[7];
    uint8_t own_type;
} __attribute__((packed));

union OpenUrmaUbHostH2DMessage {
    union SimbricksProtoBaseMsg base;
    struct OpenUrmaUbHostMmioRequest mmio;
    struct OpenUrmaUbHostCompletion completion;
    struct OpenUrmaUbHostLifecycle lifecycle;
};

union OpenUrmaUbHostD2HMessage {
    union SimbricksProtoBaseMsg base;
    struct OpenUrmaUbHostCompletion completion;
    struct OpenUrmaUbHostDmaRequest dma;
    struct OpenUrmaUbHostInterrupt interrupt;
    struct OpenUrmaUbHostLifecycle lifecycle;
};

struct OpenUrmaUbHostInterface {
    struct SimbricksBaseIf base;
};

SIMBRICKS_BASEIF_GENERIC(OpenUrmaUbHostH2D, OpenUrmaUbHostH2DMessage,
                         OpenUrmaUbHostInterface)
SIMBRICKS_BASEIF_GENERIC(OpenUrmaUbHostD2H, OpenUrmaUbHostD2HMessage,
                         OpenUrmaUbHostInterface)

static inline void openurma_ub_host_default_params(
    struct SimbricksBaseIfParams *params)
{
    SimbricksBaseIfDefaultParams(params);
    params->upper_layer_proto = OPENURMA_UB_HOST_PROTOCOL_ID;
    params->in_entries_size = 8192 + 64;
    params->out_entries_size = 8192 + 64;
}

_Static_assert(sizeof(struct OpenUrmaUbHostMmioRequest) == 64,
               "UB-HOST MMIO ABI mismatch");
_Static_assert(sizeof(struct OpenUrmaUbHostDmaRequest) == 64,
               "UB-HOST DMA ABI mismatch");
_Static_assert(sizeof(union OpenUrmaUbHostH2DMessage) == 64,
               "UB-HOST H2D ABI mismatch");
_Static_assert(sizeof(union OpenUrmaUbHostD2HMessage) == 64,
               "UB-HOST D2H ABI mismatch");

#endif
